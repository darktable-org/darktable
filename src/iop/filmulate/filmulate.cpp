/* 
 * This file is part of Filmulator.
 *
 * Copyright 2013 Omer Mano and Carlo Vaccari
 *
 * Filmulator is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Filmulator is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Filmulator. If not, see <http://www.gnu.org/licenses/>
 */
#include "filmSim.hpp"
#include <algorithm>
#include <stdio.h>
using std::cout;
using std::endl;
#include <unistd.h>

#ifdef _OPENMP
#include <omp.h>
#endif

//Function-------------------------------------------------------------------------
void filmulate(const float *const in,
               float *const out,
               const int width_in,
               const int height_in,
               const int x_out,
               const int y_out,
               const int width_out,
               const int height_out,
               const float rolloff_boundary,
               const float film_area,
               const float layer_mix_const,
               const int agitate_count)
{

    //Magic numbers
    float initial_developer_concentration = 1.0f;
    float reservoir_thickness = 1000.0f;
    float active_layer_thickness = 0.1f;
    float crystals_per_pixel = 500.0f;
    float initial_crystal_radius = 0.00001f;
    float initial_silver_salt_density = 1.0f;
    float developer_consumption_const = 2000000.0f;
    float crystal_growth_const = 0.00001f;
    float silver_salt_consumption_const = 2000000.0f;
    float total_development_time = 100.0f;
    //int agitate_count = 1;
    int development_steps = 12;
    //float film_area = 864.0f;
    float sigma_const = 0.2f;
    //float layer_mix_const = 0.2f;
    float layer_time_divisor = 20.0f;
    //float rolloff_boundary = 51275.0f;

    const int nrows = height_in;
    const int ncols = width_in;

    //Load things into matrices for Filmulator.
    matrix<float> input_image;
    input_image.set_size(nrows, ncols*3);
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(input_image)
#endif
    for (int i = 0; i < nrows; i++)
    {
        for (int j = 0; j < ncols; j++)
        {
            input_image(i, j*3    ) = 65535.0f * in[(j + i*ncols)*4    ];
            input_image(i, j*3 + 1) = 65535.0f * in[(j + i*ncols)*4 + 1];
            input_image(i, j*3 + 2) = 65535.0f * in[(j + i*ncols)*4 + 2];
        }
    }

    int npix = nrows*ncols;

    //Now we activate some of the crystals on the film. This is literally
    //akin to exposing film to light.
    matrix<float> active_crystals_per_pixel;
    active_crystals_per_pixel = exposure(input_image, crystals_per_pixel,
            rolloff_boundary);

    //We set the crystal radius to a small seed value for each color.
    matrix<float> crystal_radius;
    crystal_radius.set_size(nrows,ncols*3);
    crystal_radius = initial_crystal_radius;

    //All layers share developer, so we only make it the original image size.
    matrix<float> developer_concentration;
    developer_concentration.set_size(nrows,ncols);
    developer_concentration = initial_developer_concentration;

    //Each layer gets its own silver salt which will feed crystal growth.
    matrix<float> silver_salt_density;
    silver_salt_density.set_size(nrows,ncols*3);
    silver_salt_density = initial_silver_salt_density;

    //Now, we set up the reservoir.
    //Because we don't want the film area to influence the brightness, we
    // increase the reservoir size in proportion.
#define FILMSIZE 864;//36x24mm
    reservoir_thickness *= film_area/FILMSIZE;
    float reservoir_developer_concentration = initial_developer_concentration;

    //This is a value used in diffuse to set the length scale.
    float pixels_per_millimeter = sqrt(npix/film_area);

    //Here we do some math for the control logic for the differential
    //equation approximation computations.
    float timestep = total_development_time/development_steps;
	int agitate_period;
	if(agitate_count > 0)
        agitate_period = floor(development_steps/agitate_count);
    else
        agitate_period = 3*development_steps;
	int half_agitate_period = floor(agitate_period/2);
   
    //Now we begin the main development/diffusion loop, which approximates the
    //differential equation of film development.
    for(int i = 0; i <= development_steps; i++)
    {
        //This is where we perform the chemical reaction part.
        //The crystals grow.
        //The developer in the active layer is consumed.
        //So is the silver salt in the film.
        // The amount consumed increases as the crystals grow larger.
        //Because the developer and silver salts are consumed in bright regions,
        // this reduces the rate at which they grow. This gives us global
        // contrast reduction.
        develop(crystal_radius,crystal_growth_const,active_crystals_per_pixel,
                silver_salt_density,developer_concentration,
                active_layer_thickness,developer_consumption_const,
                silver_salt_consumption_const,timestep);
        
        //Now, we are going to perform the diffusion part.
        //Here we mix the layer among itself, which grants us the
        // local contrast increases.
        //diffuse(developer_concentration,
        diffuse_short_convolution(developer_concentration,
                                  sigma_const,
                                  pixels_per_millimeter,
                                  timestep);

        //This performs mixing between the active layer adjacent to the film
        // and the reservoir.
        //This keeps the effects from getting too crazy.
        layer_mix(developer_concentration,
                  active_layer_thickness,
                  reservoir_developer_concentration,
                  reservoir_thickness,
                  layer_mix_const,
                  layer_time_divisor,
                  pixels_per_millimeter,
                  timestep);
        
        //I want agitation to only occur in the middle of development, not
        //at the very beginning or the ends. So, I add half the agitate
        //period to the current cycle count.
        if((i+half_agitate_period) % agitate_period ==0)
            agitate(developer_concentration, active_layer_thickness,
                    reservoir_developer_concentration, reservoir_thickness,
                    pixels_per_millimeter);
    }
    
    //Done filmulating, now do some housecleaning
    silver_salt_density.free();
    developer_concentration.free();


    //Now we compute the density (opacity) of the film.
    //We assume that overlapping crystals or dye clouds are
    //nonexistant. It works okay, for now...
    //The output is crystal_radius^2 * active_crystals_per_pixel

    matrix<float> output_density = crystal_radius % crystal_radius % active_crystals_per_pixel * 500.0f;

    //Convert back to darktable's RGBA.
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(output_density)
#endif
    for (int i = 0; i < height_out; i++)
    {
        for (int j = 0; j < width_out; j++)
        {
            out[(j + i*width_out)*4    ] = std::min(1.0f,std::max(0.0f,output_density(i + y_out, (j + x_out)*3    )));
            out[(j + i*width_out)*4 + 1] = std::min(1.0f,std::max(0.0f,output_density(i + y_out, (j + x_out)*3 + 1)));
            out[(j + i*width_out)*4 + 2] = std::min(1.0f,std::max(0.0f,output_density(i + y_out, (j + x_out)*3 + 2)));
            out[(j + i*width_out)*4 + 3] = in[(j + i*ncols)*4 + 3];//copy the alpha channel
        }
    }
}

