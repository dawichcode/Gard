// Gard Graphics Runtime — SDL2 implementation
// Hardware-accelerated 2D rendering, input, timing
// Compile: gcc -c -O2 -fPIC gard_runtime_graphics.c -o gard_runtime_graphics.o $(sdl2-config --cflags)
// Link: $(sdl2-config --libs)

#include "gard_runtime_graphics.h"
#include <SDL2/SDL.h>
#include <string.h>

// ============================================================
// Internal state
// ============================================================

struct GardWindow {
    SDL_Window* window;
    SDL_Renderer* renderer;
    int32_t width;
    int32_t height;
    int32_t open;
    int32_t target_fps;
    uint32_t frame_start;
};

static int g_sdl_initialized = 0;
static const uint8_t* g_keyboard_state = NULL;
static uint8_t g_prev_keyboard[512] = {0};
static int32_t g_mouse_x = 0, g_mouse_y = 0;
static uint32_t g_mouse_buttons = 0;
static int32_t g_quit_requested = 0;

static void ensure_sdl_init(void) {
    if (!g_sdl_initialized) {
        SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER);
        g_sdl_initialized = 1;
    }
}

// ============================================================
// Window
// ============================================================

GardWindow* gard_window_create(const char* title, int32_t width, int32_t height) {
    ensure_sdl_init();
    if (width <= 0) width = 800;
    if (height <= 0) height = 600;

    SDL_Window* window = SDL_CreateWindow(
        title ? title : "Gard",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width, height,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );
    if (!window) return NULL;

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        // Fallback to software renderer
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
        if (!renderer) { SDL_DestroyWindow(window); return NULL; }
    }

    // Enable alpha blending
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    GardWindow* win = (GardWindow*)calloc(1, sizeof(GardWindow));
    win->window = window;
    win->renderer = renderer;
    win->width = width;
    win->height = height;
    win->open = 1;
    win->target_fps = 60;
    win->frame_start = SDL_GetTicks();

    g_keyboard_state = SDL_GetKeyboardState(NULL);
    return win;
}

void gard_window_destroy(GardWindow* win) {
    if (!win) return;
    if (win->renderer) SDL_DestroyRenderer(win->renderer);
    if (win->window) SDL_DestroyWindow(win->window);
    win->open = 0;
    free(win);
}

int32_t gard_window_is_open(GardWindow* win) {
    return (win && win->open && !g_quit_requested) ? 1 : 0;
}

void gard_window_set_title(GardWindow* win, const char* title) {
    if (win && win->window) SDL_SetWindowTitle(win->window, title ? title : "");
}

int32_t gard_window_width(GardWindow* win) {
    if (!win) return 0;
    SDL_GetWindowSize(win->window, &win->width, &win->height);
    return win->width;
}

int32_t gard_window_height(GardWindow* win) {
    if (!win) return 0;
    SDL_GetWindowSize(win->window, &win->width, &win->height);
    return win->height;
}

// ============================================================
// Rendering
// ============================================================

void gard_graphics_clear(GardWindow* win) {
    if (!win || !win->renderer) return;
    SDL_SetRenderDrawColor(win->renderer, 0, 0, 0, 255);
    SDL_RenderClear(win->renderer);
}

void gard_graphics_clear_color(GardWindow* win, int32_t r, int32_t g, int32_t b) {
    if (!win || !win->renderer) return;
    SDL_SetRenderDrawColor(win->renderer, (uint8_t)r, (uint8_t)g, (uint8_t)b, 255);
    SDL_RenderClear(win->renderer);
}

void gard_graphics_present(GardWindow* win) {
    if (!win || !win->renderer) return;
    SDL_RenderPresent(win->renderer);

    // Frame rate limiting
    if (win->target_fps > 0) {
        uint32_t frame_time = SDL_GetTicks() - win->frame_start;
        uint32_t target_time = 1000 / win->target_fps;
        if (frame_time < target_time) {
            SDL_Delay(target_time - frame_time);
        }
    }
    win->frame_start = SDL_GetTicks();
}

void gard_graphics_set_color(GardWindow* win, int32_t r, int32_t g, int32_t b, int32_t a) {
    if (!win || !win->renderer) return;
    SDL_SetRenderDrawColor(win->renderer, (uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)a);
}

void gard_graphics_draw_rect(GardWindow* win, int32_t x, int32_t y, int32_t w, int32_t h) {
    if (!win || !win->renderer) return;
    SDL_Rect rect = { x, y, w, h };
    SDL_RenderDrawRect(win->renderer, &rect);
}

