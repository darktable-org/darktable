#pragma once

/*

 #include <stdio.h>

 int main()
 {
  for(int i = 0; i < 32; i++)
  {
    int bit[5];
    for(int j = 0; j < 5; j++) bit[j] = i & (1 << j);
    printf("#define ");
    for(int j = 4; j >= 0; j--) printf("%c", bit[j] ? 'X' : '_');
    printf(" ");
    for(int j = 4; j >= 0; j--) printf("%s%s", bit[j] ? "0xff" : "0x00", j ? ", " : "");
    printf("\n");
  }
  return 0;
 }

*/

#define _____ 0x00, 0x00, 0x00, 0x00, 0x00
#define ____X 0x00, 0x00, 0x00, 0x00, 0xff
#define ___X_ 0x00, 0x00, 0x00, 0xff, 0x00
#define ___XX 0x00, 0x00, 0x00, 0xff, 0xff
#define __X__ 0x00, 0x00, 0xff, 0x00, 0x00
#define __X_X 0x00, 0x00, 0xff, 0x00, 0xff
#define __XX_ 0x00, 0x00, 0xff, 0xff, 0x00
#define __XXX 0x00, 0x00, 0xff, 0xff, 0xff
#define _X___ 0x00, 0xff, 0x00, 0x00, 0x00
#define _X__X 0x00, 0xff, 0x00, 0x00, 0xff
#define _X_X_ 0x00, 0xff, 0x00, 0xff, 0x00
#define _X_XX 0x00, 0xff, 0x00, 0xff, 0xff
#define _XX__ 0x00, 0xff, 0xff, 0x00, 0x00
#define _XX_X 0x00, 0xff, 0xff, 0x00, 0xff
#define _XXX_ 0x00, 0xff, 0xff, 0xff, 0x00
#define _XXXX 0x00, 0xff, 0xff, 0xff, 0xff
#define X____ 0xff, 0x00, 0x00, 0x00, 0x00
#define X___X 0xff, 0x00, 0x00, 0x00, 0xff
#define X__X_ 0xff, 0x00, 0x00, 0xff, 0x00
#define X__XX 0xff, 0x00, 0x00, 0xff, 0xff
#define X_X__ 0xff, 0x00, 0xff, 0x00, 0x00
#define X_X_X 0xff, 0x00, 0xff, 0x00, 0xff
#define X_XX_ 0xff, 0x00, 0xff, 0xff, 0x00
#define X_XXX 0xff, 0x00, 0xff, 0xff, 0xff
#define XX___ 0xff, 0xff, 0x00, 0x00, 0x00
#define XX__X 0xff, 0xff, 0x00, 0x00, 0xff
#define XX_X_ 0xff, 0xff, 0x00, 0xff, 0x00
#define XX_XX 0xff, 0xff, 0x00, 0xff, 0xff
#define XXX__ 0xff, 0xff, 0xff, 0x00, 0x00
#define XXX_X 0xff, 0xff, 0xff, 0x00, 0xff
#define XXXX_ 0xff, 0xff, 0xff, 0xff, 0x00
#define XXXXX 0xff, 0xff, 0xff, 0xff, 0xff

