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
#include <math.h>
#ifdef _OPENMP
#include <omp.h>
#endif

// This function implements diffusion between the active developer layer
// adjacent to the film and the reservoir of inactive developer.

void layer_mix(matrix<float> &developer_concentration, float active_layer_thickness,
               float &reservoir_developer_concentration, float reservoir_thickness, float layer_mix_const,
               float layer_time_divisor, float pixels_per_millimeter, float timestep)
{
  int length = developer_concentration.nr();
  int width = developer_concentration.nc();

  // layer_time_divisor adjusts the ratio between the timestep used to compute
  // the diffuse within the layer and this diffuse.
  float layer_mix = pow(layer_mix_const, timestep / layer_time_divisor);

  // layer_mix is the proportion of developer that stays in the layer.

  // This gives us the amount of developer that comes from the reservoir.
  float reservoir_portion = (1 - layer_mix) * reservoir_developer_concentration;

  // This lets us count how much developer got added to the layer in total.
  double sum = 0;

// Here we add developer to the layer.
#ifdef _OPENMP
#pragma omp parallel shared(developer_concentration) firstprivate(layer_mix, reservoir_portion)\
    reduction(+ : sum)
#endif
  {
#ifdef _OPENMP
#pragma omp for schedule(dynamic) nowait
#endif
    for(int row = 0; row < length; row++)
    {
      float temp;
      for(int col = 0; col < width; col++)
      {
        temp = developer_concentration(row, col) * layer_mix + reservoir_portion;
        // Here we accumulate how much developer went into the layer.
        sum += temp - developer_concentration(row, col);
        developer_concentration(row, col) = temp;
      }
    }
  }

  // Now, we must adjust sum to ensure that the parameters
  // are orthogonal. It's sketchy, okay?
  float reservoir_concentration_change
      = sum * active_layer_thickness / (pow(pixels_per_millimeter, 2) * reservoir_thickness);

  // The reservoir thickness is not actually the reservoir thickness, but volume.
  // This is a major weirdness from when it was originally thickness on the outside
  // but we called it volume because that's what it is on the inside, like here.

  // Now, we subtract how much went into the layer from the reservoir.
  reservoir_developer_concentration -= reservoir_concentration_change;
  return;
}