void gard_graphics_fill_rect(GardWindow* win, int32_t x, int32_t y, int32_t w, int32_t h) {
    if (!win || !win->renderer) return;
    SDL_Rect rect = { x, y, w, h };
    SDL_RenderFillRect(win->renderer, &rect);
}

void gard_graphics_draw_line(GardWindow* win, int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    if (!win || !win->renderer) return;
    SDL_RenderDrawLine(win->renderer, x1, y1, x2, y2);
}

void gard_graphics_draw_point(GardWindow* win, int32_t x, int32_t y) {
    if (!win || !win->renderer) return;
    SDL_RenderDrawPoint(win->renderer, x, y);
}

void gard_graphics_draw_circle(GardWindow* win, int32_t cx, int32_t cy, int32_t radius) {
    if (!win || !win->renderer || radius <= 0) return;
    // Midpoint circle algorithm
    int x = radius, y = 0, err = 1 - radius;
    while (x >= y) {
        SDL_RenderDrawPoint(win->renderer, cx + x, cy + y);
        SDL_RenderDrawPoint(win->renderer, cx - x, cy + y);
        SDL_RenderDrawPoint(win->renderer, cx + x, cy - y);
        SDL_RenderDrawPoint(win->renderer, cx - x, cy - y);
        SDL_RenderDrawPoint(win->renderer, cx + y, cy + x);
        SDL_RenderDrawPoint(win->renderer, cx - y, cy + x);
        SDL_RenderDrawPoint(win->renderer, cx + y, cy - x);
        SDL_RenderDrawPoint(win->renderer, cx - y, cy - x);
        y++;
        if (err < 0) { err += 2 * y + 1; }
        else { x--; err += 2 * (y - x) + 1; }
    }
}

void gard_graphics_fill_circle(GardWindow* win, int32_t cx, int32_t cy, int32_t radius) {
    if (!win || !win->renderer || radius <= 0) return;
    // Scanline fill
    for (int y = -radius; y <= radius; y++) {
        int dx = (int)SDL_sqrt((double)(radius * radius - y * y));
        SDL_RenderDrawLine(win->renderer, cx - dx, cy + y, cx + dx, cy + y);
    }
}

// ============================================================
// Input
// ============================================================

void gard_input_poll(GardWindow* win) {
    // Save previous keyboard state for "pressed" detection
    if (g_keyboard_state) memcpy(g_prev_keyboard, g_keyboard_state, 512);

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                g_quit_requested = 1;
                if (win) win->open = 0;
                break;
            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
                    if (win) win->open = 0;
                }
                break;
        }
    }

    g_keyboard_state = SDL_GetKeyboardState(NULL);
    g_mouse_buttons = SDL_GetMouseState(&g_mouse_x, &g_mouse_y);
}

int32_t gard_input_is_key_down(int32_t key_code) {
    if (!g_keyboard_state || key_code < 0 || key_code >= 512) return 0;
    return g_keyboard_state[key_code] ? 1 : 0;
}

int32_t gard_input_is_key_pressed(int32_t key_code) {
    if (!g_keyboard_state || key_code < 0 || key_code >= 512) return 0;
    return (g_keyboard_state[key_code] && !g_prev_keyboard[key_code]) ? 1 : 0;
}

int32_t gard_input_mouse_x(void) { return g_mouse_x; }
int32_t gard_input_mouse_y(void) { return g_mouse_y; }

int32_t gard_input_mouse_button(int32_t button) {
    // button: 1=left, 2=middle, 3=right
    if (button == 1) return (g_mouse_buttons & SDL_BUTTON_LMASK) ? 1 : 0;
    if (button == 2) return (g_mouse_buttons & SDL_BUTTON_MMASK) ? 1 : 0;
    if (button == 3) return (g_mouse_buttons & SDL_BUTTON_RMASK) ? 1 : 0;
    return 0;
}

int32_t gard_input_quit_requested(void) { return g_quit_requested; }

// ============================================================
// Timing
// ============================================================

void gard_graphics_delay(int32_t ms) {
    if (ms > 0) SDL_Delay((uint32_t)ms);
}

int32_t gard_graphics_get_ticks(void) {
    return (int32_t)SDL_GetTicks();
}

void gard_graphics_set_fps(GardWindow* win, int32_t fps) {
    if (win) win->target_fps = fps > 0 ? fps : 0;
}