#define FONT_WIDTH 5
#define FONT_HEIGHT 9
static const uint8_t font[][FONT_WIDTH * FONT_HEIGHT] = {
  // SPACE
  { 0x00 },
  // !
  {
    _____,
    __X__,
    __X__,
    __X__,
    __X__,
    __X__,
    _____,
    __X__,
    _____,
  },
  // "
  {
    _____,
    _X_X_,
    _X_X_,
    _____,
    _____,
    _____,
    _____,
    _____,
    _____,
  },
  // #
  {
    _____,
    _X_X_,
    _X_X_,
    XXXXX,
    _X_X_,
    XXXXX,
    _X_X_,
    _X_X_,
    _____,
  },
  // $
  {
    __X__,
    _XXX_,
    X_X_X,
    X_X__,
    _XXX_,
    __X_X,
    X_X_X,
    _XXX_,
    __X__,
  },
  // %
  {
    _____,
    XX_X_,
    XX_X_,
    __X__,
    __X__,
    __X__,
    _X_XX,
    _X_XX,
    _____,
  },
  // &
  {
    _____,
    _XX__,
    X__X_,
    X__X_,
    _XX__,
    X__X_,
    X___X,
    _XXXX,
    _____,
  },
  // '
  {
    _____,
    ___X_,
    __X__,
    _____,
    _____,
    _____,
    _____,
    _____,
    _____,
  },
  // (
  {
    _____,
    ___X_,
    __X__,
    _X___,
    _X___,
    _X___,
    __X__,
    ___X_,
    _____,
  },
  // )
  {
    _____,
    _X___,
    __X__,
    ___X_,
    ___X_,
    ___X_,
    __X__,
    _X___,
    _____,
  },
  // *
  {
    _____,
    __X__,
    X_X_X,
    _XXX_,
    __X__,
    _XXX_,
    X_X_X,
    __X__,
    _____,
  },
  // +
  {
    _____,
    _____,
    __X__,
    __X__,
    XXXXX,
    __X__,
    __X__,
    _____,
    _____,
  },
  // ,
  {
    _____,
    _____,
    _____,
    _____,
    _____,
    _____,
    _____,
    __X__,
    __X__,
  },
  // -
  {
    _____,
    _____,
    _____,
    _____,
    XXXXX,
    _____,
    _____,
    _____,
    _____,
  },
  // .
  {
    _____,
    _____,
    _____,
    _____,
    _____,
    _____,
    _____,
    __X__,
    _____,
  },
  // /
  {
    _____,
    ___X_,
    ___X_,
    __X__,
    __X__,
    __X__,
    _X___,
    _X___,
    _____,
  },
  // 0
  {
    _____,
    _XXX_,
    X___X,
    X__XX,
    X_X_X,
    XX__X,
    X___X,
    _XXX_,
    _____,
  },
  // 1
  {
    _____,
    __X__,
    _XX__,
    __X__,
    __X__,
    __X__,
    __X__,
    _XXX_,
    _____,
  },
  // 2
  {
    _____,
    _XXX_,
    X___X,
    ____X,
    __XX_,
    _X___,
    X____,
    XXXXX,
    _____,
  },
  // 3
  {
    _____,
    XXXXX,
    ____X,
    ___X_,
    __XX_,
    ____X,
    X___X,
    _XXX_,
    _____,
  },
  // 4
  {
    _____,
    ___X_,
    __XX_,
    _X_X_,
    X__X_,
    XXXXX,
    ___X_,
    ___X_,
    _____,
  },
  // 5
  {
    _____,
    XXXXX,
    X____,
    X____,
    _XXX_,
    ____X,
    X___X,
    _XXX_,
    _____,
  },
  // 6
  {
    _____,
    __XXX,
    _X___,
    X____,
    XXXX_,
    X___X,
    X___X,
    _XXX_,
    _____,
  },
  // 7
  {
    _____,
    XXXXX,
    ____X,
    ___X_,
    __X__,
    _X___,
    _X___,
    _X___,
    _____,
  },
  // 8
  {
    _____,
    _XXX_,
    X___X,
    X___X,
    _XXX_,
    X___X,
    X___X,
    _XXX_,
    _____,
  },
  // 9
  {
    _____,
    _XXX_,
    X___X,
    X___X,
    _XXXX,
    ____X,
    ___X_,
    XXX__,
    _____,
  },
  // :
  {
    _____,
    _____,
    __X__,
    _____,
    _____,
    _____,
    _____,
    __X__,
    _____,
  },
  // ;
  {
    _____,
    _____,
    __X__,
    _____,
    _____,
    _____,
    _____,
    __X__,
    __X__,
  },
  // <
  {
    _____,
    ___X_,
    __X__,
    _X___,
    X____,
    _X___,
    __X__,
    ___X_,
    _____,
  },
  // =
  {
    _____,
    _____,
    _____,
    _XXXX,
    _____,
    _XXXX,
    _____,
    _____,
    _____,
  },
  // >
  {
    _____,
    _X___,
    __X__,
    ___X_,
    ____X,
    ___X_,
    __X__,
    _X___,
    _____,
  },
  // ?
  {
    _____,
    _XXX_,
    X___X,
    ___X_,
    __X__,
    __X__,
    _____,
    __X__,
    _____,
  },
  // @
  {
    _____,
    _XXX_,
    X___X,
    X_X_X,
    XX_XX,
    X_X__,
    X___X,
    _XXX_,
    _____,
  },
  // A
  {
    _____,
    __X__,
    _X_X_,
    X___X,
    X___X,
    XXXXX,
    X___X,
    X___X,
    _____,
  },
  // B
  {
    _____,
    XXXX_,
    X___X,
    X___X,
    XXXX_,
    X___X,
    X___X,
    XXXX_,
    _____,
  },
  // C
  {
    _____,
    _XXX_,
    X___X,
    X____,
    X____,
    X____,
    X___X,
    _XXX_,
    _____,
  },
  // D
  {
    _____,
    XXXX_,
    X___X,
    X___X,
    X___X,
    X___X,
    X___X,
    XXXX_,
    _____,
  },
  // E
  {
    _____,
    XXXXX,
    X____,
    X____,
    XXXX_,
    X____,
    X____,
    XXXXX,
    _____,
  },
  // F
  {
    _____,
    XXXXX,
    X____,
    X____,
    XXXX_,
    X____,
    X____,
    X____,
    _____,
  },
  // G
  {
    _____,
    _XXX_,
    X___X,
    X____,
    X_XXX,
    X___X,
    X___X,
    _XXX_,
    _____,
  },
  // H
  {
    _____,
    X___X,
    X___X,
    X___X,
    XXXXX,
    X___X,
    X___X,
    X___X,
    _____,
  },
  // I
  {
    _____,
    _XXX_,
    __X__,
    __X__,
    __X__,
    __X__,
    __X__,
    _XXX_,
    _____,
  },
  // J
  {
    _____,
    ____X,
    ____X,
    ____X,
    ____X,
    ____X,
    X___X,
    _XXX_,
    _____,
  },
  // K
  {
    _____,
    X___X,
    X__X_,
    X_X__,
    XX___,
    X_X__,
    X__X_,
    X___X,
    _____,
  },
  // L
  {
    _____,
    X____,
    X____,
    X____,
    X____,
    X____,
    X____,
    XXXXX,
    _____,
  },
  // M
  {
    _____,
    X___X,
    XX_XX,
    X_X_X,
    X_X_X,
    X___X,
    X___X,
    X___X,
    _____,
  },
  // N
  {
    _____,
    X___X,
    X___X,
    XX__X,
    X_X_X,
    X__XX,
    X___X,
    X___X,
    _____,
  },
  // O
  {
    _____,
    _XXX_,
    X___X,
    X___X,
    X___X,
    X___X,
    X___X,
    _XXX_,
    _____,
  },
  // P
  {
    _____,
    XXXX_,
    X___X,
    X___X,
    XXXX_,
    X____,
    X____,
    X____,
    _____,
  },
  // Q
  {
    _____,
    _XXX_,
    X___X,
    X___X,
    X___X,
    X_X_X,
    X__XX,
    _XXXX,
    _____,
  },
  // R
  {
    _____,
    XXXX_,
    X___X,
    X___X,
    XXXX_,
    X_X__,
    X__X_,
    X___X,
    _____,
  },
  // S
  {
    _____,
    _XXX_,
    X___X,
    X____,
    _XXX_,
    ____X,
    X___X,
    _XXX_,
    _____,
  },
  // T
  {
    _____,
    XXXXX,
    __X__,
    __X__,
    __X__,
    __X__,
    __X__,
    __X__,
    _____,
  },
  // U
  {
    _____,
    X___X,
    X___X,
    X___X,
    X___X,
    X___X,
    X___X,
    _XXX_,
    _____,
  },
  // V
  {
    _____,
    X___X,
    X___X,
    X___X,
    X___X,
    X___X,
    _X_X_,
    __X__,
    _____,
  },
  // W
  {
    _____,
    X___X,
    X___X,
    X___X,
    X_X_X,
    X_X_X,
    XX_XX,
    X___X,
    _____,
  },
  // X
  {
    _____,
    X___X,
    X___X,
    _X_X_,
    __X__,
    _X_X_,
    X___X,
    X___X,
    _____,
  },
  // Y
  {
    _____,
    X___X,
    X___X,
    _X_X_,
    __X__,
    __X__,
    __X__,
    __X__,
    _____,
  },
  // Z
  {
    _____,
    XXXXX,
    ____X,
    ___X_,
    __X__,
    _X___,
    X____,
    XXXXX,
    _____,
  },
  // [
  {
    _____,
    __XX_,
    _X___,
    _X___,
    _X___,
    _X___,
    _X___,
    __XX_,
    _____,
  },
  // \ .
  {
    _____,
    _X___,
    _X___,
    __X__,
    __X__,
    __X__,
    ___X_,
    ___X_,
    _____,
  },
  // ]
  {
    _____,
    _XX__,
    ___X_,
    ___X_,
    ___X_,
    ___X_,
    ___X_,
    _XX__,
    _____,
  },
  // ^
  {
    _____,
    __X__,
    _X_X_,
    X___X,
    _____,
    _____,
    _____,
    _____,
    _____,
  },
  // _
  {
    _____,
    _____,
    _____,
    _____,
    _____,
    _____,
    _____,
    XXXXX,
    _____,
  },
  // `
  {
    _____,
    __X__,
    ___X_,
    _____,
    _____,
    _____,
    _____,
    _____,
    _____,
  },
  // a
  {
    _____,
    __X__,
    _X_X_,
    X___X,
    X___X,
    XXXXX,
    X___X,
    X___X,
    _____,
  },
  // b
  {
    _____,
    XXXX_,
    X___X,
    X___X,
    XXXX_,
    X___X,
    X___X,
    XXXX_,
    _____,
  },
  // c
  {
    _____,
    _XXX_,
    X___X,
    X____,
    X____,
    X____,
    X___X,
    _XXX_,
    _____,
  },
  // d
  {
    _____,
    XXXX_,
    X___X,
    X___X,
    X___X,
    X___X,
    X___X,
    XXXX_,
    _____,
  },
  // e
  {
    _____,
    XXXXX,
    X____,
    X____,
    XXXX_,
    X____,
    X____,
    XXXXX,
    _____,
  },
  // f
  {
    _____,
    XXXXX,
    X____,
    X____,
    XXXX_,
    X____,
    X____,
    X____,
    _____,
  },
  // g
  {
    _____,
    _XXX_,
    X___X,
    X____,
    X_XXX,
    X___X,
    X___X,
    _XXX_,
    _____,
  },
  // h
  {
    _____,
    X___X,
    X___X,
    X___X,
    XXXXX,
    X___X,
    X___X,
    X___X,
    _____,
  },
  // i
  {
    _____,
    _XXX_,
    __X__,
    __X__,
    __X__,
    __X__,
    __X__,
    _XXX_,
    _____,
  },
  // j
  {
    _____,
    ____X,
    ____X,
    ____X,
    ____X,
    ____X,
    X___X,
    _XXX_,
    _____,
  },
  // k
  {
    _____,
    X___X,
    X__X_,
    X_X__,
    XX___,
    X_X__,
    X__X_,
    X___X,
    _____,
  },
  // l
  {
    _____,
    X____,
    X____,
    X____,
    X____,
    X____,
    X____,
    XXXXX,
    _____,
  },
  // m
  {
    _____,
    X___X,
    XX_XX,
    X_X_X,
    X_X_X,
    X___X,
    X___X,
    X___X,
    _____,
  },
  // n
  {
    _____,
    X___X,
    X___X,
    XX__X,
    X_X_X,
    X__XX,
    X___X,
    X___X,
    _____,
  },
  // o
  {
    _____,
    _XXX_,
    X___X,
    X___X,
    X___X,
    X___X,
    X___X,
    _XXX_,
    _____,
  },
  // p
  {
    _____,
    XXXX_,
    X___X,
    X___X,
    XXXX_,
    X____,
    X____,
    X____,
    _____,
  },
  // q
  {
    _____,
    _XXX_,
    X___X,
    X___X,
    X___X,
    X_X_X,
    X__XX,
    _XXXX,
    _____,
  },
  // r
  {
    _____,
    XXXX_,
    X___X,
    X___X,
    XXXX_,
    X_X__,
    X__X_,
    X___X,
    _____,
  },
  // s
  {
    _____,
    _XXX_,
    X___X,
    X____,
    _XXX_,
    ____X,
    X___X,
    _XXX_,
    _____,
  },
  // t
  {
    _____,
    XXXXX,
    __X__,
    __X__,
    __X__,
    __X__,
    __X__,
    __X__,
    _____,
  },
  // u
  {
    _____,
    X___X,
    X___X,
    X___X,
    X___X,
    X___X,
    X___X,
    _XXX_,
    _____,
  },
  // v
  {
    _____,
    X___X,
    X___X,
    X___X,
    X___X,
    X___X,
    _X_X_,
    __X__,
    _____,
  },
  // w
  {
    _____,
    X___X,
    X___X,
    X___X,
    X_X_X,
    X_X_X,
    XX_XX,
    X___X,
    _____,
  },
  // x
  {
    _____,
    X___X,
    X___X,
    _X_X_,
    __X__,
    _X_X_,
    X___X,
    X___X,
    _____,
  },
  // y
  {
    _____,
    X___X,
    X___X,
    _X_X_,
    __X__,
    __X__,
    __X__,
    __X__,
    _____,
  },
  // z
  {
    _____,
    XXXXX,
    ____X,
    ___X_,
    __X__,
    _X___,
    X____,
    XXXXX,
    _____,
  },
  // {
  {
    _____,
    ___X_,
    __X__,
    __X__,
    _X___,
    __X__,
    __X__,
    ___X_,
    _____,
  },
  // |
  {
    _____,
    __X__,
    __X__,
    __X__,
    __X__,
    __X__,
    __X__,
    __X__,
    _____,
  },
  // }
  {
    _____,
    _X___,
    __X__,
    __X__,
    ___X_,
    __X__,
    __X__,
    _X___,
    _____,
  },
  // ~
  {
    _____,
    _____,
    _____,
    __X_X,
    _X_X_,
    _____,
    _____,
    _____,
    _____,
  }
};

static const int n_letters = sizeof(font) / sizeof(*font);

#undef _____
#undef ____X
#undef ___X_
#undef ___XX
#undef __X__
#undef __X_X
#undef __XX_
#undef __XXX
#undef _X___
#undef _X__X
#undef _X_X_
#undef _X_XX
#undef _XX__
#undef _XX_X
#undef _XXX_
#undef _XXXX
#undef X____
#undef X___X
#undef X__X_
#undef X__XX
#undef X_X__
#undef X_X_X
#undef X_XX_
#undef X_XXX
#undef XX___
#undef XX__X
#undef XX_X_
#undef XX_XX
#undef XXX__
#undef XXX_X
#undef XXXX_
#undef XXXXX

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
