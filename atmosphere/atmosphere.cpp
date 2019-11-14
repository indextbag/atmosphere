
//--------------------------------------------------------------------------------
//
//	Redistribution and use in source and binary forms, with or without
//	modification, are permitted provided that the following conditions are met :
//
//	*Redistributions of source code must retain the above copyright notice, this
//	list of conditions and the following disclaimer.
//
//	* Redistributions in binary form must reproduce the above copyright notice,
//	this list of conditions and the following disclaimer in the documentation
//	and/or other materials provided with the distribution.
//	
//	* Neither the name of the copyright holder nor the names of its
//	contributors may be used to endorse or promote products derived from
//	this software without specific prior written permission.
//	
//	THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
//	AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
//	IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
//	DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
//	FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
//	DAMAGES(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
//	SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
//	CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
//	OR TORT(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
//	OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// 
// Copyright(c) 2019, Sergen Eren
// All rights reserved.
//----------------------------------------------------------------------------------
// 
//	Version 1.0: Sergen Eren, 12/11/2019
//
// File: This is the implementation file for atmosphere class functions 
//
//-----------------------------------------------


#include <vector>
#include <string>

#include "atmosphere/atmosphere.h"
#include "atmosphere/constants.h"

#include "helper_math.h"


// Functions that hold the texture calculation kernels from atmosphere_kernels.ptx file
atmosphere_error_t atmosphere::init_functions(CUmodule &cuda_module) {

	CUresult error;
	error = cuModuleGetFunction(transmittance_function, cuda_module, "calculate_transmittance");
	if (error != CUDA_SUCCESS) return ATMO_INIT_FUNC_ERR;
	
	error = cuModuleGetFunction(direct_irradiance_function, cuda_module, "calculate_direct_irradiance");
	if (error != CUDA_SUCCESS) return ATMO_INIT_FUNC_ERR;
	
	error = cuModuleGetFunction(indirect_irradiance_function, cuda_module, "calculate_indirect_irradiance");
	if (error != CUDA_SUCCESS) return ATMO_INIT_FUNC_ERR;
	
	error = cuModuleGetFunction(multiple_scattering_function, cuda_module, "calculate_multiple_scattering");
	if (error != CUDA_SUCCESS) return ATMO_INIT_FUNC_ERR;
	
	error = cuModuleGetFunction(scattering_density_function, cuda_module, "calculate__scattering_density");
	if (error != CUDA_SUCCESS) return ATMO_INIT_FUNC_ERR;
	
	error = cuModuleGetFunction(single_scattering_function, cuda_module, "calculate_single_scattering");
	if (error != CUDA_SUCCESS) return ATMO_INIT_FUNC_ERR;
	
	
	return ATMO_NO_ERR;

}


double atmosphere::coeff(double lambda, int component) {

	double x = cie_color_matching_function_table_value(lambda, 1);
	double y = cie_color_matching_function_table_value(lambda, 2);
	double z = cie_color_matching_function_table_value(lambda, 3);
	double sRGB = XYZ_TO_SRGB[component * 3 + 0] * x + XYZ_TO_SRGB[component * 3 + 1] * y + XYZ_TO_SRGB[component * 3 + 2] * z;

	return sRGB;
	   
}

void atmosphere::sky_sun_radiance_to_luminance(float3& sky_spectral_radiance_to_luminance, float3& sun_spectral_radiance_to_luminance) {

	bool precompute_illuminance = num_precomputed_wavelengths() > 3;
	double sky_k_r, sky_k_g, sky_k_b;

	if (precompute_illuminance)
		sky_k_r = sky_k_g = sky_k_b = static_cast<double>(MAX_LUMINOUS_EFFICACY);
	else
		compute_spectral_radiance_to_luminance_factors(m_wave_lengths, m_solar_irradiance, -3, sky_k_r, sky_k_g, sky_k_b);

	// Compute the values for the SUN_RADIANCE_TO_LUMINANCE constant.
	double sun_k_r, sun_k_g, sun_k_b;
	compute_spectral_radiance_to_luminance_factors(m_wave_lengths, m_solar_irradiance, 0, sun_k_r, sun_k_g, sun_k_b);

	sky_spectral_radiance_to_luminance = make_float3((float)sky_k_r, (float)sky_k_g, (float)sky_k_b);
	sun_spectral_radiance_to_luminance = make_float3((float)sun_k_r, (float)sun_k_g, (float)sun_k_b);
	   
}

