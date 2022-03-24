/*
 *    This file is part of darktable,
 *    Copyright (C) 2016-2021 darktable developers.
 *
 *    darktable is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    darktable is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "control/control.h"
#include "views/knight_font.h"
#include "views/view.h"
#include "views/view_api.h"

DT_MODULE(1)

#pragma GCC diagnostic ignored "-Wshadow"

// tunables for how the game looks and reacts
#define ASPECT_RATIO 0.875 // the playground
#define LOOP_SPEED 50      // ms between event loop calls
#define STEP_SIZE 0.25     // factor wrt. sprite size for movement

#define MAX_ALIEN_SHOTS 3 // max shots in the air from the big alien block. mystery goes extra
#define N_ALIENS_X 11     // number of aliens in the block in x direction
#define N_ALIENS_Y 5      // number of aliens in the block in y direction
#define ALIEN_DEATH_TIME                                                                                     \
  (0.3 * 1000.0 / LOOP_SPEED)     // number frames to show explosions + freeze alien movement on hit
#define ALIEN_SHOT_PROBABILITY 20 // rand() % ALIEN_SHOT_PROBABILITY == 0 is the test

#define LETTER_WIDTH (1.0 / 45.0)   // scale font so that 45 letters fit next to each other
#define LETTER_SPACING (1.0 / 28.0) // space text so that 28 letters fit next to each other
#define LETTER_HEIGHT (LETTER_WIDTH * FONT_HEIGHT / (float)FONT_WIDTH)
#define CELL_WIDTH (1.0 / 20.0)        // size factor for when nothing else is appropriate
#define GAP 1.5                        // space between aliens in the block + lifes
#define SHOT_LENGTH (0.4 * CELL_WIDTH) // length of the visible shot graphics

#define TOP_MARGIN (5 * LETTER_HEIGHT)                         // start of the alien block from the top
#define BOTTOM_MARGIN (1.0 - 2 * LETTER_HEIGHT * ASPECT_RATIO) // ground plane
#define MYSTERY_SHIP_Y (3 * LETTER_HEIGHT)                     // height where the UFO flies
#define PLAYER_Y 0.85                                          // height where the player moves

// *_[WIDTH|HEIGHT] is pixel size of the data
// *_TARGET_[WIDTH|HEIGHT] is size wrt. playground (0..1)
#define TARGET_HEIGHT(n) (((float)n##_TARGET_WIDTH / n##_WIDTH * n##_HEIGHT * ASPECT_RATIO))

// clang-format off

#define ALIEN_WIDTH 6 // pixel size of the bitmaps
#define ALIEN_HEIGHT 6
#define ALIEN_TARGET_WIDTH CELL_WIDTH
#define ALIEN_TARGET_HEIGHT TARGET_HEIGHT(ALIEN)
static const uint8_t alien[2][ALIEN_WIDTH * ALIEN_HEIGHT] = {
  // first animation frame
  {
    0x00, 0xff, 0xff, 0xff, 0xff, 0x00,
    0xff, 0x00, 0xff, 0xff, 0x00, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0xff, 0x00, 0x00, 0xff, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xff, 0xff, 0xff, 0xff, 0x00,
  },
  // second animation frame
  {
    0x00, 0xff, 0xff, 0xff, 0xff, 0x00,
    0xff, 0x00, 0xff, 0xff, 0x00, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0xff, 0x00, 0x00, 0xff, 0x00,
    0x00, 0xff, 0xff, 0xff, 0xff, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  },
};

#define PLAYER_WIDTH 13
#define PLAYER_HEIGHT 8
#define PLAYER_TARGET_WIDTH (1.2 * CELL_WIDTH)
#define PLAYER_TARGET_HEIGHT TARGET_HEIGHT(PLAYER)
static const uint8_t player[3][PLAYER_WIDTH * PLAYER_HEIGHT] = {
  // normal graphic
  {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  },
  // explosion 1
  {
    0x00, 0xff, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00, 0x00,
    0x00, 0xff, 0xff, 0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff,
    0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0xff, 0xff, 0x00, 0xff, 0x00, 0x00,
    0xff, 0xff, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0xff, 0x00,
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00,
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00,
  },
  // explosion 2
  {
    0x00, 0x00, 0xff, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00,
    0x00, 0xff, 0x00, 0xff, 0x00, 0x00, 0xff, 0x00, 0xff, 0xff, 0x00, 0xff, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0xff, 0x00, 0x00, 0xff, 0x00, 0x00,
    0x00, 0xff, 0xff, 0x00, 0xff, 0x00, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0xff,
    0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0xff, 0x00,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0xff,
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00,
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00,
  },
};

#define MYSTERY_SHIP_WIDTH 16
#define MYSTERY_SHIP_HEIGHT 7
#define MYSTERY_SHIP_TARGET_WIDTH CELL_WIDTH
#define MYSTERY_SHIP_TARGET_HEIGHT TARGET_HEIGHT(MYSTERY_SHIP)
static const uint8_t mystery_ship[MYSTERY_SHIP_WIDTH * MYSTERY_SHIP_HEIGHT] = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00,
  0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00,
  0x00, 0xff, 0xff, 0x00, 0xff, 0xff, 0x00, 0xff, 0xff, 0x00, 0xff, 0xff, 0x00, 0xff, 0xff, 0x00,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0x00, 0x00, 0xff, 0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0xff, 0x00, 0x00,
  0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00,
};

#define BUNKER_WIDTH 22
#define BUNKER_HEIGHT 16
#define BUNKER_TARGET_WIDTH (1.0 / 9.0)
#define BUNKER_TARGET_HEIGHT TARGET_HEIGHT(BUNKER)
#define BUNKER_Y (PLAYER_Y - PLAYER_TARGET_HEIGHT - BUNKER_TARGET_HEIGHT)
static const uint8_t bunker[BUNKER_WIDTH * BUNKER_HEIGHT] = {
  0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00,
  0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00,
  0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};


#define EXPLOSION_WIDTH 12
#define EXPLOSION_HEIGHT 12
// keep this in sync to the bunker so that the damages look good later
#define EXPLOSION_TARGET_WIDTH (BUNKER_TARGET_WIDTH / BUNKER_WIDTH * EXPLOSION_WIDTH)
#define EXPLOSION_TARGET_HEIGHT TARGET_HEIGHT(EXPLOSION)
#define EXPLOSION_ALIEN 0
#define EXPLOSION_MYSTERY 1
#define EXPLOSION_SHOT 2
#define EXPLOSION_TOP 3
#define EXPLOSION_BOTTOM 4
#define EXPLOSION_AMOUNT 5
static const uint8_t explosions[EXPLOSION_AMOUNT][EXPLOSION_WIDTH * EXPLOSION_HEIGHT] = {
  // aliens
  {
    0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00,
    0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00,
    0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00,
    0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  },
  // mystery
  {
    0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0x00,
    0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00,
    0xff, 0xff, 0xff, 0xff, 0x00, 0xff, 0xff, 0x00, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00,
    0x00, 0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  },
  // shot
  {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0x00, 0xff, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xff, 0x00, 0xff, 0xff, 0xff, 0x00, 0xff, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xff, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  },
  // on the top
  {
    0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0x00, 0xff, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0x00, 0xff, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xff, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  },
  // on the bottom
  {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0xff, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xff, 0x00, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xff, 0x00, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
  },
};

// clang-format on


typedef struct dt_knight_shot_t
{
  gboolean active;
  float x, y, start, direction;
} dt_knight_shot_t;

typedef struct dt_knight_alien_t
{
  gboolean alive;
  float x, y;
  int frame;
  int points;
} dt_knight_alien_t;

typedef struct dt_knight_explosion_t
{
  float x, y, target_width;
  int ttl;
  cairo_pattern_t *sprite;
} dt_knight_explosion_t;

typedef struct dt_knight_t
{
  // control state
  enum
  {
    INTRO,
    START,
    GAME,
    WIN,
    LOSE
  } game_state;
  unsigned int animation_loop; // animation frame counter for the non-interactive states
  guint event_loop;
  int freeze; // frames until the freeze is over
  gboolean total_freeze, super_total_final_freeze;
  GList *explosions;
  int move; // we handle movement in the event loop. using key_pressed suffers from X's key repeat + delay

  // visible game state
  int credit;
  int lifes;
  unsigned int score_1, score_2, high_score;

  // other state
  float player_x;
  dt_knight_shot_t player_shot;

  dt_knight_alien_t aliens[N_ALIENS_X * N_ALIENS_Y];
  int n_aliens;
  enum
  {
    ALIEN_LEFT,
    ALIEN_RIGHT,
    ALIEN_DOWN_THEN_LEFT,
    ALIEN_DOWN_THEN_RIGHT
  } alien_direction;
  int alien_next_to_move;
  dt_knight_shot_t alien_shots[MAX_ALIEN_SHOTS + 1]; // the mystery ship can shoot, too, so it's +1
  int n_alien_shots;
  float mystery_ship_x;
  int time_until_mystery_ship;
  float mystery_ship_potential_shot_x;

  // buffers to free in the end
  GList *bufs, *surfaces, *patterns;
  // sprites
  cairo_pattern_t *alien_sprite[2];
  cairo_pattern_t *player_sprite[3];
  cairo_pattern_t *mystery_sprite;
  cairo_pattern_t *explosion_sprite[EXPLOSION_AMOUNT];
  cairo_pattern_t **letters;
  cairo_pattern_t *bunker_sprite[4];
  // needed to add explosions to the bunkers
  int bunker_stride;
  uint8_t *bunker_buf[4];
} dt_knight_t;

const char *name(const dt_view_t *self)
{
  return _("good knight");
}

uint32_t view(const dt_view_t *self)
{
  return DT_VIEW_KNIGHT;
}

uint32_t flags()
{
  return VIEW_FLAGS_HIDDEN;
}

// turn a monochrome pixel buffer into a cairo pattern for later usage
static inline cairo_pattern_t *_new_sprite(const uint8_t *data, const int width, const int height,
                                           int *_stride, GList **bufs, GList **surfaces, GList **patterns)
{
  const int32_t stride = cairo_format_stride_for_width(CAIRO_FORMAT_A8, width);
  uint8_t *buf = (uint8_t *)malloc((size_t)stride * height);
  for(int y = 0; y < height; y++) memcpy(&buf[y * stride], &(data[y * width]), sizeof(uint8_t) * width);
  cairo_surface_t *surface = cairo_image_surface_create_for_data(buf, CAIRO_FORMAT_A8, width, height, stride);
  cairo_pattern_t *pattern = cairo_pattern_create_for_surface(surface);
  cairo_pattern_set_filter(pattern, CAIRO_FILTER_NEAREST);
  *bufs = g_list_append(*bufs, buf);
  *surfaces = g_list_append(*surfaces, surface);
  *patterns = g_list_append(*patterns, pattern);
  if(_stride) *_stride = stride;
  return pattern;
}

// spawn a mystery ship every 25±3 seconds
static int _get_mystery_timeout()
{
  int r = (rand() % 7) - 3;
  return (25.0 + r) * 1000.0 / LOOP_SPEED;
}

// reset most but not all values of dt_knight_t
static void _reset_board(dt_knight_t *d)
{
  d->player_x = 0.0;
  d->player_shot.active = FALSE;

  for(int y = 0; y < N_ALIENS_Y; y++)
    for(int x = 0; x < N_ALIENS_X; x++)
    {
      int i = y * N_ALIENS_X + x;
      d->aliens[i].x = x * ALIEN_TARGET_WIDTH * GAP + 0.5 - (N_ALIENS_X - 1) * 0.5 * ALIEN_TARGET_WIDTH * GAP
                       - 0.5 * ALIEN_TARGET_WIDTH;
      d->aliens[i].y = y * ALIEN_TARGET_HEIGHT * GAP + TOP_MARGIN;
      d->aliens[i].alive = TRUE;
      d->aliens[i].frame = 0;
      d->aliens[i].points = ((N_ALIENS_Y - y - 1) / 2 + 1) * 10; // bottom 2: 10, middle 2: 20, top: 30
    }
  d->n_aliens = N_ALIENS_Y * N_ALIENS_X;
  d->alien_direction = ALIEN_RIGHT;
  d->alien_next_to_move = 0 + (N_ALIENS_Y - 1) * N_ALIENS_X;
  for(int i = 0; i < MAX_ALIEN_SHOTS + 1; i++) d->alien_shots[i].active = FALSE;
  d->n_alien_shots = 0;
  d->mystery_ship_x = -1.0;
  d->time_until_mystery_ship = _get_mystery_timeout();
  d->mystery_ship_potential_shot_x = 0.0;

  d->move = 0;
  d->freeze = 0;
  d->total_freeze = FALSE;
  d->super_total_final_freeze = FALSE;
  d->animation_loop = 0;
  g_list_free_full(d->explosions, free);
  d->explosions = NULL;

  d->lifes = 3;
  d->score_1 = d->score_2 = 0;
}

void init(dt_view_t *self)
{
  self->data = calloc(1, sizeof(dt_knight_t));
  dt_knight_t *d = (dt_knight_t *)self->data;

  _reset_board(d);
  d->game_state = INTRO;

  // create sprites
  // good knight alien frames
  for(int i = 0; i < 2; i++)
    d->alien_sprite[i]
        = _new_sprite(alien[i], ALIEN_WIDTH, ALIEN_HEIGHT, NULL, &(d->bufs), &(d->surfaces), &(d->patterns));
  // player
  for(int i = 0; i < 3; i++)
    d->player_sprite[i] = _new_sprite(player[i], PLAYER_WIDTH, PLAYER_HEIGHT, NULL, &(d->bufs),
                                      &(d->surfaces), &(d->patterns));
  // mystery ship
  d->mystery_sprite = _new_sprite(mystery_ship, MYSTERY_SHIP_WIDTH, MYSTERY_SHIP_HEIGHT, NULL, &(d->bufs),
                                  &(d->surfaces), &(d->patterns));
  // explosions
  for(int i = 0; i < EXPLOSION_AMOUNT; i++)
    d->explosion_sprite[i] = _new_sprite(explosions[i], EXPLOSION_WIDTH, EXPLOSION_HEIGHT, NULL, &(d->bufs),
                                         &(d->surfaces), &(d->patterns));
  // bunkers
  for(int i = 0; i < 4; i++)
  {
    d->bunker_sprite[i] = _new_sprite(bunker, BUNKER_WIDTH, BUNKER_HEIGHT, &d->bunker_stride, &(d->bufs),
                                      &(d->surfaces), &(d->patterns));
    d->bunker_buf[i] = (uint8_t *)g_list_last(d->bufs)->data;
  }
  // font
  d->letters = (cairo_pattern_t **)malloc(sizeof(cairo_pattern_t *) * n_letters);
  for(int i = 0; i < n_letters; i++)
    d->letters[i]
        = _new_sprite(font[i], FONT_WIDTH, FONT_HEIGHT, NULL, &(d->bufs), &(d->surfaces), &(d->patterns));
}

void cleanup(dt_view_t *self)
{
  dt_knight_t *d = (dt_knight_t *)self->data;

  g_list_free_full(d->patterns, (GDestroyNotify)cairo_pattern_destroy);
  g_list_free_full(d->surfaces, (GDestroyNotify)cairo_surface_destroy);
  g_list_free_full(d->bufs, free);

  free(d->letters);
  free(self->data);
}

// get the next alien in move order: bottom left to top right
static int _next_alien(dt_knight_alien_t *aliens, int current)
{
  for(int i = 0; i < N_ALIENS_Y * N_ALIENS_X; i++)
  {
    int x = (current % N_ALIENS_X) + 1;
    int y = current / N_ALIENS_X;
    if(x == N_ALIENS_X)
    {
      x = 0;
      y = (y - 1 + N_ALIENS_Y) % N_ALIENS_Y;
    }
    current = x + y * N_ALIENS_X;
    if(aliens[current].alive) return current;
  }
  return -1;
}

// get the lowest alien in the left most column
static float _leftest(const dt_knight_alien_t *aliens)
{
  for(int x = 0; x < N_ALIENS_X; x++)
    for(int y = N_ALIENS_Y - 1; y >= 0; y--)
    {
      const int i = x + y * N_ALIENS_X;
      if(aliens[i].alive) return aliens[i].x;
    }
  return 0.0;
}

// get the lowest alien in the rightmost column
static float _rightest(const dt_knight_alien_t *aliens)
{
  for(int x = N_ALIENS_X - 1; x >= 0; x--)
    for(int y = N_ALIENS_Y - 1; y >= 0; y--)
    {
      const int i = x + y * N_ALIENS_X;
      if(aliens[i].alive) return aliens[i].x;
    }
  return 0.0;
}

// reset the spawn timer when removing the mystery ship
static inline void _kill_mystery_ship(dt_knight_t *d)
{
  d->mystery_ship_x = -1.0;
  d->time_until_mystery_ship = _get_mystery_timeout();
}

// roll a dice to see where the mystery ship will shoot when adding it
static inline void _add_mystery_ship(dt_knight_t *d)
{
  d->mystery_ship_x = 0.0;
  // only shoot once per occurrence
  d->mystery_ship_potential_shot_x = (float)rand() / (float)RAND_MAX;
}

// return a new explosion object with the fields initialized. has to be free()'d
static dt_knight_explosion_t *_new_explosion(float x, float y, int ttl, cairo_pattern_t *sprite)
{
  dt_knight_explosion_t *explosion = (dt_knight_explosion_t *)malloc(sizeof(dt_knight_explosion_t));
  explosion->x = x;
  explosion->y = y;
  explosion->ttl = ttl;
  explosion->sprite = sprite;
  return explosion;
}

// change the bunker graphics by subtracting an explosion sprite
static void _destroy_bunker(dt_knight_t *d, int bunker_idx, int hit_x, int hit_y)
{
  uint8_t *buf = d->bunker_buf[bunker_idx];
  // the explosion has stride == width
  const uint8_t *ex = explosions[EXPLOSION_SHOT];

  const int ex_half = EXPLOSION_WIDTH / 2.0 + 0.5;
  const int ex_x0 = MAX(ex_half - hit_x, 0);
  const int ex_x1 = MIN(BUNKER_WIDTH - hit_x + ex_half, EXPLOSION_WIDTH);
  const int ex_y0 = MAX(ex_half - hit_y, 0);
  const int ex_y1 = MIN(BUNKER_HEIGHT - hit_y + ex_half, EXPLOSION_HEIGHT);

  const int buf_x0 = MAX(hit_x - ex_half, 0);
  const int buf_y0 = MAX(hit_y - ex_half, 0);

  for(int y = ex_y0, j = 0; y < ex_y1; y++, j++)
    for(int x = ex_x0, i = 0; x < ex_x1; x++, i++)
    {
      const int in = x + y * EXPLOSION_WIDTH;
      const int out = buf_x0 + i + (buf_y0 + j) * d->bunker_stride;
      buf[out] &= ~ex[in];
    }
}

// check if a shot hit a bunker and deal out damage if needed
static gboolean _hit_bunker(dt_knight_t *d, const dt_knight_shot_t *shot)
{
  const float top = BUNKER_Y;
  const float bottom = BUNKER_Y + BUNKER_TARGET_HEIGHT;
  if((shot->direction > 0.0 && shot->y <= bottom && shot->y + SHOT_LENGTH >= top)
     || (shot->y >= top && shot->y - SHOT_LENGTH <= bottom))
  {
    // we might have hit a bunker
    for(int i = 0; i < 4; i++)
    {
      const float bunker_x = (i * 2 + 1) * BUNKER_TARGET_WIDTH;
      // check the bounding box
      if(shot->x >= bunker_x && shot->x <= bunker_x + BUNKER_TARGET_WIDTH)
      {
        // we are in the bb, now check the pixels, we might have hit a hole
        uint8_t *buf = d->bunker_buf[i];
        int pixel_x = ((shot->x - bunker_x) / BUNKER_TARGET_WIDTH) * BUNKER_WIDTH + 0.5;
        pixel_x = CLAMP(pixel_x, 0, BUNKER_WIDTH - 1);
        for(int j = 0; j < BUNKER_HEIGHT; j++)
        {
          const int pixel_y = shot->direction > 0 ? BUNKER_HEIGHT - 1 - j : j;
          const int pixel = pixel_x + pixel_y * d->bunker_stride;
          if(buf[pixel] == 0xff)
          {
            // destroy it!
            _destroy_bunker(d, i, pixel_x, pixel_y);
            const float _x
                = bunker_x + pixel_x * BUNKER_TARGET_WIDTH / BUNKER_WIDTH - 0.5 * EXPLOSION_TARGET_WIDTH;
            const float _y
                = BUNKER_Y + pixel_y * BUNKER_TARGET_HEIGHT / BUNKER_HEIGHT - 0.5 * EXPLOSION_TARGET_HEIGHT;
            dt_knight_explosion_t *explosion
                = _new_explosion(_x, _y, ALIEN_DEATH_TIME, d->explosion_sprite[EXPLOSION_SHOT]);
            d->explosions = g_list_append(d->explosions, explosion);
            return TRUE;
          }
        }
        break; // can't possibly hit any other bunker
      }
    }
  }
  return FALSE;
}

// when an alien occupies the same space as a bunker the touched part gets removed
static void _walk_over_bunker(dt_knight_t *d, float x, float y, float w, float h)
{
  const float top = BUNKER_Y;
  const float bottom = BUNKER_Y + BUNKER_TARGET_HEIGHT;
  if(y <= bottom && y + h >= top)
  {
    // we might have hit a bunker
    for(int i = 0; i < 4; i++)
    {
      const float bunker_x = (i * 2 + 1) * BUNKER_TARGET_WIDTH;
      // check the bounding box
      if(x + w >= bunker_x && x <= bunker_x + BUNKER_TARGET_WIDTH)
      {
        // we are in the bb, clear the rectangle
        uint8_t *buf = d->bunker_buf[i];

        // express x/y relative to bunker_x/bunker_y in bunker pixels
        const int pixel_x = (x - bunker_x) / BUNKER_TARGET_WIDTH * BUNKER_WIDTH + 0.5;
        const int pixel_y = (y - BUNKER_Y) / BUNKER_TARGET_HEIGHT * BUNKER_HEIGHT + 0.5;
        const int pixel_w = w / BUNKER_TARGET_WIDTH * BUNKER_WIDTH + 0.5;
        const int pixel_h = h / BUNKER_TARGET_HEIGHT * BUNKER_HEIGHT + 0.5;

        // overlap with bunker
        const int overhang_left = MAX(-1 * pixel_x, 0);
        const int overhang_right = MAX(pixel_x + pixel_w - BUNKER_WIDTH, 0);
        const int overlap_x = pixel_w - overhang_left - overhang_right;

        const int overhang_top = MAX(-1 * pixel_y, 0);
        const int overhang_bottom = MAX(pixel_y + pixel_h - BUNKER_HEIGHT, 0);
        const int overlap_y = pixel_h - overhang_top - overhang_bottom;

        // the area to clear is (x0, y0) -> (x0 + overlap_x, y0 + overlap_y)
        const int x0 = MAX(pixel_x, 0);
        const int y0 = MAX(pixel_y, 0);

        for(int _y = y0; _y < y0 + overlap_y; _y++)
        {
          const int i = x0 + _y * d->bunker_stride;
          memset(&buf[i], 0x00, overlap_x);
        }
        break; // can't possibly hit any other bunker
      }
    }
  }
}

// the control logic for the interactive part
static gboolean _event_loop_game(dt_knight_t *d)
{
  // clean up explosions
  for(GList *iter = d->explosions; iter; iter = g_list_next(iter))
  {
    dt_knight_explosion_t *explosion = (dt_knight_explosion_t *)iter->data;
    explosion->ttl--;
    if(explosion->ttl == 0)
    {
      free(explosion);
      iter = d->explosions = g_list_delete_link(d->explosions, iter);
    }
  }

  if(d->freeze > 0)
  {
    d->freeze--;
    if(d->freeze == 0 && d->total_freeze)
    {
      // the player was hit. move him to the left
      d->total_freeze = FALSE;
      d->player_x = 0.0;
      d->lifes--;
      if(d->super_total_final_freeze) d->lifes = 0;
    }
    if(d->super_total_final_freeze) goto end;
  }

  // handle movement in the event loop to not be affected by X's keyboard repeat rates and delay
  if(!d->total_freeze)
    d->player_x
        = CLAMP(d->player_x + d->move * PLAYER_TARGET_WIDTH * STEP_SIZE, 0.0, 1.0 - PLAYER_TARGET_WIDTH);

  // spawn a mystery ship roughly every 25 seconds
  d->time_until_mystery_ship--;
  if(d->time_until_mystery_ship == 0)
    _add_mystery_ship(d);
  else
  {
    if(d->mystery_ship_x >= 0.0) d->mystery_ship_x += MYSTERY_SHIP_TARGET_WIDTH * STEP_SIZE;
    if(d->mystery_ship_x >= 1.0 - MYSTERY_SHIP_TARGET_WIDTH) _kill_mystery_ship(d);
  }

  // don't fire in the first 1.5 seconds
  if(d->animation_loop > 1.5 * 1000.0 / LOOP_SPEED)
  {
    if(d->freeze == 0)
    {
      // randomly shoot at the player
      if(d->n_alien_shots < MAX_ALIEN_SHOTS && rand() % ALIEN_SHOT_PROBABILITY == 0)
      {
        int column = rand() % N_ALIENS_X;
        for(int c = 0; c < N_ALIENS_X; c++)
        {
          // if the column has no alien left we try the next one
          int column_candidate = (column + c) % N_ALIENS_X;
          for(int row = N_ALIENS_Y - 1; row >= 0; row--)
          {
            const int i = row * N_ALIENS_X + column_candidate;
            if(!d->aliens[i].alive) continue;

            // find an empty spot in the shot table
            for(int s = 0; s < MAX_ALIEN_SHOTS; s++)
            {
              if(d->alien_shots[s].active) continue;
              d->n_alien_shots++;
              d->alien_shots[s].active = TRUE;
              d->alien_shots[s].x = d->aliens[i].x + 0.5 * ALIEN_TARGET_WIDTH;
              d->alien_shots[s].y = d->alien_shots[s].start
                  = d->aliens[i].y + ALIEN_TARGET_HEIGHT + SHOT_LENGTH;
              d->alien_shots[s].direction = -1.0;
              goto alien_shots_fired;
            }
          }
        }
      }
    }
  alien_shots_fired:

    // the mystery ship can shoot, too
    if(d->mystery_ship_x >= d->mystery_ship_potential_shot_x - 0.5 * MYSTERY_SHIP_TARGET_WIDTH
       && !d->alien_shots[MAX_ALIEN_SHOTS].active)
    {
      d->mystery_ship_potential_shot_x = 2.0;
      d->alien_shots[MAX_ALIEN_SHOTS].active = TRUE;
      d->alien_shots[MAX_ALIEN_SHOTS].x = d->mystery_ship_x + 0.5 * MYSTERY_SHIP_TARGET_WIDTH;
      d->alien_shots[MAX_ALIEN_SHOTS].y = d->alien_shots[MAX_ALIEN_SHOTS].start
          = MYSTERY_SHIP_Y + MYSTERY_SHIP_TARGET_HEIGHT + SHOT_LENGTH;
      d->alien_shots[MAX_ALIEN_SHOTS].direction = -1.0;
    }
  }
  else
    d->animation_loop++;

  // move shots
  // the player shot
  if(d->player_shot.active)
  {
    d->player_shot.y -= SHOT_LENGTH;

    // TODO: setting this to 0.0 means we can shoot between the aliens. is that what the original did?
    const float half_gap = ALIEN_TARGET_WIDTH * (GAP - 1.0) / 2.0;

    // did the player hit something?
    // check aliens
    for(int i = 0; i < N_ALIENS_Y * N_ALIENS_X; i++)
    {
      dt_knight_alien_t *curr_alien = &d->aliens[i];
      if(!curr_alien->alive) continue;
      if(d->player_shot.x >= curr_alien->x - half_gap
         && d->player_shot.x <= curr_alien->x + ALIEN_TARGET_WIDTH + half_gap
         && d->player_shot.y >= curr_alien->y - SHOT_LENGTH && d->player_shot.y <= curr_alien->y + ALIEN_TARGET_HEIGHT)
      {
        // we hit an alien
        d->freeze = ALIEN_DEATH_TIME;
        d->player_shot.active = FALSE;
        curr_alien->alive = FALSE;
        d->n_aliens--;
        d->score_1 += curr_alien->points;
        dt_knight_explosion_t *explosion
            = _new_explosion(curr_alien->x, curr_alien->y, ALIEN_DEATH_TIME, d->explosion_sprite[EXPLOSION_ALIEN]);
        d->explosions = g_list_append(d->explosions, explosion);
        if(d->alien_next_to_move == i) d->alien_next_to_move = _next_alien(d->aliens, d->alien_next_to_move);
        break;
      }
    }

    // test other stuff
    if(d->player_shot.y <= 2.5 * LETTER_HEIGHT)
    {
      // we hit the top of the board
      d->player_shot.active = FALSE;
      dt_knight_explosion_t *explosion
          = _new_explosion(d->player_shot.x - 0.5 * EXPLOSION_TARGET_WIDTH, 2.5 * LETTER_HEIGHT,
                           ALIEN_DEATH_TIME, d->explosion_sprite[EXPLOSION_TOP]);
      d->explosions = g_list_append(d->explosions, explosion);
    }
    else if(d->player_shot.x >= d->mystery_ship_x
            && d->player_shot.x <= d->mystery_ship_x + MYSTERY_SHIP_TARGET_WIDTH
            && d->player_shot.y >= MYSTERY_SHIP_Y - SHOT_LENGTH
            && d->player_shot.y <= MYSTERY_SHIP_Y + MYSTERY_SHIP_TARGET_HEIGHT)
    {
      // we hit the mystery ship
      d->player_shot.active = FALSE;
      d->score_1 += 50;
      dt_knight_explosion_t *explosion = _new_explosion(d->mystery_ship_x, MYSTERY_SHIP_Y, ALIEN_DEATH_TIME,
                                                        d->explosion_sprite[EXPLOSION_MYSTERY]);
      d->explosions = g_list_append(d->explosions, explosion);
      _kill_mystery_ship(d);
    }
    else if(_hit_bunker(d, &d->player_shot))
    {
      // we hit a bunker
      d->player_shot.active = FALSE;
    }

    // shot vs. shot is tested later
  }


  // now move the alien shots
  gboolean was_hit = d->total_freeze; // guard against several hits at once
  for(int s = 0; s < MAX_ALIEN_SHOTS + 1; s++)
  {
    dt_knight_shot_t *shot = &d->alien_shots[s];
    if(!shot->active) continue;

    shot->y += SHOT_LENGTH;

    if(shot->x >= d->player_x - 0.2 * PLAYER_TARGET_WIDTH
       && shot->x <= d->player_x + 1.2 * PLAYER_TARGET_WIDTH && shot->y >= PLAYER_Y
       && shot->y <= PLAYER_Y + PLAYER_TARGET_HEIGHT + SHOT_LENGTH)
    {
      // we hit the player. he is immune when the alien was directly above him!
      if(shot->start <= PLAYER_Y - ALIEN_TARGET_HEIGHT && !was_hit)
      {
        was_hit = TRUE;
        d->freeze = 3.0 * 1000.0 / LOOP_SPEED;
        d->total_freeze = TRUE;
      }
      shot->active = FALSE;
      d->n_alien_shots--;
    }
    else if(d->player_shot.active && fabs(shot->x - d->player_shot.x) < 0.4 * CELL_WIDTH
            && shot->y >= d->player_shot.y) // they can only meet from one direction
    {
      // the player hit the alien's shot. destroy it. there's a 50% chance that the player shot survives
      // FIXME: is it this way or the other way round?
      shot->active = FALSE;
      d->n_alien_shots--;
      if(rand() % 2 == 0) d->player_shot.active = FALSE;
      dt_knight_explosion_t *explosion
          = _new_explosion(d->player_shot.x - 0.5 * EXPLOSION_TARGET_WIDTH, d->player_shot.y,
                           ALIEN_DEATH_TIME, d->explosion_sprite[EXPLOSION_SHOT]);
      d->explosions = g_list_append(d->explosions, explosion);
    }
    else if(_hit_bunker(d, shot))
    {
      // we hit a bunker
      shot->active = FALSE;
      d->n_alien_shots--;
    }
    else if(shot->y >= BOTTOM_MARGIN)
    {
      // we hit the ground
      shot->active = FALSE;
      d->n_alien_shots--;
      dt_knight_explosion_t *explosion
          = _new_explosion(shot->x - 0.5 * EXPLOSION_TARGET_WIDTH, BOTTOM_MARGIN - EXPLOSION_TARGET_HEIGHT,
                           ALIEN_DEATH_TIME, d->explosion_sprite[EXPLOSION_BOTTOM]);
      d->explosions = g_list_append(d->explosions, explosion);
    }
  }


  // move in blocks of 2
  if(d->freeze == 0)
  {
    for(int i = 0; i < 2; i++)
    {
      if(d->alien_next_to_move == -1) break;
      const int x = d->alien_next_to_move % N_ALIENS_X;
      const int y = d->alien_next_to_move / N_ALIENS_X;
      const int next = _next_alien(d->aliens, d->alien_next_to_move);
      const int next_x = next % N_ALIENS_X;
      const int next_y = next / N_ALIENS_X;
      dt_knight_alien_t *alien_tm = &d->aliens[d->alien_next_to_move];
      switch(d->alien_direction)
      {
        case ALIEN_LEFT:
          alien_tm->x -= STEP_SIZE * ALIEN_TARGET_WIDTH;
          if((next_y > y || (next_y == y && next_x < x) || next == d->alien_next_to_move)
             && _leftest(d->aliens) - STEP_SIZE * ALIEN_TARGET_WIDTH < 0.0)
            d->alien_direction = ALIEN_DOWN_THEN_RIGHT;
          break;
        case ALIEN_RIGHT:
          alien_tm->x += STEP_SIZE * ALIEN_TARGET_WIDTH;
          if((next_y > y || (next_y == y && next_x < x) || next == d->alien_next_to_move)
             && _rightest(d->aliens) + ALIEN_TARGET_WIDTH + STEP_SIZE * ALIEN_TARGET_WIDTH > 1.0)
            d->alien_direction = ALIEN_DOWN_THEN_LEFT;
          break;
        case ALIEN_DOWN_THEN_LEFT:
        case ALIEN_DOWN_THEN_RIGHT:
          alien_tm->y += 0.5 * ALIEN_TARGET_HEIGHT;
          if(alien_tm->y + ALIEN_TARGET_HEIGHT >= PLAYER_Y + 0.5 * PLAYER_TARGET_HEIGHT)
          {
            d->freeze = 3.0 * 1000.0 / LOOP_SPEED;
            d->total_freeze = TRUE;
            d->super_total_final_freeze = TRUE;
          }
          if(next_y > y || (next_y == y && next_x < x) || next == d->alien_next_to_move)
            d->alien_direction = (d->alien_direction == ALIEN_DOWN_THEN_LEFT ? ALIEN_LEFT : ALIEN_RIGHT);
          break;
      }

      // when going over a bunker it (the bunker) gets destroyed
      _walk_over_bunker(d, alien_tm->x, alien_tm->y, ALIEN_TARGET_WIDTH, ALIEN_TARGET_HEIGHT);

      // allow the last one to go really fast, but keep it animating
      if(!(i == 0 && d->alien_next_to_move == next)) alien_tm->frame = 1 - alien_tm->frame;
      d->alien_next_to_move = next;
    }
  }

end:
  // finally, did one side win?
  if(d->n_aliens == 0)
  {
    d->high_score = MAX(d->score_1, d->high_score);
    d->game_state = WIN;
    d->animation_loop = 0;
  }

  if(d->lifes == 0)
  {
    d->game_state = LOSE;
    d->animation_loop = 0;
  }

  return TRUE;
}

// the control logic for the non-interactive part: just count up the frames
static gboolean _event_loop_animation(dt_knight_t *d)
{
  d->animation_loop++;
  return TRUE;
}

// control dispatcher, makes sure that the screen is redrawn afterwards
static gboolean _event_loop(gpointer user_data)
{
  dt_knight_t *d = (dt_knight_t *)user_data;
  gboolean res = FALSE;  // silence warning about uninitialized res
  switch(d->game_state)
  {
    case INTRO:
    case START:
    case WIN:
    case LOSE:
      res = _event_loop_animation(d);
      break;
    case GAME:
      res = _event_loop_game(d);
      break;
  }
  dt_control_queue_redraw_center();
  return res;
}

static gboolean _key_press(GtkWidget *w, GdkEventKey *event, dt_knight_t *d);
static gboolean _key_release(GtkWidget *w, GdkEventKey *event, dt_knight_t *d);

void enter(dt_view_t *self)
{
  dt_knight_t *d = (dt_knight_t *)self->data;

  dt_control_change_cursor(GDK_BLANK_CURSOR);

  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_LEFT, FALSE, TRUE);
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_RIGHT, FALSE, TRUE);
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_TOP, FALSE, TRUE);
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_BOTTOM, FALSE, TRUE);
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_CENTER_TOP, FALSE, TRUE);
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_CENTER_BOTTOM, FALSE, TRUE);

  // set the initial game state
  switch(d->game_state)
  {
    case GAME: // allow to pause by leaving the view
      break;
    case WIN:
    case LOSE:
      // don't show the full intro again. it gets annoying
      d->game_state = START;
    case INTRO:
    case START:
      // restart the current state
      d->animation_loop = 0;
      _reset_board(d);
      break;
  }

  g_signal_connect(dt_ui_center(darktable.gui->ui), "key-press-event", G_CALLBACK(_key_press), d);
  g_signal_connect(dt_ui_center(darktable.gui->ui), "key-release-event", G_CALLBACK(_key_release), d);

  // start event loop
  d->event_loop = g_timeout_add(LOOP_SPEED, _event_loop, d);
}

void leave(dt_view_t *self)
{
  dt_knight_t *d = (dt_knight_t *)self->data;

  // show normal gui again
  dt_control_change_cursor(GDK_LEFT_PTR);

  g_signal_handlers_disconnect_by_func(dt_ui_center(darktable.gui->ui), G_CALLBACK(_key_press), d);
  g_signal_handlers_disconnect_by_func(dt_ui_center(darktable.gui->ui), G_CALLBACK(_key_release), d);

  // stop event loop
  if(d->event_loop > 0) g_source_remove(d->event_loop);
  d->event_loop = 0;
}

// set the sprite's matrix to scale it up to the desired size to deal with the window size
static void _scale_sprite(cairo_pattern_t *pattern, const int width, const float target_width)
{
  cairo_matrix_t matrix;
  const float s = width / target_width;
  cairo_matrix_init_scale(&matrix, s, s);
  cairo_pattern_set_matrix(pattern, &matrix);
}

// show text using the font described in knight_font.h
// text can be justified left ('l'), right ('r') or centered ('c')
static void _show_text(cairo_t *cr, cairo_pattern_t **letters, const char *text, float x, float y, float w,
                       float h, char justify)
{
  const int l = strlen(text);
  const float spacing = LETTER_SPACING * w;
  cairo_save(cr);
  cairo_translate(cr, x, y);
  if(justify == 'c')
  {
    const float justify_offset
        = (-1 * (int)(l / 2.0 + 0.5) * LETTER_SPACING + LETTER_SPACING - LETTER_WIDTH) * w;
    cairo_translate(cr, justify_offset, 0);
  }
  else if(justify == 'r')
  {
    const float justify_offset = (-1 * l * LETTER_SPACING + LETTER_SPACING - LETTER_WIDTH) * w;
    cairo_translate(cr, justify_offset, 0);
  }
  for(int i = 0; i < l; i++)
  {
    unsigned int c = (text[i] - ' ') % n_letters;
    cairo_mask(cr, letters[c]);
    cairo_translate(cr, spacing, 0);
  }
  cairo_fill(cr);
  cairo_restore(cr);
}

// helper functions to draw specific parts of the GUI
static void _show_top_line(dt_knight_t *d, cairo_t *cr, int32_t w, int32_t h)
{
  _show_text(cr, d->letters, "SCORE<1>", LETTER_WIDTH * w, 0.0, w, h, 'l');
  _show_text(cr, d->letters, "HI-SCORE", 0.5 * w, 0.0, w, h, 'c');
  _show_text(cr, d->letters, "SCORE<2>", (1.0 - LETTER_WIDTH) * w, 0.0, w, h, 'r');
}

static void _show_score_1(dt_knight_t *d, cairo_t *cr, int32_t w, int32_t h)
{
  char text[64];
  snprintf(text, sizeof(text), "%04u", d->score_1);
  _show_text(cr, d->letters, text, (LETTER_WIDTH + LETTER_SPACING * 2) * w, 2 * LETTER_HEIGHT * w, w, h, 'l');
}

static void _show_score_2(dt_knight_t *d, cairo_t *cr, int32_t w, int32_t h)
{
  char text[64];
  snprintf(text, sizeof(text), "%04u", d->score_2);
  _show_text(cr, d->letters, text, (1.0 - (LETTER_WIDTH + LETTER_SPACING * 2)) * w, 2 * LETTER_HEIGHT * w, w,
             h, 'r');
}

static void _show_high_score(dt_knight_t *d, cairo_t *cr, int32_t w, int32_t h)
{
  char text[64];
  snprintf(text, sizeof(text), "%04u", d->high_score);
  _show_text(cr, d->letters, text, 0.5 * w, 2 * LETTER_HEIGHT * w, w, h, 'c');
}

static void _show_credit(dt_knight_t *d, cairo_t *cr, int32_t w, int32_t h)
{
  char text[64];
  snprintf(text, sizeof(text), "CREDIT %02d", d->credit);
  _show_text(cr, d->letters, text, (1.0 - LETTER_WIDTH - LETTER_SPACING) * w, h - (2 * LETTER_HEIGHT) * w, w,
             h, 'r');
}

static void _show_lifes(dt_knight_t *d, cairo_t *cr, int32_t w, int32_t h)
{
  char text[64];

  cairo_save(cr);
  cairo_translate(cr, 0, h - (2 * LETTER_HEIGHT) * w);

  cairo_set_source_rgb(cr, 1, 1, 1);
  snprintf(text, sizeof(text), "%d", d->lifes);
  _show_text(cr, d->letters, text, LETTER_WIDTH * w, 0.0, w, h, 'l');

  cairo_set_source_rgb(cr, 0, 1, 0);
  cairo_translate(cr, (LETTER_SPACING + GAP * PLAYER_TARGET_WIDTH) * w, 0);
  for(int i = 0; i < d->lifes - 1; i++)
  {
    cairo_mask(cr, d->player_sprite[0]);
    cairo_translate(cr, GAP * PLAYER_TARGET_WIDTH * w, 0);
  }
  cairo_restore(cr);
  cairo_fill(cr);
}

static void _show_bunkers(dt_knight_t *d, cairo_t *cr, int32_t w, int32_t h)
{
  cairo_save(cr);
  cairo_set_source_rgb(cr, 0, 1, 0);
  cairo_translate(cr, BUNKER_TARGET_WIDTH * w, BUNKER_Y * h);
  for(int i = 0; i < 4; i++)
  {
    cairo_mask(cr, d->bunker_sprite[i]);
    cairo_translate(cr, 2 * BUNKER_TARGET_WIDTH * w, 0);
  }
  cairo_fill(cr);
  cairo_restore(cr);
}

static void _show_aliens(dt_knight_t *d, cairo_t *cr, int32_t w, int32_t h)
{
  cairo_save(cr);
  for(int y = 0; y < N_ALIENS_Y; y++)
    for(int x = 0; x < N_ALIENS_X; x++)
    {
      int i = y * N_ALIENS_X + x;
      if(!d->aliens[i].alive) continue;
      cairo_save(cr);
      cairo_translate(cr, d->aliens[i].x * w, d->aliens[i].y * h);
      cairo_mask(cr, d->alien_sprite[d->aliens[i].frame]);
      cairo_fill(cr);
      cairo_restore(cr);
    }
  cairo_restore(cr);
}

static void _show_ground(dt_knight_t *d, cairo_t *cr, int32_t w, int32_t h)
{
  cairo_set_line_width(cr, h / 250.0);
  cairo_set_source_rgb(cr, 0, 1, 0);
  float y = BOTTOM_MARGIN * h;
  cairo_move_to(cr, 0, y);
  cairo_line_to(cr, w, y);
  cairo_stroke(cr);
}

static void _show_shot(dt_knight_t *d, cairo_t *cr, int32_t w, int32_t h, dt_knight_shot_t *shot)
{
  if(shot->active)
  {
    cairo_move_to(cr, shot->x * w, shot->y * h);
    cairo_rel_line_to(cr, 0, shot->direction * SHOT_LENGTH * w);
    cairo_stroke(cr);
  }
}

// display the running game, according to its state
static void _expose_game(dt_knight_t *d, cairo_t *cr, int32_t w, int32_t h)
{
  // draw the bottom ground line
  _show_ground(d, cr, w, h);

  // draw shots
  cairo_set_source_rgb(cr, 1, 1, 1);
  _show_shot(d, cr, w, h, &d->player_shot);
  for(int s = 0; s < MAX_ALIEN_SHOTS + 1; s++) _show_shot(d, cr, w, h, &d->alien_shots[s]);


  cairo_set_line_width(cr, 1); // was set by _show_ground()


  // draw player
  cairo_set_source_rgb(cr, 0, 1, 0);
  cairo_save(cr);
  cairo_translate(cr, d->player_x * w, PLAYER_Y * h);
  if(d->total_freeze)
    // explosion animation
    cairo_mask(cr, d->player_sprite[1 + (d->freeze % 4) / 2]);
  else
    // normal graphic
    cairo_mask(cr, d->player_sprite[0]);
  cairo_fill(cr);
  cairo_restore(cr);


  // draw bunkers
  _show_bunkers(d, cr, w, h);


  // draw the alien block
  cairo_set_source_rgb(cr, 1, 1, 1);
  _show_aliens(d, cr, w, h);


  // draw mystery ship
  if(d->mystery_ship_x >= 0.0)
  {
    cairo_save(cr);
    cairo_set_source_rgb(cr, 1, 0, 0);
    cairo_translate(cr, d->mystery_ship_x * w, MYSTERY_SHIP_Y * h);
    cairo_mask(cr, d->mystery_sprite);
    cairo_fill(cr);
    cairo_restore(cr);
  }


  // draw explosions
  cairo_set_source_rgb(cr, 1, 1, 1);
  for(GList *iter = d->explosions; iter; iter = g_list_next(iter))
  {
    dt_knight_explosion_t *explosion = (dt_knight_explosion_t *)iter->data;
    cairo_save(cr);
    cairo_translate(cr, explosion->x * w, explosion->y * h);
    cairo_mask(cr, explosion->sprite);
    cairo_fill(cr);
    cairo_restore(cr);
  }


  // draw overlay
  _show_top_line(d, cr, w, h);
  _show_score_1(d, cr, w, h);
  //   _show_score_2(d, cr, w, h); // TODO: 2nd player
  _show_high_score(d, cr, w, h);
  _show_credit(d, cr, w, h);
  _show_lifes(d, cr, w, h);
}

// draw the non-interactive part of the game: intro and win/lose screen
static void _expose_intro(dt_knight_t *d, cairo_t *cr, int32_t w, int32_t h)
{
  cairo_set_source_rgb(cr, 1, 1, 1);

  _show_top_line(d, cr, w, h);
  _show_high_score(d, cr, w, h);
  _show_credit(d, cr, w, h);

  const int wipe_duration = 1.0 * 1000.0 / (float)LOOP_SPEED; // i.e., 1 second

  if(d->game_state == INTRO)
  {
    _show_score_1(d, cr, w, h);
    _show_score_2(d, cr, w, h);

    if(d->animation_loop > 8.5 * 1000.0 / (float)LOOP_SPEED && d->player_shot.active)
    {
      d->game_state = START;
      d->animation_loop = 0;
    }
    else if(d->animation_loop > 7.5 * 1000.0 / (float)LOOP_SPEED)
    {
      // wait for player select
      _show_text(cr, d->letters, "PUSH", 0.5 * w, 11 * LETTER_HEIGHT * w, w, h, 'c');
      _show_text(cr, d->letters, "1 OR 2 PLAYERS BUTTON", 0.5 * w, 13 * LETTER_HEIGHT * w, w, h, 'c');
    }
    else if(d->animation_loop > 1.0 * 1000.0 / (float)LOOP_SPEED)
    {
      d->player_shot.active = FALSE;
      // 1s - 5s: show welcome text
      _show_text(cr, d->letters, "THE DARKTABLE TEAM", 0.5 * w, 6 * LETTER_HEIGHT * w, w, h, 'c');
      _show_text(cr, d->letters, "PRESENTS", 0.5 * w, 8 * LETTER_HEIGHT * w, w, h, 'c');
      _show_text(cr, d->letters, "THE GOOD KNIGHT", 0.5 * w, 10 * LETTER_HEIGHT * w, w, h, 'c');

      // 5s - 5.5s: wipe
      const int wipe_start = 6.0 * 1000.0 / (float)LOOP_SPEED;
      if(d->animation_loop > wipe_start)
      {
        const float wipe_progress = (float)(d->animation_loop - wipe_start) / wipe_duration;
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_rectangle(cr, 0, 5 * LETTER_HEIGHT * w, wipe_progress * w, 9 * LETTER_HEIGHT * w);
        cairo_fill(cr);
      }
    }
  }
  else if(d->game_state == START)
  {
    if(d->animation_loop > 5.0 * 1000.0 / (float)LOOP_SPEED)
    {
      int n_aliens = 0;
      d->n_aliens = MIN(d->animation_loop - (5.0 * 1000.0 / (float)LOOP_SPEED), N_ALIENS_X * N_ALIENS_Y);
      for(int y = N_ALIENS_Y - 1; y >= 0; y--)
        for(int x = 0; x < N_ALIENS_X; x++)
        {
          const int i = x + y * N_ALIENS_X;
          d->aliens[i].alive = n_aliens++ < d->n_aliens;
        }
      if(d->n_aliens == N_ALIENS_X * N_ALIENS_Y)
      {
        d->game_state = GAME;
        d->player_shot.active = FALSE;
        d->player_x = 0.0;
        d->animation_loop = 0;
      }
      _show_score_1(d, cr, w, h);
      _show_aliens(d, cr, w, h);
      _show_bunkers(d, cr, w, h);
      _show_ground(d, cr, w, h);
      _show_lifes(d, cr, w, h);
    }
    else if(d->animation_loop > 1.5 * 1000.0 / (float)LOOP_SPEED)
    {
      _show_text(cr, d->letters, "PLAY PLAYER<1>", 0.5 * w, 13 * LETTER_HEIGHT * w, w, h, 'c');
      _show_lifes(d, cr, w, h);
      if(d->animation_loop % (int)(1000.0 / (LOOP_SPEED * 2.) + 0.5) < (1000.0 / (LOOP_SPEED * 4.0)))
        _show_score_1(d, cr, w, h);
    }
    else
    {
      _show_score_1(d, cr, w, h);
      _show_score_2(d, cr, w, h);
      if(d->animation_loop <= 1.0 * 1000.0 / (float)LOOP_SPEED)
      {
        const float wipe_progress = (float)d->animation_loop / wipe_duration;

        _show_text(cr, d->letters, "PUSH", 0.5 * w, 11 * LETTER_HEIGHT * w, w, h, 'c');
        _show_text(cr, d->letters, "1 OR 2 PLAYERS BUTTON", 0.5 * w, 13 * LETTER_HEIGHT * w, w, h, 'c');

        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_rectangle(cr, 0, 0, wipe_progress * w, h);
        cairo_fill(cr);
      }
    }
  }
  else if(d->game_state == LOSE)
  {
    _show_score_1(d, cr, w, h);
    _show_lifes(d, cr, w, h);
    cairo_set_source_rgb(cr, 1, 1, 1);
    _show_text(cr, d->letters, "GAME OVER", 0.5 * w, 6 * LETTER_HEIGHT * w, w, h, 'c');
    if(d->animation_loop > 2.0 * 1000.0 / LOOP_SPEED)
      _show_text(cr, d->letters, "NOW GET BACK TO WORK", 0.5 * w, 8 * LETTER_HEIGHT * w, w, h, 'c');
    const int wipe_start = 5.0 * 1000.0 / (float)LOOP_SPEED;
    if(d->animation_loop > wipe_start)
    {
      const float wipe_progress = (float)(d->animation_loop - wipe_start) / wipe_duration;
      cairo_set_source_rgb(cr, 0, 0, 0);
      cairo_rectangle(cr, 0, 0, wipe_progress * w, h);
      cairo_fill(cr);
    }
    if(d->animation_loop > wipe_start + wipe_duration * 2) dt_ctl_switch_mode_to("lighttable");
  }
  else if(d->game_state == WIN)
  {
    _show_score_1(d, cr, w, h);
    _show_lifes(d, cr, w, h);
    cairo_set_source_rgb(cr, 1, 1, 1);
    _show_text(cr, d->letters, "WELL DONE EARTHLING", 0.5 * w, 6 * LETTER_HEIGHT * w, w, h, 'c');
    if(d->animation_loop > 1.0 * 1000.0 / LOOP_SPEED)
      _show_text(cr, d->letters, "THIS TIME YOU WIN", 0.5 * w, 8 * LETTER_HEIGHT * w, w, h, 'c');
    if(d->animation_loop > 4.0 * 1000.0 / LOOP_SPEED)
      _show_text(cr, d->letters, "NOW GET BACK TO WORK", 0.5 * w, 11 * LETTER_HEIGHT * w, w, h, 'c');
    const int wipe_start = 7.0 * 1000.0 / (float)LOOP_SPEED;
    if(d->animation_loop > wipe_start)
    {
      const float wipe_progress = (float)(d->animation_loop - wipe_start) / wipe_duration;
      cairo_set_source_rgb(cr, 0, 0, 0);
      cairo_rectangle(cr, 0, 0, wipe_progress * w, h);
      cairo_fill(cr);
    }
    if(d->animation_loop > wipe_start + wipe_duration * 2) dt_ctl_switch_mode_to("lighttable");
  }
}

void expose(dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  dt_knight_t *d = (dt_knight_t *)self->data;

  // we want a fixed playground aspect ratio
  int w = width, h = height;
  if(width / ASPECT_RATIO < height)
    h = (float)w / ASPECT_RATIO;
  else
    w = (float)h * ASPECT_RATIO;

  cairo_save(cr);
  // set 0/0 to the top left of the playground
  cairo_translate(cr, (width - w) / 2, (height - h) / 2);

  // prepare sprites
  for(int i = 0; i < 2; i++) _scale_sprite(d->alien_sprite[i], ALIEN_WIDTH, ALIEN_TARGET_WIDTH * w);
  for(int i = 0; i < 3; i++) _scale_sprite(d->player_sprite[i], PLAYER_WIDTH, PLAYER_TARGET_WIDTH * w);
  _scale_sprite(d->mystery_sprite, MYSTERY_SHIP_WIDTH, MYSTERY_SHIP_TARGET_WIDTH * w);
  for(int i = 0; i < EXPLOSION_AMOUNT; i++)
    _scale_sprite(d->explosion_sprite[i], EXPLOSION_WIDTH, EXPLOSION_TARGET_WIDTH * w);
  for(int i = 0; i < 4; i++) _scale_sprite(d->bunker_sprite[i], BUNKER_WIDTH, BUNKER_TARGET_WIDTH * w);
  for(int i = 0; i < n_letters; i++) _scale_sprite(d->letters[i], FONT_WIDTH, LETTER_WIDTH * w);

  // clear background
  cairo_set_source_rgb(cr, 0, 0, 0);
  cairo_paint(cr);

  switch(d->game_state)
  {
    case INTRO:
    case START:
    case LOSE:
    case WIN:
      _expose_intro(d, cr, w, h);
      break;
    case GAME:
      _expose_game(d, cr, w, h);
      break;
    default:
      break;
  }

  cairo_restore(cr);
}

static gboolean _key_release(GtkWidget *w, GdkEventKey *event, dt_knight_t *d)
{
  switch(event->keyval)
  {
    case GDK_KEY_Left:
    case GDK_KEY_Right:
      d->move = 0;
      return 1;
  }
  return 0;
}

static gboolean _key_press(GtkWidget *w, GdkEventKey *event, dt_knight_t *d)
{
  switch(event->keyval)
  {
    // do movement in the event loop
    case GDK_KEY_Left:
      d->move = -1;
      return 1;
    case GDK_KEY_Right:
      d->move = 1;
      return 1;
    case GDK_KEY_space:
      if(!d->player_shot.active && !d->total_freeze)
      {
        d->player_shot.active = TRUE;
        d->player_shot.x = d->player_x + 0.5 * PLAYER_TARGET_WIDTH;
        d->player_shot.y = d->player_shot.start = PLAYER_Y;
        d->player_shot.direction = 1.0;
      }
      return 1;
  }

  return 0;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

