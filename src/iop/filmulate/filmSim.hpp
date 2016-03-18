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

#ifndef FILMSIM_H
#define FILMSIM_H

#include <iostream>
#include <fstream>
#include <cmath>
#include <cstdlib>
#include "jpeglib.h"
#include <setjmp.h>
#include "matrix.hpp"

//#ifdef DOUT
//#define dout cout
//#else
//#define dout 0 && cout
//#define NDEBUG
//#endif
//#include "assert.h" //Included later so NDEBUG has an effect
//
//#ifdef TOUT
//#define tout cout
//#else
//#define tout 0 && cout
//#endif

matrix<float> exposure(matrix<float> input_image, float crystals_per_pixel,
        float rolloff_boundary);

//Equalizes the concentration of developer across the reservoir and all pixels.
void agitate( matrix<float> &developerConcentration, float activeLayerThickness,
              float &reservoirDeveloperConcentration, float reservoirThickness,
              float pixelsPerMillimeter );

//This simulates one step of the development reaction.
void develop( matrix<float> &crystalRad,
              float crystalGrowthConst,
              const matrix<float> &activeCrystalsPerPixel,
              matrix<float> &silverSaltDensity,
              matrix<float> &develConcentration,
              float activeLayerThickness,
              float developerConsumptionConst,
              float silverSaltConsumptionConst,
              float timestep);

void diffuse(matrix<float> &developer_concentration,
        float sigma_const,
        float pixels_per_millimeter,
        float timestep);

void diffuse_short_convolution(matrix<float> &developer_concentration,
                               const float sigma_const,
                               const float pixels_per_millimeter,
                               const float timestep);

void layer_mix(matrix<float> &developer_concentration,
               float active_layer_thickness,
               float &reservoir_developer_concentration,
               float reservoir_thickness,
               float layer_mix_const,
               float layer_time_divisor,
               float pixels_per_millimeter,
               float timestep);

void filmulate(const float *const in,
               float *const out,
               const int c_width,
               const int c_height,
               const float rolloff_boundary,
               const float film_area,
               const float layer_mix_const,
               const int c_agitate_count);

#endif // FILMSIM_H