double atmosphere::cie_color_matching_function_table_value(double wavelength, int column) {

	if (wavelength <= kLambdaMin || wavelength >= kLambdaMax)
		return 0.0;

	double u = (wavelength - kLambdaMin) / 5.0;
	int row = (int)floor(u);

	u -= row;
	return CIE_2_DEG_COLOR_MATCHING_FUNCTIONS[4 * row + column] * (1.0 - u) + CIE_2_DEG_COLOR_MATCHING_FUNCTIONS[4 * (row + 1) + column] * u;

}

double atmosphere::interpolate(const std::vector<double>& wavelengths, const std::vector<double>& wavelength_function, double wavelength)
{
	if (wavelength < wavelengths[0])
		return wavelength_function[0];

	for (int i = 0; i < wavelengths.size() - 1; ++i)
	{
		if (wavelength < wavelengths[i + 1])
		{
			double u = (wavelength - wavelengths[i]) / (wavelengths[i + 1] - wavelengths[i]);
			return wavelength_function[i] * (1.0 - u) + wavelength_function[i + 1] * u;
		}
	}

	return wavelength_function[wavelength_function.size() - 1];
}

void atmosphere::compute_spectral_radiance_to_luminance_factors(const std::vector<double>& wavelengths, const std::vector<double>& solar_irradiance, double lambda_power, double & k_r, double & k_g, double & k_b)
{
	k_r = 0.0;
	k_g = 0.0;
	k_b = 0.0;
	double solar_r = interpolate(wavelengths, solar_irradiance, kLambdaR);
	double solar_g = interpolate(wavelengths, solar_irradiance, kLambdaG);
	double solar_b = interpolate(wavelengths, solar_irradiance, kLambdaB);
	int dlambda = 1;

	for (int lambda = kLambdaMin; lambda < kLambdaMax; lambda += dlambda)
	{
		double x_bar = cie_color_matching_function_table_value(lambda, 1);
		double y_bar = cie_color_matching_function_table_value(lambda, 2);
		double z_bar = cie_color_matching_function_table_value(lambda, 3);

		const double* xyz2srgb = &XYZ_TO_SRGB[0];
		double r_bar = xyz2srgb[0] * x_bar + xyz2srgb[1] * y_bar + xyz2srgb[2] * z_bar;
		double g_bar = xyz2srgb[3] * x_bar + xyz2srgb[4] * y_bar + xyz2srgb[5] * z_bar;
		double b_bar = xyz2srgb[6] * x_bar + xyz2srgb[7] * y_bar + xyz2srgb[8] * z_bar;
		double irradiance = interpolate(wavelengths, solar_irradiance, lambda);

		k_r += r_bar * irradiance / solar_r * pow(lambda / kLambdaR, lambda_power);
		k_g += g_bar * irradiance / solar_g * pow(lambda / kLambdaG, lambda_power);
		k_b += b_bar * irradiance / solar_b * pow(lambda / kLambdaB, lambda_power);
	}

	k_r *= static_cast<double>(MAX_LUMINOUS_EFFICACY) * dlambda;
	k_g *= static_cast<double>(MAX_LUMINOUS_EFFICACY) * dlambda;
	k_b *= static_cast<double>(MAX_LUMINOUS_EFFICACY) * dlambda;

}

// Precomputes the textures that will be sent to the render kernel
atmosphere_error_t atmosphere::precompute(TextureBuffer* buffer, double* lambdas, double* luminance_from_radiance, bool blend, int num_scattering_orders) {

	printf("in precompute");

	return ATMO_NO_ERR;

}


