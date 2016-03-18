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

matrix<float> exposure(matrix<float> input_image, float crystals_per_pixel,
        float rolloff_boundary)
{
    if (rolloff_boundary < 1.0f)
    {
        rolloff_boundary = 1.0f;
    }
    else if(rolloff_boundary > 65534.0f)
    {
        rolloff_boundary = 65534.0f;
    }
    int nrows = input_image.nr();
    int ncols = input_image.nc();
    float input;
    float crystal_headroom = 65535-rolloff_boundary;
#ifndef _OPENMP
#pragma omp parallel shared(input_image, crystals_per_pixel, rolloff_boundary,\
        nrows, ncols, crystal_headroom) private(input)
#endif
    {
#ifndef _OPENMP
#pragma omp for schedule(dynamic) nowait
#endif
    for(int row = 0; row < nrows; row++)
    {
        for(int col = 0; col<ncols; col++)
        {
            input = std::max(0.0f,input_image(row,col));
            if(input > rolloff_boundary)
                input = 65535-(crystal_headroom*crystal_headroom/
                        (input+crystal_headroom-rolloff_boundary));
            input_image(row,col)=input*crystals_per_pixel*0.00015387105;
            //Magic number mostly for historical reasons
        }
    }
    }
    return input_image;
}
