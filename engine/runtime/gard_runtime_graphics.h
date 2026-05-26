// Gard Graphics Runtime — SDL2-based window, rendering, input, audio
// Production-grade: hardware-accelerated rendering, event loop, game loop support
// Link: -lSDL2 -lSDL2_ttf -lSDL2_image -lSDL2_mixer

#ifndef GARD_RUNTIME_GRAPHICS_H
#define GARD_RUNTIME_GRAPHICS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handles
typedef struct GardWindow GardWindow;

// === Window ===
GardWindow* gard_window_create(const char* title, int32_t width, int32_t height);
void gard_window_destroy(GardWindow* win);
int32_t gard_window_is_open(GardWindow* win);
void gard_window_set_title(GardWindow* win, const char* title);
int32_t gard_window_width(GardWindow* win);
int32_t gard_window_height(GardWindow* win);

// === Rendering ===
void gard_graphics_clear(GardWindow* win);
void gard_graphics_clear_color(GardWindow* win, int32_t r, int32_t g, int32_t b);
void gard_graphics_present(GardWindow* win);
void gard_graphics_set_color(GardWindow* win, int32_t r, int32_t g, int32_t b, int32_t a);
void gard_graphics_draw_rect(GardWindow* win, int32_t x, int32_t y, int32_t w, int32_t h);
void gard_graphics_fill_rect(GardWindow* win, int32_t x, int32_t y, int32_t w, int32_t h);
void gard_graphics_draw_line(GardWindow* win, int32_t x1, int32_t y1, int32_t x2, int32_t y2);
void gard_graphics_draw_point(GardWindow* win, int32_t x, int32_t y);
void gard_graphics_draw_circle(GardWindow* win, int32_t cx, int32_t cy, int32_t radius);
void gard_graphics_fill_circle(GardWindow* win, int32_t cx, int32_t cy, int32_t radius);

// === Input (polled per frame) ===
void gard_input_poll(GardWindow* win);
int32_t gard_input_is_key_down(int32_t key_code);
int32_t gard_input_is_key_pressed(int32_t key_code);
int32_t gard_input_mouse_x(void);
int32_t gard_input_mouse_y(void);
int32_t gard_input_mouse_button(int32_t button);
int32_t gard_input_quit_requested(void);

// === Timing ===
void gard_graphics_delay(int32_t ms);
int32_t gard_graphics_get_ticks(void);
void gard_graphics_set_fps(GardWindow* win, int32_t fps);

// === Key codes (subset matching SDL scancodes) ===
// Use these with gard_input_is_key_down
// A=4, B=5, ..., Z=29, 1=30, ..., 0=39
// RETURN=40, ESCAPE=41, BACKSPACE=42, TAB=43, SPACE=44
// RIGHT=79, LEFT=80, DOWN=81, UP=82

#ifdef __cplusplus
}
#endif

#endif // GARD_RUNTIME_GRAPHICS_H