// Initialization function that fills the atmosphere parameters 
atmosphere_error_t atmosphere::init(bool use_constant_solar_spectrum_, bool use_ozone_) {

	constexpr double kPi = 3.1415926;
	
	constexpr double kSolarIrradiance[48] = {
	  1.11776, 1.14259, 1.01249, 1.14716, 1.72765, 1.73054, 1.6887, 1.61253,
	  1.91198, 2.03474, 2.02042, 2.02212, 1.93377, 1.95809, 1.91686, 1.8298,
	  1.8685, 1.8931, 1.85149, 1.8504, 1.8341, 1.8345, 1.8147, 1.78158, 1.7533,
	  1.6965, 1.68194, 1.64654, 1.6048, 1.52143, 1.55622, 1.5113, 1.474, 1.4482,
	  1.41018, 1.36775, 1.34188, 1.31429, 1.28303, 1.26758, 1.2367, 1.2082,
	  1.18737, 1.14683, 1.12362, 1.1058, 1.07124, 1.04992
	};

	constexpr double kOzoneCrossSection[48] = {
	1.18e-27, 2.182e-28, 2.818e-28, 6.636e-28, 1.527e-27, 2.763e-27, 5.52e-27,
	8.451e-27, 1.582e-26, 2.316e-26, 3.669e-26, 4.924e-26, 7.752e-26, 9.016e-26,
	1.48e-25, 1.602e-25, 2.139e-25, 2.755e-25, 3.091e-25, 3.5e-25, 4.266e-25,
	4.672e-25, 4.398e-25, 4.701e-25, 5.019e-25, 4.305e-25, 3.74e-25, 3.215e-25,
	2.662e-25, 2.238e-25, 1.852e-25, 1.473e-25, 1.209e-25, 9.423e-26, 7.455e-26,
	6.566e-26, 5.105e-26, 4.15e-26, 4.228e-26, 3.237e-26, 2.451e-26, 2.801e-26,
	2.534e-26, 1.624e-26, 1.465e-26, 2.078e-26, 1.383e-26, 7.105e-27
	};

	constexpr double kDobsonUnit = 2.687e20;
	constexpr double kMaxOzoneNumberDensity = 300.0 * kDobsonUnit / 15000.0;
	constexpr double kConstantSolarIrradiance = 1.5;
	constexpr double kTopRadius = 6420000.0;
	constexpr double kRayleigh = 1.24062e-6;
	constexpr double kRayleighScaleHeight = 8000.0f;
	constexpr double kMieScaleHeight = 1200.0f;
	constexpr double kMieAngstromAlpha = 0.0;
	constexpr double kMieAngstromBeta = 5.328e-3;
	constexpr double kMieSingleScatteringAlbedo = 0.9;
	constexpr double kGroundAlbedo = 0.1;
	
	m_absorption_density.push_back(new DensityProfileLayer(25000.0f, 0.0f, 0.0f, 1.0f / 15000.0f, -2.0f / 3.0f));
	m_absorption_density.push_back(new DensityProfileLayer(0.0f, 0.0f, 0.0f, -1.0f / 15000.0f, 8.0f / 3.0f));

	for (int l = kLambdaMin; l <= kLambdaMax; l += 10) {
		double lambda = static_cast<double>(l) * 1e-3;  // micro-meters
		double mie =
			kMieAngstromBeta / kMieScaleHeight * pow(lambda, -kMieAngstromAlpha);
		m_wave_lengths.push_back(l);
		if (use_constant_solar_spectrum_) {
			m_solar_irradiance.push_back(kConstantSolarIrradiance);
		}
		else {
			m_solar_irradiance.push_back(kSolarIrradiance[(l - kLambdaMin) / 10]);
		}
		m_rayleigh_scattering.push_back(kRayleigh * pow(lambda, -4));
		m_mie_scattering.push_back(mie * kMieSingleScatteringAlbedo);
		m_mie_extinction.push_back(mie);
		m_absorption_extinction.push_back(use_ozone_ ? kMaxOzoneNumberDensity * kOzoneCrossSection[(l - kLambdaMin) / 10] :	0.0);
		m_ground_albedo.push_back(kGroundAlbedo);
	}

	m_half_precision = false;
	m_combine_scattering_textures = true;
	m_sun_angular_radius = 0.00935 / 2.0;
	m_bottom_radius = 6360000.0f;
	m_top_radius = 6420000.0f;
	m_rayleigh_density = new DensityProfileLayer(0.0f, 1.0f, -1.0f / kRayleighScaleHeight, 0.0f, 0.0f);
	m_mie_density = new DensityProfileLayer(0.0f, 1.0f, -1.0f / kMieScaleHeight, 0.0f, 0.0f);
	m_mie_phase_function_g = 0.8;
	m_max_sun_zenith_angle = 102.0 / 180.0 * kPi;
	m_length_unit_in_meters = 1000.0;

	int num_scattering_orders = 4;

	m_texture_buffer = new TextureBuffer(m_half_precision);


	// Start precomputation

	if (num_precomputed_wavelengths() <= 3) {

		precompute(m_texture_buffer, nullptr, nullptr, false, num_scattering_orders);
	}
	else {
			   		 
		return ATMO_NO_ERR;
	}

	return ATMO_NO_ERR;

}

atmosphere::~atmosphere() {

	transmittance_function = nullptr;
	direct_irradiance_function = nullptr;
	indirect_irradiance_function = nullptr;
	multiple_scattering_function = nullptr;
	scattering_density_function = nullptr;
	single_scattering_function = nullptr;

	delete m_texture_buffer;

}

atmosphere::atmosphere() {

	transmittance_function = new CUfunction;
	direct_irradiance_function = new CUfunction;
	indirect_irradiance_function = new CUfunction;
	multiple_scattering_function = new CUfunction;
	scattering_density_function = new CUfunction;
	single_scattering_function = new CUfunction;
}