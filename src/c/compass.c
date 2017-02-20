#include <pebble.h>
#include "compass.h"

static Window *s_window;
static Layer *s_back_layer = NULL;
static Layer *s_time_layer = NULL;
static Layer *s_digits_layer = NULL;
static Layer *s_needle_layer = NULL;
static Layer *s_charge_layer = NULL;

static char s_time_buffer[16] = "";
static char s_head_buffer[16] = "";
static BatteryChargeState g_charge;
static CompassHeadingData g_heading;
static int16_t g_degrees = -1;
static int16_t g_second = 0;

static GFont s_font;

// Needle colors, ARGB8
#define ARROW_COLOR_N GColorBlueMoonARGB8
#define ARROW_COLOR_S GColorOrangeARGB8

#define A 12 // (needle width / 2)
#define B 48 // (needle length / 2)

// These constants to be recalculated when A or B is changed
#define L 3242542  // (65536 * sqrt(A * A + B * B))
#define RL 1325    // (65536 / sqrt(A * A + B * B))

static const GPathInfo PATH_INFO_N = {
  .num_points = 3,
  .points = (GPoint []) {{-7, -67}, { 0, -60}, { 7, -67}}
};

static const GPathInfo PATH_INFO_S = {
  .num_points = 3,
  .points = (GPoint []) {{ 7,  67}, { 0,  60}, {-7,  67}}
};

static const GPathInfo PATH_INFO_W = {
  .num_points = 3,
  .points = (GPoint []) {{-67, -7}, { -60, 0}, { -67,  7}}
};

static const GPathInfo PATH_INFO_E = {
  .num_points = 3,
  .points = (GPoint []) {{ 67,  7}, {  60, 0}, {  67, -7}}
};

static struct color_map charge_colors[] = {
    { 100, 0x00FF55 },
    {  90, 0x00FF00 },
    {  80, 0x55FF00 },
    {  70, 0xAAFF00 },
    {  50, 0xFFFF00 },
    {  30, 0xFFAA00 },
    {  20, 0xFF5500 },
    {   0, 0xFF0000 },
    {  -1, 0xFFFFFF }
};

// multiply two fixed point numbers in range [-1, 1]
int32_t ratio_mul(int32_t a, int32_t b)
{
  int32_t x;

  if (a < 0)
    a -= 1;
  if (a > 0)
    a += 1;
  if (b < 0)
    b -= 1;
  if (b > 0)
    b += 1;

  x = (a >> 1) * (b >> 1);
  if (x < 0)
    x += (1 << 13);
  if (x > 0)
    x -= (1 << 13);
  x >>= 14;
  
  return x;
}

// blend two ARGB8 colors (alpha = 0 .. 65536)
static uint8_t blend(int32_t alpha, uint8_t c0, uint8_t c1)
{
  uint32_t beta = (1 << 16) - alpha;
  uint32_t r0 = (c0 & 0x30) >> 4;
  uint32_t g0 = (c0 & 0x0C) >> 2;
  uint32_t b0 = (c0 & 0x03);
  uint32_t r1 = (c1 & 0x30) >> 4;
  uint32_t g1 = (c1 & 0x0C) >> 2;
  uint32_t b1 = (c1 & 0x03);
  uint32_t x;
  uint8_t c = 3;

  c <<= 2;
  x = r1 * alpha + r0 * beta + (1 << 15);
  x >>= 16;
  c |= x;
  c <<= 2;
  x = g1 * alpha + g0 * beta + (1 << 15);
  x >>= 16;
  c |= x;
  c <<= 2;
  x = b1 * alpha + b0 * beta + (1 << 15);
  x >>= 16;
  c |= x;
  
  return c;
}

static GPath *path_n = NULL;
static GPath *path_s = NULL;
static GPath *path_w = NULL;
static GPath *path_e = NULL;

// get battery charge indicator color
GColor get_charging_color(int percent)
{
  struct color_map *cm = charge_colors;
  GColor c;

  if (percent < 0)
    percent = 0;
  
  do {
    c = GColorFromHEX(cm->color);
    cm += 1;
  } while (percent <= cm->key);

  return c;
}

static void back_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  GPoint center = grect_center_point(&bounds);

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);

  graphics_context_set_stroke_width(ctx, 1);
  graphics_context_set_stroke_color(ctx, GColorDarkGray);

  graphics_context_set_fill_color(ctx, GColorIcterine);

  for (int i = 0; i < 8; i += 1) {
    gpath_move_to(path_n, center);
    gpath_move_to(path_s, center);
    gpath_move_to(path_w, center);
    gpath_move_to(path_e, center);

    gpath_draw_filled(ctx, path_n);
    gpath_draw_filled(ctx, path_s);
    gpath_draw_filled(ctx, path_w);
    gpath_draw_filled(ctx, path_e);
  }
}

