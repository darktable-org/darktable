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

void develop( matrix<float> &crystalRad,
              float crystalGrowthConst,
              const matrix<float> &activeCrystalsPerPixel,
              matrix<float> &silverSaltDensity,
              matrix<float> &develConcentration,
              float activeLayerThickness,
              float developerConsumptionConst,
              float silverSaltConsumptionConst,
              float timestep)
{

    //Setting up dimensions and boundaries.
    int height = develConcentration.nr();
    int width = develConcentration.nc();
    //We still count columns of pixels, because we must process them
    // whole, so to ensure this we runthree adjacent elements at a time.
    
    //Here we pre-compute some repeatedly used values.
    float cgc = crystalGrowthConst*timestep;
    float dcc = 2.0*developerConsumptionConst / ( activeLayerThickness*3.0 );
    float sscc = silverSaltConsumptionConst*2.0;

    //These are only used once per loop, so they don't have to be matrices.
    float dCrystalRadR;
    float dCrystalRadG;
    float dCrystalRadB;
    float dCrystalVolR;
    float dCrystalVolG;
    float dCrystalVolB;

    //These are the column indices for red, green, and blue.
    int row, col, colr, colg, colb;

#ifndef _OPENMP
#pragma omp parallel shared( develConcentration, silverSaltDensity,\
        crystalRad, activeCrystalsPerPixel, cgc, dcc, sscc )\
        private( row, col,\
        colr, colg, colb,\
        dCrystalRadR, dCrystalRadG, dCrystalRadB,\
        dCrystalVolR, dCrystalVolG, dCrystalVolB )
#endif
    {

#ifndef _OPENMP
#pragma omp for schedule( dynamic ) nowait
#endif
        for ( row = 0; row < height; row++ )
        {
            for ( col = 0; col < width; col++ )
            {
                colr = col * 3;
                colg = colr + 1;
                colb = colr + 2;
                //This is the rate of thickness accumulating on the crystals.
                dCrystalRadR = develConcentration( row, col ) * silverSaltDensity( row, colr ) * cgc;
                dCrystalRadG = develConcentration( row, col ) * silverSaltDensity( row, colg ) * cgc;
                dCrystalRadB = develConcentration( row, col ) * silverSaltDensity( row, colb ) * cgc;
    
                //The volume change is proportional to 4*pi*r^2*dr.
                //We kinda shuffled around the constants, so ignore the lack of
                //the 4 and the pi.
                //However, there are varying numbers of crystals, so we also
                //multiply by the number of crystals per pixel.
                dCrystalVolR = dCrystalRadR * crystalRad( row, colr ) * crystalRad( row, colr ) *
                        activeCrystalsPerPixel( row, colr );
                dCrystalVolG = dCrystalRadG * crystalRad( row, colg ) * crystalRad( row, colg ) *
                        activeCrystalsPerPixel( row, colg );
                dCrystalVolB = dCrystalRadB * crystalRad( row, colb ) * crystalRad( row, colb ) *
                        activeCrystalsPerPixel( row, colb );
    
                //Now we apply the new crystal radius.
                crystalRad( row, colr ) += dCrystalRadR;
                crystalRad( row, colg ) += dCrystalRadG;
                crystalRad( row, colb ) += dCrystalRadB;
    
                //Here is where we consume developer. The 3 layers of film,
                //(one per color) share the same developer.
                develConcentration( row ,col ) -= dcc * ( dCrystalVolR +
                                                          dCrystalVolG +
                                                          dCrystalVolB );

                //Prevent developer concentration from going negative.
                if ( develConcentration( row, col ) < 0 )
                {
                    develConcentration( row, col ) = 0;
                }
                //Here, silver salts are consumed in proportion to how much
                //silver was deposited on the crystals. Unlike the developer,
                //each color layer has its own separate amount in this sim.
                silverSaltDensity( row, colr ) -= sscc * dCrystalVolR;
                silverSaltDensity( row, colg ) -= sscc * dCrystalVolG;
                silverSaltDensity( row, colb ) -= sscc * dCrystalVolB;
            }
        }
    }
    return;
}
