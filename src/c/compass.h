#pragma once
#include <pebble.h>

#define RGB222(r, g, b) ((3 << 6) | (r << 4) | (g << 2) | b)

struct color_map {
  int key;
  int color;
};