static void charge_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  GPoint center = grect_center_point(&bounds);
  int c = g_charge.charge_percent;
  GRect charge_frame = {
#ifdef PBL_ROUND
    .origin = { center.x - 21, center.x + 74 },
    .size = { 42, 8 },
#else
    .origin = { center.x - 31, bounds.size.h - 8},
    .size = { 62, 8 },
#endif
  };

  GRect charge_level = {
#ifdef PBL_ROUND
    .origin = { center.x - 20, center.y + 75  },
    .size = { (c / 5) * 2, 6 },
#else
    .origin = { center.x - 30, bounds.size.h - 7 },
    .size = { (c / 5) * 3, 6 },
#endif
  };

  GColor color = get_charging_color(g_charge.charge_percent);
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_context_set_fill_color(ctx, color);
  graphics_fill_rect(ctx, charge_level, 0, GCornerNone);
  graphics_draw_rect(ctx, charge_frame);
}

static void time_update_proc(Layer *layer, GContext *ctx) {
#ifdef PBL_ROUND
  GRect bounds = layer_get_bounds(layer);
  GPoint center = grect_center_point(&bounds);
  GRect time_rect = GRect(center.x - 20, center.y - 88, 64, 16);
#else
  GRect time_rect = GRect(0, 0, 64, 16);
#endif
  
  time_t now = time(NULL);
  struct tm *t = localtime(&now);

  if (clock_is_24h_style()) {
    strftime(s_time_buffer, 16, "%H:%M", t);
  } else {
    strftime(s_time_buffer, 16, "%I:%M%P", t);
  }

  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, s_time_buffer, s_font, time_rect,
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void digits_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  GPoint center = grect_center_point(&bounds);

#ifdef PBL_ROUND
  GRect head_rect = GRect(center.x + 8, center.y - 72, 48, 16);
  GPoint status_center = GPoint(center.x + 48, center.y + 48);
#else
  GRect head_rect = GRect(80, 0, 48, 16);
  GPoint status_center = GPoint(134, 10);
#endif

  snprintf(s_head_buffer, 16, " %03d°", g_degrees);

  graphics_context_set_fill_color(ctx, GColorDarkGray);
  graphics_context_set_text_color(ctx, GColorWhite);

  if(g_heading.compass_status == CompassStatusDataInvalid) {
    graphics_context_set_fill_color(ctx, GColorRed);
    strcpy(s_head_buffer, " ----");
  }

  if(g_heading.compass_status == CompassStatusCalibrating) {
    if (g_second & 1)
      graphics_context_set_fill_color(ctx, GColorChromeYellow);
    else
      graphics_context_set_fill_color(ctx, GColorArmyGreen);
  }

  if(g_heading.compass_status == CompassStatusCalibrated)
    graphics_context_set_fill_color(ctx, GColorMediumSpringGreen);

  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_fill_circle(ctx, status_center, 8);
  graphics_draw_circle(ctx, status_center, 8);

  graphics_draw_text(ctx, s_head_buffer, s_font, head_rect,
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void needle_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  GPoint center = grect_center_point(&bounds);
  GBitmap *fb = graphics_capture_frame_buffer(ctx);

  int32_t needle_angle = g_heading.magnetic_heading;
  const int32_t r = (A * B) << 16;

  int32_t sy = 0;
  int32_t sx = 0;
  int32_t alpha;
  uint8_t c;
  int x, y;

  if(g_heading.compass_status == CompassStatusDataInvalid)
    return;

  sy = sin_lookup(needle_angle);
  sx = cos_lookup(needle_angle);

  // Iterate over all rows
  for (y = center.y - 56; y <= center.y + 56; y += 1) {
    // Get this row's range and data
    GBitmapDataRowInfo info = gbitmap_get_data_row_info(fb, y);

    // Iterate over all visible columns
    for (x = info.min_x; x <= info.max_x; x += 1) {
      int32_t dx = x - center.x;
      int32_t dy = y - center.y;
      
      // Map current pixel into rotating needle's plane
      int32_t rx = dx * sx + dy * sy;
      int32_t ry = dy * sx - dx * sy;

      // distance from the nearest edge (inside: d < 0, outside: d > 0)
      int32_t ld = abs(rx) * B + abs(ry) * A - r;

      // Out of the needle area
      if (ld > L * 2)
        continue;

      // Diamond outline
      if (ld >= 0) {
        if  (ld < L)
          c = GColorWhiteARGB8;
        else {
          alpha = ratio_mul(ld - L, RL);
          c = blend(alpha, GColorWhiteARGB8, info.data[x]);
        }
        info.data[x] = c;
        continue;
      }
      
      // Line between blue and red
      if (abs(ry) < 2 * TRIG_MAX_RATIO) {
        c = GColorWhiteARGB8;
        if (ry + TRIG_MAX_RATIO < 0) {
          alpha = abs(ry) - TRIG_MAX_RATIO;
          c = blend(alpha, c, ARROW_COLOR_N);
          info.data[x] = c;
          continue;
        }
        if (ry - TRIG_MAX_RATIO > 0) {
          alpha = abs(ry) - TRIG_MAX_RATIO;
          c = blend(alpha, c, ARROW_COLOR_S);
          info.data[x] = c;
          continue;
        }
        info.data[x] = c;
        continue;
      }

      // Blue filled triangle
      if (ry < 0) {
          c = ARROW_COLOR_N;
        if (ld + L > 0) {
          alpha = ratio_mul(L + ld, RL);
          c = blend(alpha, c, GColorWhiteARGB8);
        }
        info.data[x] = c;
      }
      
      // Red filled triangle
      if (ry > 0) {
          c = ARROW_COLOR_S;
        if (ld + L > 0) {
          alpha = ratio_mul(L + ld, RL);
          c = blend(alpha, c, GColorWhiteARGB8);
        }
        info.data[x] = c;
      }
    }
  }
  graphics_release_frame_buffer(ctx, fb);

  int16_t deg = TRIGANGLE_TO_DEG(needle_angle);
  if (deg != g_degrees) {
    g_degrees = deg;
    layer_mark_dirty(s_digits_layer);
  }
}

static void handle_second_tick(struct tm *tick_time, TimeUnits units_changed) {
  g_second = tick_time->tm_sec;
  layer_mark_dirty(s_digits_layer);
}

static void charge_handler(BatteryChargeState charge)
{
  g_charge = charge;
  if (s_charge_layer) {
    layer_mark_dirty(s_charge_layer);
  }
}

static void heading_handler(CompassHeadingData heading) 
{
  g_heading = heading;

  layer_mark_dirty(s_needle_layer);
}
  
static void window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  path_n = gpath_create(&PATH_INFO_N);
  path_s = gpath_create(&PATH_INFO_S);
  path_w = gpath_create(&PATH_INFO_W);
  path_e = gpath_create(&PATH_INFO_E);

  s_back_layer = layer_create(bounds);
  layer_set_update_proc(s_back_layer, back_update_proc);
  layer_add_child(window_layer, s_back_layer);

  s_time_layer = layer_create(bounds);
  layer_set_update_proc(s_time_layer, time_update_proc);
  layer_add_child(window_layer, s_time_layer);

  s_digits_layer = layer_create(bounds);
  layer_set_update_proc(s_digits_layer, digits_update_proc);
  layer_add_child(window_layer, s_digits_layer);

  s_needle_layer = layer_create(bounds);
  layer_set_update_proc(s_needle_layer, needle_update_proc);
  layer_add_child(window_layer, s_needle_layer);

  s_charge_layer = layer_create(bounds);
  layer_set_update_proc(s_charge_layer, charge_update_proc);
  layer_add_child(window_layer, s_charge_layer);
}

static void window_unload(Window *window) {
  layer_destroy(s_back_layer);
  layer_destroy(s_time_layer);
  layer_destroy(s_digits_layer);
  layer_destroy(s_needle_layer);
  layer_destroy(s_charge_layer);
  
  gpath_destroy(path_n);
  gpath_destroy(path_s);
  gpath_destroy(path_w);
  gpath_destroy(path_e);
}

static void init() {
  s_font = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_PERFECT_16));
  g_charge = battery_state_service_peek();
  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);

  s_time_buffer[0] = '\0';
  s_head_buffer[0] = '\0';

  battery_state_service_subscribe(charge_handler);
  tick_timer_service_subscribe(SECOND_UNIT, handle_second_tick);
  compass_service_subscribe(heading_handler);
}

static void deinit() {
  tick_timer_service_unsubscribe();
  battery_state_service_unsubscribe();
  window_destroy(s_window);
}

int main() {
  init();
  app_event_loop();
  deinit();
}
