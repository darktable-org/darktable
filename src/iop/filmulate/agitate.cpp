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

void agitate ( matrix<float> &developerConcentration,
               float activeLayerThickness,
               float &reservoirDeveloperConcentration,
               float reservoirThickness,
               float pixelsPerMillimeter )
{
    int npixels = developerConcentration.nc()*
        developerConcentration.nr();
    float totalDeveloper = sum( developerConcentration ) *
        activeLayerThickness / pow( pixelsPerMillimeter, 2 ) +
        reservoirDeveloperConcentration * reservoirThickness;
    float contactLayerSize = npixels * activeLayerThickness /
        pow( pixelsPerMillimeter, 2 );
    reservoirDeveloperConcentration = totalDeveloper / ( reservoirThickness +
        contactLayerSize );
    developerConcentration = reservoirDeveloperConcentration;
    return;
}
