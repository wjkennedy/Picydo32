#ifndef PICO8_CART_H
#define PICO8_CART_H

#include <stdint.h>

struct GameCart {
    const uint8_t name_len;
    const char* name;

    const uint16_t code_len;
    const uint8_t* code;

    const uint16_t gff_len;
    const uint8_t* gff;

    const uint16_t gfx_len;
    const uint8_t* gfx;

    const uint16_t sfx_len;
    const uint8_t* sfx;

    const uint16_t map_len;
    const uint8_t* map;

    const uint16_t label_len;
    const uint8_t* label;
};

typedef struct GameCart GameCart;

#endif
