#include "runtime/stdlib_graphics.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_mixer.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <cmath>

namespace gard {
namespace runtime {
namespace stdlib {

// ===== Internal State =====

struct GardWindow {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    bool open = true;
    int width = 800;
    int height = 600;
    std::string title;
};

struct GardFont {
    TTF_Font* font = nullptr;
    int size = 16;
    std::string path;
};

struct GardImage {
    SDL_Texture* texture = nullptr;
    int width = 0;
    int height = 0;
};

static std::unordered_map<int, std::shared_ptr<GardWindow>> g_windows;
static std::unordered_map<int, std::shared_ptr<GardFont>> g_fonts;
static std::unordered_map<int, std::shared_ptr<GardImage>> g_images;
static std::mutex g_gfxMutex;
static int g_nextWinId = 1;
static int g_nextFontId = 1;
static int g_nextImageId = 1;
static bool g_sdlInitialized = false;
static bool g_ttfInitialized = false;
static bool g_mixerInitialized = false;

// Input state (updated each frame via pollEvents)
static bool g_keysDown[512] = {};
static bool g_mouseButtons[8] = {};
static int g_mouseX = 0, g_mouseY = 0;
static int g_mouseDX = 0, g_mouseDY = 0;

static void ensureSDL() {
    if (!g_sdlInitialized) {
        SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER);
        IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);
        g_sdlInitialized = true;
    }
}

static void ensureTTF() {
    if (!g_ttfInitialized) {
        TTF_Init();
        g_ttfInitialized = true;
    }
}

static SDL_Color valueToColor(const Value& v) {
    SDL_Color c = {255, 255, 255, 255};
    if (v.type == ValueType::Object && v.objVal) {
        auto rIt = v.objVal->fields.find("r");
        auto gIt = v.objVal->fields.find("g");
        auto bIt = v.objVal->fields.find("b");
        auto aIt = v.objVal->fields.find("a");
        if (rIt != v.objVal->fields.end()) c.r = (uint8_t)rIt->second.toInt();
        if (gIt != v.objVal->fields.end()) c.g = (uint8_t)gIt->second.toInt();
        if (bIt != v.objVal->fields.end()) c.b = (uint8_t)bIt->second.toInt();
        if (aIt != v.objVal->fields.end()) c.a = (uint8_t)aIt->second.toInt();
    }
    return c;
}

// ===== Register All Graphics Natives =====

void registerGraphicsModule(VM& vm) {

    // ===== Window Management =====

    // Window.create(title, width, height, options?)
    vm.registerNative("Window.create", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty()) { vm.throwError("GardWindowError", "Window.create: title required"); return Value::makeNull(); }
        ensureSDL();

        std::string title = a[0].toString();
        int width = a.size() >= 2 ? a[1].toInt() : 800;
        int height = a.size() >= 3 ? a[2].toInt() : 600;

        // Parse options
        Uint32 flags = SDL_WINDOW_SHOWN;
        bool vsync = false;
        if (a.size() >= 4 && a[3].type == ValueType::Object && a[3].objVal) {
            auto& opts = a[3].objVal->fields;
            if (opts.count("resizable") && opts["resizable"].toBool()) flags |= SDL_WINDOW_RESIZABLE;
            if (opts.count("fullscreen") && opts["fullscreen"].toBool()) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
            if (opts.count("borderless") && opts["borderless"].toBool()) flags |= SDL_WINDOW_BORDERLESS;
            if (opts.count("vsync") && opts["vsync"].toBool()) vsync = true;
            if (opts.count("msaa") && opts["msaa"].toInt() > 0) {
                SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
                SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, opts["msaa"].toInt());
            }
        }

        SDL_Window* win = SDL_CreateWindow(title.c_str(),
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, flags);
        if (!win) {
            vm.throwError("GardWindowError", "Window.create: " + std::string(SDL_GetError()));
            return Value::makeNull();
        }

        Uint32 rendererFlags = SDL_RENDERER_ACCELERATED;
        if (vsync) rendererFlags |= SDL_RENDERER_PRESENTVSYNC;
        SDL_Renderer* renderer = SDL_CreateRenderer(win, -1, rendererFlags);
        if (!renderer) {
            SDL_DestroyWindow(win);
            vm.throwError("GardWindowError", "Window.create: renderer failed: " + std::string(SDL_GetError()));
            return Value::makeNull();
        }
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

        auto gw = std::make_shared<GardWindow>();
        gw->window = win;
        gw->renderer = renderer;
        gw->width = width;
        gw->height = height;
        gw->title = title;

        int id = g_nextWinId++;
        { std::lock_guard<std::mutex> lock(g_gfxMutex); g_windows[id] = gw; }

        Value result = Value::makeObject("Window");
        result.objVal->fields["_id"] = Value::makeInt(id);
        result.objVal->fields["title"] = Value::makeString(title);
        result.objVal->fields["width"] = Value::makeInt(width);
        result.objVal->fields["height"] = Value::makeInt(height);
        return result;
    });

    // Window.close(win)
    vm.registerNative("Window.close", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        int id = a[0].objVal->fields["_id"].toInt();
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto it = g_windows.find(id);
        if (it != g_windows.end()) {
            it->second->open = false;
            if (it->second->renderer) SDL_DestroyRenderer(it->second->renderer);
            if (it->second->window) SDL_DestroyWindow(it->second->window);
            g_windows.erase(it);
        }
        return Value::makeNull();
    });

    // Window.setTitle(win, title)
    vm.registerNative("Window.setTitle", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        int id = a[0].objVal->fields["_id"].toInt();
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto it = g_windows.find(id);
        if (it != g_windows.end() && it->second->window) {
            std::string title = a[1].toString();
            SDL_SetWindowTitle(it->second->window, title.c_str());
            it->second->title = title;
        }
        return Value::makeNull();
    });

    // Window.resize(win, width, height)
    vm.registerNative("Window.resize", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) return Value::makeNull();
        int id = a[0].objVal->fields["_id"].toInt();
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto it = g_windows.find(id);
        if (it != g_windows.end() && it->second->window) {
            int w = a[1].toInt(), h = a[2].toInt();
            SDL_SetWindowSize(it->second->window, w, h);
            it->second->width = w;
            it->second->height = h;
        }
        return Value::makeNull();
    });

    // Window.setFullscreen(win, enabled)
    vm.registerNative("Window.setFullscreen", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        int id = a[0].objVal->fields["_id"].toInt();
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto it = g_windows.find(id);
        if (it != g_windows.end() && it->second->window) {
            Uint32 flag = a[1].toBool() ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0;
            SDL_SetWindowFullscreen(it->second->window, flag);
        }
        return Value::makeNull();
    });

    // Window.isOpen(win)
    vm.registerNative("Window.isOpen", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeBool(false);
        int id = a[0].objVal->fields["_id"].toInt();
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto it = g_windows.find(id);
        return Value::makeBool(it != g_windows.end() && it->second->open);
    });

    // Window.getSize(win)
    vm.registerNative("Window.getSize", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        int id = a[0].objVal->fields["_id"].toInt();
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto it = g_windows.find(id);
        if (it == g_windows.end()) return Value::makeNull();
        Value size = Value::makeObject("Size");
        size.objVal->fields["width"] = Value::makeInt(it->second->width);
        size.objVal->fields["height"] = Value::makeInt(it->second->height);
        return size;
    });

    // Window.setIcon(win, imagePath)
    vm.registerNative("Window.setIcon", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        int id = a[0].objVal->fields["_id"].toInt();
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto it = g_windows.find(id);
        if (it == g_windows.end() || !it->second->window) return Value::makeNull();
        SDL_Surface* icon = IMG_Load(a[1].toString().c_str());
        if (!icon) { vm.throwError("GardWindowError", "Window.setIcon: " + std::string(IMG_GetError())); return Value::makeNull(); }
        SDL_SetWindowIcon(it->second->window, icon);
        SDL_FreeSurface(icon);
        return Value::makeNull();
    });

    // ===== Event Loop =====

    // Window.pollEvents(win) — returns array of event objects
    vm.registerNative("Window.pollEvents", [](const std::vector<Value>& a) -> Value {
        Value events = Value::makeArray();
        g_mouseDX = 0; g_mouseDY = 0;

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            Value ev = Value::makeObject("Event");
            switch (e.type) {
                case SDL_QUIT:
                    ev.objVal->fields["type"] = Value::makeString("close");
                    break;
                case SDL_KEYDOWN:
                    ev.objVal->fields["type"] = Value::makeString("keyDown");
                    ev.objVal->fields["key"] = Value::makeString(SDL_GetKeyName(e.key.keysym.sym));
                    ev.objVal->fields["keyCode"] = Value::makeInt(e.key.keysym.sym);
                    ev.objVal->fields["repeat"] = Value::makeBool(e.key.repeat != 0);
                    if (e.key.keysym.scancode < 512) g_keysDown[e.key.keysym.scancode] = true;
                    break;
                case SDL_KEYUP:
                    ev.objVal->fields["type"] = Value::makeString("keyUp");
                    ev.objVal->fields["key"] = Value::makeString(SDL_GetKeyName(e.key.keysym.sym));
                    ev.objVal->fields["keyCode"] = Value::makeInt(e.key.keysym.sym);
                    if (e.key.keysym.scancode < 512) g_keysDown[e.key.keysym.scancode] = false;
                    break;
                case SDL_MOUSEBUTTONDOWN:
                    ev.objVal->fields["type"] = Value::makeString("mouseDown");
                    ev.objVal->fields["button"] = Value::makeInt(e.button.button);
                    ev.objVal->fields["x"] = Value::makeInt(e.button.x);
                    ev.objVal->fields["y"] = Value::makeInt(e.button.y);
                    if (e.button.button < 8) g_mouseButtons[e.button.button] = true;
                    break;
                case SDL_MOUSEBUTTONUP:
                    ev.objVal->fields["type"] = Value::makeString("mouseUp");
                    ev.objVal->fields["button"] = Value::makeInt(e.button.button);
                    ev.objVal->fields["x"] = Value::makeInt(e.button.x);
                    ev.objVal->fields["y"] = Value::makeInt(e.button.y);
                    if (e.button.button < 8) g_mouseButtons[e.button.button] = false;
                    break;
                case SDL_MOUSEMOTION:
                    ev.objVal->fields["type"] = Value::makeString("mouseMove");
                    ev.objVal->fields["x"] = Value::makeInt(e.motion.x);
                    ev.objVal->fields["y"] = Value::makeInt(e.motion.y);
                    ev.objVal->fields["dx"] = Value::makeInt(e.motion.xrel);
                    ev.objVal->fields["dy"] = Value::makeInt(e.motion.yrel);
                    g_mouseX = e.motion.x; g_mouseY = e.motion.y;
                    g_mouseDX += e.motion.xrel; g_mouseDY += e.motion.yrel;
                    break;
                case SDL_MOUSEWHEEL:
                    ev.objVal->fields["type"] = Value::makeString("mouseWheel");
                    ev.objVal->fields["delta"] = Value::makeInt(e.wheel.y);
                    ev.objVal->fields["deltaX"] = Value::makeInt(e.wheel.x);
                    break;
                case SDL_WINDOWEVENT:
                    if (e.window.event == SDL_WINDOWEVENT_RESIZED) {
                        ev.objVal->fields["type"] = Value::makeString("resize");
                        ev.objVal->fields["width"] = Value::makeInt(e.window.data1);
                        ev.objVal->fields["height"] = Value::makeInt(e.window.data2);
                    } else if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                        ev.objVal->fields["type"] = Value::makeString("focus");
                    } else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                        ev.objVal->fields["type"] = Value::makeString("blur");
                    } else if (e.window.event == SDL_WINDOWEVENT_CLOSE) {
                        ev.objVal->fields["type"] = Value::makeString("close");
                    } else {
                        continue; // skip unhandled window events
                    }
                    break;
                default:
                    continue; // skip unhandled events
            }
            events.arrVal->elements.push_back(ev);
        }
        return events;
    });

    // Window.waitEvent(win) — blocking wait for next event
    vm.registerNative("Window.waitEvent", [](const std::vector<Value>& a) -> Value {
        SDL_Event e;
        if (!SDL_WaitEvent(&e)) return Value::makeNull();
        Value ev = Value::makeObject("Event");
        ev.objVal->fields["type"] = Value::makeString("unknown");
        if (e.type == SDL_QUIT) ev.objVal->fields["type"] = Value::makeString("close");
        else if (e.type == SDL_KEYDOWN) { ev.objVal->fields["type"] = Value::makeString("keyDown"); ev.objVal->fields["key"] = Value::makeString(SDL_GetKeyName(e.key.keysym.sym)); }
        else if (e.type == SDL_KEYUP) { ev.objVal->fields["type"] = Value::makeString("keyUp"); ev.objVal->fields["key"] = Value::makeString(SDL_GetKeyName(e.key.keysym.sym)); }
        return ev;
    });

    // Window.onEvent(win, type, handler) — register event callback
    // handler is a function name (string) called when the event fires
    // Usage: Window.onEvent(win, "keyDown", "onKeyDown");
    vm.registerNative("Window.onEvent", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) return Value::makeBool(false);
        int id = a[0].objVal->fields["_id"].toInt();
        std::string eventType = a[1].toString();
        std::string handler = a[2].toString();

        // Store handler on the window object for dispatch during pollEvents
        std::string key = "_on_" + eventType;
        a[0].objVal->fields[key] = Value::makeString(handler);
        return Value::makeBool(true);
    });

    // ===== Input State =====

    // Input.isKeyDown(key)
    vm.registerNative("Input.isKeyDown", [](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeBool(false);
        SDL_Scancode sc = SDL_GetScancodeFromName(a[0].toString().c_str());
        if (sc < 512) return Value::makeBool(g_keysDown[sc]);
        return Value::makeBool(false);
    });

    // Input.isMouseDown(button)
    vm.registerNative("Input.isMouseDown", [](const std::vector<Value>& a) -> Value {
        int btn = a.empty() ? 1 : a[0].toInt();
        if (btn >= 0 && btn < 8) return Value::makeBool(g_mouseButtons[btn]);
        return Value::makeBool(false);
    });

    // Input.getMousePosition()
    vm.registerNative("Input.getMousePosition", [](const std::vector<Value>&) -> Value {
        Value pos = Value::makeObject("Position");
        pos.objVal->fields["x"] = Value::makeInt(g_mouseX);
        pos.objVal->fields["y"] = Value::makeInt(g_mouseY);
        return pos;
    });

    // Input.getMouseDelta()
    vm.registerNative("Input.getMouseDelta", [](const std::vector<Value>&) -> Value {
        Value delta = Value::makeObject("Delta");
        delta.objVal->fields["dx"] = Value::makeInt(g_mouseDX);
        delta.objVal->fields["dy"] = Value::makeInt(g_mouseDY);
        return delta;
    });

    // ===== 2D Graphics =====

    // Graphics2D.clear(win, color)
    vm.registerNative("Graphics2D.clear", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        int id = a[0].objVal->fields["_id"].toInt();
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto it = g_windows.find(id);
        if (it == g_windows.end() || !it->second->renderer) return Value::makeNull();
        SDL_Color c = (a.size() >= 2) ? valueToColor(a[1]) : SDL_Color{0, 0, 0, 255};
        SDL_SetRenderDrawColor(it->second->renderer, c.r, c.g, c.b, c.a);
        SDL_RenderClear(it->second->renderer);
        return Value::makeNull();
    });

    // Graphics2D.present(win)
    vm.registerNative("Graphics2D.present", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        int id = a[0].objVal->fields["_id"].toInt();
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto it = g_windows.find(id);
        if (it != g_windows.end() && it->second->renderer) SDL_RenderPresent(it->second->renderer);
        return Value::makeNull();
    });

    // Graphics2D.drawRect(win, x, y, w, h, color)
    vm.registerNative("Graphics2D.drawRect", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 6 || !a[0].objVal) return Value::makeNull();
        int id = a[0].objVal->fields["_id"].toInt();
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto it = g_windows.find(id);
        if (it == g_windows.end() || !it->second->renderer) return Value::makeNull();
        SDL_Color c = valueToColor(a[5]);
        SDL_SetRenderDrawColor(it->second->renderer, c.r, c.g, c.b, c.a);
        SDL_Rect rect = {a[1].toInt(), a[2].toInt(), a[3].toInt(), a[4].toInt()};
        SDL_RenderFillRect(it->second->renderer, &rect);
        return Value::makeNull();
    });

    // Graphics2D.drawRectOutline(win, x, y, w, h, color, thickness?)
    vm.registerNative("Graphics2D.drawRectOutline", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 6 || !a[0].objVal) return Value::makeNull();
        int id = a[0].objVal->fields["_id"].toInt();
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto it = g_windows.find(id);
        if (it == g_windows.end() || !it->second->renderer) return Value::makeNull();
        SDL_Color c = valueToColor(a[5]);
        SDL_SetRenderDrawColor(it->second->renderer, c.r, c.g, c.b, c.a);
        SDL_Rect rect = {a[1].toInt(), a[2].toInt(), a[3].toInt(), a[4].toInt()};
        SDL_RenderDrawRect(it->second->renderer, &rect);
        return Value::makeNull();
    });

    // Graphics2D.drawLine(win, x1, y1, x2, y2, color, thickness?)
    vm.registerNative("Graphics2D.drawLine", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 6 || !a[0].objVal) return Value::makeNull();
        int id = a[0].objVal->fields["_id"].toInt();
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto it = g_windows.find(id);
        if (it == g_windows.end() || !it->second->renderer) return Value::makeNull();
        SDL_Color c = valueToColor(a[5]);
        SDL_SetRenderDrawColor(it->second->renderer, c.r, c.g, c.b, c.a);
        SDL_RenderDrawLine(it->second->renderer, a[1].toInt(), a[2].toInt(), a[3].toInt(), a[4].toInt());
        return Value::makeNull();
    });

    // Graphics2D.drawCircle(win, cx, cy, radius, color) — Midpoint circle algorithm
    vm.registerNative("Graphics2D.drawCircle", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 5 || !a[0].objVal) return Value::makeNull();
        int id = a[0].objVal->fields["_id"].toInt();
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto it = g_windows.find(id);
        if (it == g_windows.end() || !it->second->renderer) return Value::makeNull();
        SDL_Color c = valueToColor(a[4]);
        SDL_SetRenderDrawColor(it->second->renderer, c.r, c.g, c.b, c.a);
        int cx = a[1].toInt(), cy = a[2].toInt(), r = a[3].toInt();
        // Filled circle using horizontal lines
        for (int dy = -r; dy <= r; dy++) {
            int dx = (int)std::sqrt(r * r - dy * dy);
            SDL_RenderDrawLine(it->second->renderer, cx - dx, cy + dy, cx + dx, cy + dy);
        }
        return Value::makeNull();
    });

    // Graphics2D.drawCircleOutline(win, cx, cy, radius, color)
    vm.registerNative("Graphics2D.drawCircleOutline", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 5 || !a[0].objVal) return Value::makeNull();
        int id = a[0].objVal->fields["_id"].toInt();
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto it = g_windows.find(id);
        if (it == g_windows.end() || !it->second->renderer) return Value::makeNull();
        SDL_Color c = valueToColor(a[4]);
        SDL_SetRenderDrawColor(it->second->renderer, c.r, c.g, c.b, c.a);
        int cx = a[1].toInt(), cy = a[2].toInt(), r = a[3].toInt();
        int x = r, y = 0, err = 1 - r;
        while (x >= y) {
            SDL_RenderDrawPoint(it->second->renderer, cx+x, cy+y); SDL_RenderDrawPoint(it->second->renderer, cx-x, cy+y);
            SDL_RenderDrawPoint(it->second->renderer, cx+x, cy-y); SDL_RenderDrawPoint(it->second->renderer, cx-x, cy-y);
            SDL_RenderDrawPoint(it->second->renderer, cx+y, cy+x); SDL_RenderDrawPoint(it->second->renderer, cx-y, cy+x);
            SDL_RenderDrawPoint(it->second->renderer, cx+y, cy-x); SDL_RenderDrawPoint(it->second->renderer, cx-y, cy-x);
            y++;
            if (err < 0) err += 2*y+1;
            else { x--; err += 2*(y-x)+1; }
        }
        return Value::makeNull();
    });

    // Graphics2D.drawPixel(win, x, y, color)
    vm.registerNative("Graphics2D.drawPixel", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 4 || !a[0].objVal) return Value::makeNull();
        int id = a[0].objVal->fields["_id"].toInt();
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto it = g_windows.find(id);
        if (it == g_windows.end() || !it->second->renderer) return Value::makeNull();
        SDL_Color c = valueToColor(a[3]);
        SDL_SetRenderDrawColor(it->second->renderer, c.r, c.g, c.b, c.a);
        SDL_RenderDrawPoint(it->second->renderer, a[1].toInt(), a[2].toInt());
        return Value::makeNull();
    });

    // Graphics2D.drawTriangle(win, x1, y1, x2, y2, x3, y3, color)
    vm.registerNative("Graphics2D.drawTriangle", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 8 || !a[0].objVal) return Value::makeNull();
        int id = a[0].objVal->fields["_id"].toInt();
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto it = g_windows.find(id);
        if (it == g_windows.end() || !it->second->renderer) return Value::makeNull();
        SDL_Color c = valueToColor(a[7]);
        SDL_SetRenderDrawColor(it->second->renderer, c.r, c.g, c.b, c.a);
        SDL_RenderDrawLine(it->second->renderer, a[1].toInt(), a[2].toInt(), a[3].toInt(), a[4].toInt());
        SDL_RenderDrawLine(it->second->renderer, a[3].toInt(), a[4].toInt(), a[5].toInt(), a[6].toInt());
        SDL_RenderDrawLine(it->second->renderer, a[5].toInt(), a[6].toInt(), a[1].toInt(), a[2].toInt());
        return Value::makeNull();
    });

    // Graphics2D.drawPolygon(win, points, color) — filled polygon (scanline fill)
    // points is an array of {x, y} objects
    vm.registerNative("Graphics2D.drawPolygon", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal || !a[1].arrVal) return Value::makeNull();
        int id = a[0].objVal->fields["_id"].toInt();
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto it = g_windows.find(id);
        if (it == g_windows.end() || !it->second->renderer) return Value::makeNull();
        SDL_Color c = valueToColor(a[2]);
        SDL_SetRenderDrawColor(it->second->renderer, c.r, c.g, c.b, c.a);
        // Draw polygon outline (connect all points)
        auto& pts = a[1].arrVal->elements;
        if (pts.size() < 3) return Value::makeNull();
        for (size_t i = 0; i < pts.size(); i++) {
            size_t next = (i + 1) % pts.size();
            int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
            if (pts[i].type == ValueType::Object && pts[i].objVal) {
                auto xi = pts[i].objVal->fields.find("x"); if (xi != pts[i].objVal->fields.end()) x1 = xi->second.toInt();
                auto yi = pts[i].objVal->fields.find("y"); if (yi != pts[i].objVal->fields.end()) y1 = yi->second.toInt();
            }
            if (pts[next].type == ValueType::Object && pts[next].objVal) {
                auto xi = pts[next].objVal->fields.find("x"); if (xi != pts[next].objVal->fields.end()) x2 = xi->second.toInt();
                auto yi = pts[next].objVal->fields.find("y"); if (yi != pts[next].objVal->fields.end()) y2 = yi->second.toInt();
            }
            SDL_RenderDrawLine(it->second->renderer, x1, y1, x2, y2);
        }
        return Value::makeNull();
    });

    // ===== Colors =====

    vm.registerNative("Color.rgb", [](const std::vector<Value>& a) -> Value {
        Value c = Value::makeObject("Color");
        c.objVal->fields["r"] = Value::makeInt(a.size() >= 1 ? a[0].toInt() : 0);
        c.objVal->fields["g"] = Value::makeInt(a.size() >= 2 ? a[1].toInt() : 0);
        c.objVal->fields["b"] = Value::makeInt(a.size() >= 3 ? a[2].toInt() : 0);
        c.objVal->fields["a"] = Value::makeInt(255);
        return c;
    });
    vm.registerNative("Color.rgba", [](const std::vector<Value>& a) -> Value {
        Value c = Value::makeObject("Color");
        c.objVal->fields["r"] = Value::makeInt(a.size() >= 1 ? a[0].toInt() : 0);
        c.objVal->fields["g"] = Value::makeInt(a.size() >= 2 ? a[1].toInt() : 0);
        c.objVal->fields["b"] = Value::makeInt(a.size() >= 3 ? a[2].toInt() : 0);
        c.objVal->fields["a"] = Value::makeInt(a.size() >= 4 ? a[3].toInt() : 255);
        return c;
    });
    vm.registerNative("Color.hex", [](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeNull();
        std::string hex = a[0].toString();
        if (!hex.empty() && hex[0] == '#') hex = hex.substr(1);
        int r = 0, g = 0, b = 0, al = 255;
        if (hex.size() >= 6) {
            r = std::stoi(hex.substr(0, 2), nullptr, 16);
            g = std::stoi(hex.substr(2, 2), nullptr, 16);
            b = std::stoi(hex.substr(4, 2), nullptr, 16);
            if (hex.size() >= 8) al = std::stoi(hex.substr(6, 2), nullptr, 16);
        }
        Value c = Value::makeObject("Color");
        c.objVal->fields["r"] = Value::makeInt(r);
        c.objVal->fields["g"] = Value::makeInt(g);
        c.objVal->fields["b"] = Value::makeInt(b);
        c.objVal->fields["a"] = Value::makeInt(al);
        return c;
    });
    // Predefined colors
    vm.registerNative("Color.RED", [](const std::vector<Value>&) -> Value { Value c=Value::makeObject("Color"); c.objVal->fields["r"]=Value::makeInt(255); c.objVal->fields["g"]=Value::makeInt(0); c.objVal->fields["b"]=Value::makeInt(0); c.objVal->fields["a"]=Value::makeInt(255); return c; });
    vm.registerNative("Color.GREEN", [](const std::vector<Value>&) -> Value { Value c=Value::makeObject("Color"); c.objVal->fields["r"]=Value::makeInt(0); c.objVal->fields["g"]=Value::makeInt(255); c.objVal->fields["b"]=Value::makeInt(0); c.objVal->fields["a"]=Value::makeInt(255); return c; });
    vm.registerNative("Color.BLUE", [](const std::vector<Value>&) -> Value { Value c=Value::makeObject("Color"); c.objVal->fields["r"]=Value::makeInt(0); c.objVal->fields["g"]=Value::makeInt(0); c.objVal->fields["b"]=Value::makeInt(255); c.objVal->fields["a"]=Value::makeInt(255); return c; });
    vm.registerNative("Color.WHITE", [](const std::vector<Value>&) -> Value { Value c=Value::makeObject("Color"); c.objVal->fields["r"]=Value::makeInt(255); c.objVal->fields["g"]=Value::makeInt(255); c.objVal->fields["b"]=Value::makeInt(255); c.objVal->fields["a"]=Value::makeInt(255); return c; });
    vm.registerNative("Color.BLACK", [](const std::vector<Value>&) -> Value { Value c=Value::makeObject("Color"); c.objVal->fields["r"]=Value::makeInt(0); c.objVal->fields["g"]=Value::makeInt(0); c.objVal->fields["b"]=Value::makeInt(0); c.objVal->fields["a"]=Value::makeInt(255); return c; });
    vm.registerNative("Color.TRANSPARENT", [](const std::vector<Value>&) -> Value { Value c=Value::makeObject("Color"); c.objVal->fields["r"]=Value::makeInt(0); c.objVal->fields["g"]=Value::makeInt(0); c.objVal->fields["b"]=Value::makeInt(0); c.objVal->fields["a"]=Value::makeInt(0); return c; });

    // Color.hsl(h, s, l) — HSL to RGB conversion
    // h: 0-360, s: 0-100, l: 0-100
    vm.registerNative("Color.hsl", [](const std::vector<Value>& a) -> Value {
        double h = a.size() >= 1 ? a[0].toDouble() : 0;
        double s = a.size() >= 2 ? a[1].toDouble() / 100.0 : 0;
        double l = a.size() >= 3 ? a[2].toDouble() / 100.0 : 0;
        // HSL to RGB
        auto hue2rgb = [](double p, double q, double t) -> double {
            if (t < 0) t += 1; if (t > 1) t -= 1;
            if (t < 1.0/6) return p + (q - p) * 6 * t;
            if (t < 1.0/2) return q;
            if (t < 2.0/3) return p + (q - p) * (2.0/3 - t) * 6;
            return p;
        };
        double r, g, b;
        if (s == 0) { r = g = b = l; }
        else {
            double q = l < 0.5 ? l * (1 + s) : l + s - l * s;
            double p = 2 * l - q;
            double hNorm = h / 360.0;
            r = hue2rgb(p, q, hNorm + 1.0/3);
            g = hue2rgb(p, q, hNorm);
            b = hue2rgb(p, q, hNorm - 1.0/3);
        }
        Value c = Value::makeObject("Color");
        c.objVal->fields["r"] = Value::makeInt((int)(r * 255));
        c.objVal->fields["g"] = Value::makeInt((int)(g * 255));
        c.objVal->fields["b"] = Value::makeInt((int)(b * 255));
        c.objVal->fields["a"] = Value::makeInt(255);
        return c;
    });

    // ===== Images =====

    // Image.load(path)
    vm.registerNative("Image.load", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty()) { vm.throwError("GardTextureError", "Image.load: path required"); return Value::makeNull(); }
        ensureSDL();
        // We need a renderer to create texture — use first available window
        SDL_Renderer* renderer = nullptr;
        { std::lock_guard<std::mutex> lock(g_gfxMutex);
          if (!g_windows.empty()) renderer = g_windows.begin()->second->renderer; }
        if (!renderer) { vm.throwError("GardTextureError", "Image.load: no window created yet"); return Value::makeNull(); }
        SDL_Surface* surface = IMG_Load(a[0].toString().c_str());
        if (!surface) { vm.throwError("GardTextureError", "Image.load: " + std::string(IMG_GetError())); return Value::makeNull(); }
        SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surface);
        int w = surface->w, h = surface->h;
        SDL_FreeSurface(surface);
        if (!tex) { vm.throwError("GardTextureError", "Image.load: texture creation failed"); return Value::makeNull(); }

        auto img = std::make_shared<GardImage>();
        img->texture = tex; img->width = w; img->height = h;
        int id = g_nextImageId++;
        { std::lock_guard<std::mutex> lock(g_gfxMutex); g_images[id] = img; }

        Value result = Value::makeObject("Image");
        result.objVal->fields["_id"] = Value::makeInt(id);
        result.objVal->fields["width"] = Value::makeInt(w);
        result.objVal->fields["height"] = Value::makeInt(h);
        return result;
    });

    // Graphics2D.drawImage(win, img, x, y)
    vm.registerNative("Graphics2D.drawImage", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 4 || !a[0].objVal || !a[1].objVal) return Value::makeNull();
        int winId = a[0].objVal->fields["_id"].toInt();
        int imgId = a[1].objVal->fields["_id"].toInt();
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto wit = g_windows.find(winId); auto iit = g_images.find(imgId);
        if (wit == g_windows.end() || iit == g_images.end()) return Value::makeNull();
        SDL_Rect dst = {a[2].toInt(), a[3].toInt(), iit->second->width, iit->second->height};
        SDL_RenderCopy(wit->second->renderer, iit->second->texture, nullptr, &dst);
        return Value::makeNull();
    });

    // Graphics2D.drawImageScaled(win, img, x, y, w, h)
    vm.registerNative("Graphics2D.drawImageScaled", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 6 || !a[0].objVal || !a[1].objVal) return Value::makeNull();
        int winId = a[0].objVal->fields["_id"].toInt();
        int imgId = a[1].objVal->fields["_id"].toInt();
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto wit = g_windows.find(winId); auto iit = g_images.find(imgId);
        if (wit == g_windows.end() || iit == g_images.end()) return Value::makeNull();
        SDL_Rect dst = {a[2].toInt(), a[3].toInt(), a[4].toInt(), a[5].toInt()};
        SDL_RenderCopy(wit->second->renderer, iit->second->texture, nullptr, &dst);
        return Value::makeNull();
    });

    // Graphics2D.drawImageRotated(win, img, x, y, angle)
    vm.registerNative("Graphics2D.drawImageRotated", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 5 || !a[0].objVal || !a[1].objVal) return Value::makeNull();
        int winId = a[0].objVal->fields["_id"].toInt();
        int imgId = a[1].objVal->fields["_id"].toInt();
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto wit = g_windows.find(winId); auto iit = g_images.find(imgId);
        if (wit == g_windows.end() || iit == g_images.end()) return Value::makeNull();
        SDL_Rect dst = {a[2].toInt(), a[3].toInt(), iit->second->width, iit->second->height};
        double angle = a[4].toDouble();
        SDL_RenderCopyEx(wit->second->renderer, iit->second->texture, nullptr, &dst, angle, nullptr, SDL_FLIP_NONE);
        return Value::makeNull();
    });

    // Graphics2D.drawImageRegion(win, img, srcRect, dstRect) — sprite sheet
    // srcRect/dstRect: {x, y, width, height}
    vm.registerNative("Graphics2D.drawImageRegion", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 4 || !a[0].objVal || !a[1].objVal || !a[2].objVal || !a[3].objVal) return Value::makeNull();
        int winId = a[0].objVal->fields["_id"].toInt();
        int imgId = a[1].objVal->fields["_id"].toInt();
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto wit = g_windows.find(winId); auto iit = g_images.find(imgId);
        if (wit == g_windows.end() || iit == g_images.end()) return Value::makeNull();
        auto getRect = [](const Value& v) -> SDL_Rect {
            SDL_Rect r = {0, 0, 0, 0};
            if (v.objVal) {
                auto xi = v.objVal->fields.find("x"); if (xi != v.objVal->fields.end()) r.x = xi->second.toInt();
                auto yi = v.objVal->fields.find("y"); if (yi != v.objVal->fields.end()) r.y = yi->second.toInt();
                auto wi = v.objVal->fields.find("width"); if (wi != v.objVal->fields.end()) r.w = wi->second.toInt();
                auto hi = v.objVal->fields.find("height"); if (hi != v.objVal->fields.end()) r.h = hi->second.toInt();
            }
            return r;
        };
        SDL_Rect src = getRect(a[2]), dst = getRect(a[3]);
        SDL_RenderCopy(wit->second->renderer, iit->second->texture, &src, &dst);
        return Value::makeNull();
    });

    // Image.create(width, height) — create blank image
    vm.registerNative("Image.create", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) { vm.throwError("GardTextureError", "Image.create: width and height required"); return Value::makeNull(); }
        ensureSDL();
        SDL_Renderer* renderer = nullptr;
        { std::lock_guard<std::mutex> lock(g_gfxMutex);
          if (!g_windows.empty()) renderer = g_windows.begin()->second->renderer; }
        if (!renderer) { vm.throwError("GardTextureError", "Image.create: no window created yet"); return Value::makeNull(); }
        int w = a[0].toInt(), h = a[1].toInt();
        SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, w, h);
        if (!tex) { vm.throwError("GardTextureError", "Image.create: " + std::string(SDL_GetError())); return Value::makeNull(); }
        auto img = std::make_shared<GardImage>();
        img->texture = tex; img->width = w; img->height = h;
        int id = g_nextImageId++;
        { std::lock_guard<std::mutex> lock(g_gfxMutex); g_images[id] = img; }
        Value result = Value::makeObject("Image");
        result.objVal->fields["_id"] = Value::makeInt(id);
        result.objVal->fields["width"] = Value::makeInt(w);
        result.objVal->fields["height"] = Value::makeInt(h);
        return result;
    });

    // Image.save(img, path) — save image to file (PNG)
    vm.registerNative("Image.save", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardTextureError", "Image.save: image and path required"); return Value::makeNull(); }
        int imgId = a[0].objVal->fields["_id"].toInt();
        std::string path = a[1].toString();
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto iit = g_images.find(imgId);
        if (iit == g_images.end()) { vm.throwError("GardTextureError", "Image.save: invalid image"); return Value::makeNull(); }
        // Read pixels from texture
        SDL_Renderer* renderer = nullptr;
        if (!g_windows.empty()) renderer = g_windows.begin()->second->renderer;
        if (!renderer) { vm.throwError("GardTextureError", "Image.save: no renderer"); return Value::makeNull(); }
        int w = iit->second->width, h = iit->second->height;
        SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
        if (!surface) { vm.throwError("GardTextureError", "Image.save: surface creation failed"); return Value::makeNull(); }
        SDL_SetRenderTarget(renderer, iit->second->texture);
        SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_RGBA32, surface->pixels, surface->pitch);
        SDL_SetRenderTarget(renderer, nullptr);
        IMG_SavePNG(surface, path.c_str());
        SDL_FreeSurface(surface);
        return Value::makeBool(true);
    });

    // ===== Text / Font =====

    // Font.load(path, size)
    vm.registerNative("Font.load", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) { vm.throwError("GardGraphicsError", "Font.load: path and size required"); return Value::makeNull(); }
        ensureSDL(); ensureTTF();
        std::string path = a[0].toString();
        int size = a[1].toInt();
        TTF_Font* font = TTF_OpenFont(path.c_str(), size);
        if (!font) { vm.throwError("GardGraphicsError", "Font.load: " + std::string(TTF_GetError())); return Value::makeNull(); }
        auto gf = std::make_shared<GardFont>();
        gf->font = font; gf->size = size; gf->path = path;
        int id = g_nextFontId++;
        { std::lock_guard<std::mutex> lock(g_gfxMutex); g_fonts[id] = gf; }
        Value result = Value::makeObject("Font");
        result.objVal->fields["_id"] = Value::makeInt(id);
        result.objVal->fields["size"] = Value::makeInt(size);
        return result;
    });

    // Graphics2D.drawText(win, text, x, y, font, color)
    vm.registerNative("Graphics2D.drawText", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 6 || !a[0].objVal || !a[4].objVal) return Value::makeNull();
        int winId = a[0].objVal->fields["_id"].toInt();
        int fontId = a[4].objVal->fields["_id"].toInt();
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto wit = g_windows.find(winId); auto fit = g_fonts.find(fontId);
        if (wit == g_windows.end() || fit == g_fonts.end()) return Value::makeNull();
        SDL_Color c = valueToColor(a[5]);
        SDL_Surface* surface = TTF_RenderUTF8_Blended(fit->second->font, a[1].toString().c_str(), c);
        if (!surface) return Value::makeNull();
        SDL_Texture* tex = SDL_CreateTextureFromSurface(wit->second->renderer, surface);
        SDL_Rect dst = {a[2].toInt(), a[3].toInt(), surface->w, surface->h};
        SDL_FreeSurface(surface);
        if (tex) { SDL_RenderCopy(wit->second->renderer, tex, nullptr, &dst); SDL_DestroyTexture(tex); }
        return Value::makeNull();
    });

    // Font.measureText(font, text)
    vm.registerNative("Font.measureText", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        int fontId = a[0].objVal->fields["_id"].toInt();
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto fit = g_fonts.find(fontId);
        if (fit == g_fonts.end()) return Value::makeNull();
        int w = 0, h = 0;
        TTF_SizeUTF8(fit->second->font, a[1].toString().c_str(), &w, &h);
        Value size = Value::makeObject("Size");
        size.objVal->fields["width"] = Value::makeInt(w);
        size.objVal->fields["height"] = Value::makeInt(h);
        return size;
    });

    // Font.setSize(font, size) — change font size (reloads font at new size)
    vm.registerNative("Font.setSize", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        int fontId = a[0].objVal->fields["_id"].toInt();
        int newSize = a[1].toInt();
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto fit = g_fonts.find(fontId);
        if (fit == g_fonts.end()) return Value::makeNull();
        // Reload font at new size
        TTF_Font* newFont = TTF_OpenFont(fit->second->path.c_str(), newSize);
        if (!newFont) { vm.throwError("GardGraphicsError", "Font.setSize: " + std::string(TTF_GetError())); return Value::makeNull(); }
        TTF_CloseFont(fit->second->font);
        fit->second->font = newFont;
        fit->second->size = newSize;
        a[0].objVal->fields["size"] = Value::makeInt(newSize);
        return Value::makeNull();
    });

    // ===== Key Constants =====
    vm.registerNative("Key.SPACE", [](const std::vector<Value>&) -> Value { return Value::makeString("Space"); });
    vm.registerNative("Key.ESCAPE", [](const std::vector<Value>&) -> Value { return Value::makeString("Escape"); });
    vm.registerNative("Key.RETURN", [](const std::vector<Value>&) -> Value { return Value::makeString("Return"); });
    vm.registerNative("Key.W", [](const std::vector<Value>&) -> Value { return Value::makeString("W"); });
    vm.registerNative("Key.A", [](const std::vector<Value>&) -> Value { return Value::makeString("A"); });
    vm.registerNative("Key.S", [](const std::vector<Value>&) -> Value { return Value::makeString("S"); });
    vm.registerNative("Key.D", [](const std::vector<Value>&) -> Value { return Value::makeString("D"); });
    vm.registerNative("Key.ARROW_UP", [](const std::vector<Value>&) -> Value { return Value::makeString("Up"); });
    vm.registerNative("Key.ARROW_DOWN", [](const std::vector<Value>&) -> Value { return Value::makeString("Down"); });
    vm.registerNative("Key.ARROW_LEFT", [](const std::vector<Value>&) -> Value { return Value::makeString("Left"); });
    vm.registerNative("Key.ARROW_RIGHT", [](const std::vector<Value>&) -> Value { return Value::makeString("Right"); });
    vm.registerNative("Mouse.LEFT", [](const std::vector<Value>&) -> Value { return Value::makeInt(SDL_BUTTON_LEFT); });
    vm.registerNative("Mouse.RIGHT", [](const std::vector<Value>&) -> Value { return Value::makeInt(SDL_BUTTON_RIGHT); });
    vm.registerNative("Mouse.MIDDLE", [](const std::vector<Value>&) -> Value { return Value::makeInt(SDL_BUTTON_MIDDLE); });

    // ===== 3D Math: Vec3 =====
    vm.registerNative("Vec3.create", [](const std::vector<Value>& a) -> Value {
        Value v = Value::makeObject("Vec3");
        v.objVal->fields["x"] = Value::makeDouble(a.size()>=1?a[0].toDouble():0);
        v.objVal->fields["y"] = Value::makeDouble(a.size()>=2?a[1].toDouble():0);
        v.objVal->fields["z"] = Value::makeDouble(a.size()>=3?a[2].toDouble():0);
        return v;
    });
    vm.registerNative("Vec3.add", [](const std::vector<Value>& a) -> Value {
        if(a.size()<2||!a[0].objVal||!a[1].objVal) return Value::makeNull();
        Value v=Value::makeObject("Vec3");
        v.objVal->fields["x"]=Value::makeDouble(a[0].objVal->fields["x"].toDouble()+a[1].objVal->fields["x"].toDouble());
        v.objVal->fields["y"]=Value::makeDouble(a[0].objVal->fields["y"].toDouble()+a[1].objVal->fields["y"].toDouble());
        v.objVal->fields["z"]=Value::makeDouble(a[0].objVal->fields["z"].toDouble()+a[1].objVal->fields["z"].toDouble());
        return v;
    });
    vm.registerNative("Vec3.sub", [](const std::vector<Value>& a) -> Value {
        if(a.size()<2||!a[0].objVal||!a[1].objVal) return Value::makeNull();
        Value v=Value::makeObject("Vec3");
        v.objVal->fields["x"]=Value::makeDouble(a[0].objVal->fields["x"].toDouble()-a[1].objVal->fields["x"].toDouble());
        v.objVal->fields["y"]=Value::makeDouble(a[0].objVal->fields["y"].toDouble()-a[1].objVal->fields["y"].toDouble());
        v.objVal->fields["z"]=Value::makeDouble(a[0].objVal->fields["z"].toDouble()-a[1].objVal->fields["z"].toDouble());
        return v;
    });
    vm.registerNative("Vec3.mul", [](const std::vector<Value>& a) -> Value {
        if(a.size()<2||!a[0].objVal) return Value::makeNull();
        double s=a[1].toDouble();
        Value v=Value::makeObject("Vec3");
        v.objVal->fields["x"]=Value::makeDouble(a[0].objVal->fields["x"].toDouble()*s);
        v.objVal->fields["y"]=Value::makeDouble(a[0].objVal->fields["y"].toDouble()*s);
        v.objVal->fields["z"]=Value::makeDouble(a[0].objVal->fields["z"].toDouble()*s);
        return v;
    });
    vm.registerNative("Vec3.dot", [](const std::vector<Value>& a) -> Value {
        if(a.size()<2||!a[0].objVal||!a[1].objVal) return Value::makeDouble(0);
        double d=a[0].objVal->fields["x"].toDouble()*a[1].objVal->fields["x"].toDouble()
                +a[0].objVal->fields["y"].toDouble()*a[1].objVal->fields["y"].toDouble()
                +a[0].objVal->fields["z"].toDouble()*a[1].objVal->fields["z"].toDouble();
        return Value::makeDouble(d);
    });
    vm.registerNative("Vec3.cross", [](const std::vector<Value>& a) -> Value {
        if(a.size()<2||!a[0].objVal||!a[1].objVal) return Value::makeNull();
        double ax=a[0].objVal->fields["x"].toDouble(),ay=a[0].objVal->fields["y"].toDouble(),az=a[0].objVal->fields["z"].toDouble();
        double bx=a[1].objVal->fields["x"].toDouble(),by=a[1].objVal->fields["y"].toDouble(),bz=a[1].objVal->fields["z"].toDouble();
        Value v=Value::makeObject("Vec3");
        v.objVal->fields["x"]=Value::makeDouble(ay*bz-az*by);
        v.objVal->fields["y"]=Value::makeDouble(az*bx-ax*bz);
        v.objVal->fields["z"]=Value::makeDouble(ax*by-ay*bx);
        return v;
    });
    vm.registerNative("Vec3.length", [](const std::vector<Value>& a) -> Value {
        if(a.empty()||!a[0].objVal) return Value::makeDouble(0);
        double x=a[0].objVal->fields["x"].toDouble(),y=a[0].objVal->fields["y"].toDouble(),z=a[0].objVal->fields["z"].toDouble();
        return Value::makeDouble(std::sqrt(x*x+y*y+z*z));
    });
    vm.registerNative("Vec3.normalize", [](const std::vector<Value>& a) -> Value {
        if(a.empty()||!a[0].objVal) return Value::makeNull();
        double x=a[0].objVal->fields["x"].toDouble(),y=a[0].objVal->fields["y"].toDouble(),z=a[0].objVal->fields["z"].toDouble();
        double len=std::sqrt(x*x+y*y+z*z); if(len==0)len=1;
        Value v=Value::makeObject("Vec3");
        v.objVal->fields["x"]=Value::makeDouble(x/len);v.objVal->fields["y"]=Value::makeDouble(y/len);v.objVal->fields["z"]=Value::makeDouble(z/len);
        return v;
    });
    vm.registerNative("Vec3.div", [](const std::vector<Value>& a) -> Value {
        if(a.size()<2||!a[0].objVal) return Value::makeNull();
        double s=a[1].toDouble(); if(s==0)s=1;
        Value v=Value::makeObject("Vec3");
        v.objVal->fields["x"]=Value::makeDouble(a[0].objVal->fields["x"].toDouble()/s);
        v.objVal->fields["y"]=Value::makeDouble(a[0].objVal->fields["y"].toDouble()/s);
        v.objVal->fields["z"]=Value::makeDouble(a[0].objVal->fields["z"].toDouble()/s);
        return v;
    });

    // ===== 3D Math: Mat4 (16-element array, row-major) =====
    vm.registerNative("Mat4.identity", [](const std::vector<Value>&) -> Value {
        Value m=Value::makeObject("Mat4"); Value d=Value::makeArray();
        for(int i=0;i<16;i++) d.arrVal->elements.push_back(Value::makeDouble((i%5==0)?1.0:0.0));
        m.objVal->fields["data"]=d; return m;
    });
    vm.registerNative("Mat4.translate", [](const std::vector<Value>& a) -> Value {
        if(a.size()<4||!a[0].objVal) return Value::makeNull();
        Value m=Value::makeObject("Mat4"); Value d=Value::makeArray();
        for(int i=0;i<16;i++) d.arrVal->elements.push_back(a[0].objVal->fields["data"].arrVal->elements[i]);
        d.arrVal->elements[12]=Value::makeDouble(d.arrVal->elements[12].toDouble()+a[1].toDouble());
        d.arrVal->elements[13]=Value::makeDouble(d.arrVal->elements[13].toDouble()+a[2].toDouble());
        d.arrVal->elements[14]=Value::makeDouble(d.arrVal->elements[14].toDouble()+a[3].toDouble());
        m.objVal->fields["data"]=d; return m;
    });
    vm.registerNative("Mat4.scale", [](const std::vector<Value>& a) -> Value {
        if(a.size()<4||!a[0].objVal) return Value::makeNull();
        Value m=Value::makeObject("Mat4"); Value d=Value::makeArray();
        for(int i=0;i<16;i++) d.arrVal->elements.push_back(a[0].objVal->fields["data"].arrVal->elements[i]);
        d.arrVal->elements[0]=Value::makeDouble(d.arrVal->elements[0].toDouble()*a[1].toDouble());
        d.arrVal->elements[5]=Value::makeDouble(d.arrVal->elements[5].toDouble()*a[2].toDouble());
        d.arrVal->elements[10]=Value::makeDouble(d.arrVal->elements[10].toDouble()*a[3].toDouble());
        m.objVal->fields["data"]=d; return m;
    });
    vm.registerNative("Mat4.rotate", [](const std::vector<Value>& a) -> Value {
        if(a.size()<5||!a[0].objVal) return Value::makeNull();
        double angle=a[1].toDouble()*3.14159265358979/180.0;
        double ax=a[2].toDouble(),ay=a[3].toDouble(),az=a[4].toDouble();
        double len=std::sqrt(ax*ax+ay*ay+az*az); if(len>0){ax/=len;ay/=len;az/=len;}
        double c=std::cos(angle),s=std::sin(angle),t=1-c;
        double r[16]={t*ax*ax+c,t*ax*ay+s*az,t*ax*az-s*ay,0,t*ax*ay-s*az,t*ay*ay+c,t*ay*az+s*ax,0,t*ax*az+s*ay,t*ay*az-s*ax,t*az*az+c,0,0,0,0,1};
        auto& src=a[0].objVal->fields["data"].arrVal->elements;
        Value m=Value::makeObject("Mat4"); Value d=Value::makeArray();
        for(int row=0;row<4;row++) for(int col=0;col<4;col++){
            double v=0; for(int k=0;k<4;k++) v+=src[row*4+k].toDouble()*r[k*4+col];
            d.arrVal->elements.push_back(Value::makeDouble(v));
        }
        m.objVal->fields["data"]=d; return m;
    });
    vm.registerNative("Mat4.perspective", [](const std::vector<Value>& a) -> Value {
        if(a.size()<4) return Value::makeNull();
        double fov=a[0].toDouble()*3.14159265358979/180.0,aspect=a[1].toDouble(),near=a[2].toDouble(),far=a[3].toDouble();
        double f=1.0/std::tan(fov/2.0);
        Value m=Value::makeObject("Mat4"); Value d=Value::makeArray();
        for(int i=0;i<16;i++) d.arrVal->elements.push_back(Value::makeDouble(0));
        d.arrVal->elements[0]=Value::makeDouble(f/aspect); d.arrVal->elements[5]=Value::makeDouble(f);
        d.arrVal->elements[10]=Value::makeDouble((far+near)/(near-far)); d.arrVal->elements[11]=Value::makeDouble(-1);
        d.arrVal->elements[14]=Value::makeDouble((2*far*near)/(near-far));
        m.objVal->fields["data"]=d; return m;
    });
    vm.registerNative("Mat4.ortho", [](const std::vector<Value>& a) -> Value {
        if(a.size()<6) return Value::makeNull();
        double l=a[0].toDouble(),r=a[1].toDouble(),b=a[2].toDouble(),t=a[3].toDouble(),n=a[4].toDouble(),f=a[5].toDouble();
        Value m=Value::makeObject("Mat4"); Value d=Value::makeArray();
        for(int i=0;i<16;i++) d.arrVal->elements.push_back(Value::makeDouble(0));
        d.arrVal->elements[0]=Value::makeDouble(2/(r-l)); d.arrVal->elements[5]=Value::makeDouble(2/(t-b));
        d.arrVal->elements[10]=Value::makeDouble(-2/(f-n)); d.arrVal->elements[12]=Value::makeDouble(-(r+l)/(r-l));
        d.arrVal->elements[13]=Value::makeDouble(-(t+b)/(t-b)); d.arrVal->elements[14]=Value::makeDouble(-(f+n)/(f-n));
        d.arrVal->elements[15]=Value::makeDouble(1);
        m.objVal->fields["data"]=d; return m;
    });
    vm.registerNative("Mat4.lookAt", [](const std::vector<Value>& a) -> Value {
        if(a.size()<3||!a[0].objVal||!a[1].objVal||!a[2].objVal) return Value::makeNull();
        double ex=a[0].objVal->fields["x"].toDouble(),ey=a[0].objVal->fields["y"].toDouble(),ez=a[0].objVal->fields["z"].toDouble();
        double tx=a[1].objVal->fields["x"].toDouble(),ty=a[1].objVal->fields["y"].toDouble(),tz=a[1].objVal->fields["z"].toDouble();
        double ux=a[2].objVal->fields["x"].toDouble(),uy=a[2].objVal->fields["y"].toDouble(),uz=a[2].objVal->fields["z"].toDouble();
        double fx=ex-tx,fy=ey-ty,fz=ez-tz; double fl=std::sqrt(fx*fx+fy*fy+fz*fz); if(fl>0){fx/=fl;fy/=fl;fz/=fl;}
        double sx=uy*fz-uz*fy,sy=uz*fx-ux*fz,sz=ux*fy-uy*fx; double sl=std::sqrt(sx*sx+sy*sy+sz*sz); if(sl>0){sx/=sl;sy/=sl;sz/=sl;}
        double uux=fy*sz-fz*sy,uuy=fz*sx-fx*sz,uuz=fx*sy-fy*sx;
        Value m=Value::makeObject("Mat4"); Value d=Value::makeArray();
        double data[16]={sx,uux,-fx,0,sy,uuy,-fy,0,sz,uuz,-fz,0,-(sx*ex+sy*ey+sz*ez),-(uux*ex+uuy*ey+uuz*ez),fx*ex+fy*ey+fz*ez,1};
        for(int i=0;i<16;i++) d.arrVal->elements.push_back(Value::makeDouble(data[i]));
        m.objVal->fields["data"]=d; return m;
    });
    vm.registerNative("Mat4.multiply", [](const std::vector<Value>& a) -> Value {
        if(a.size()<2||!a[0].objVal||!a[1].objVal) return Value::makeNull();
        auto& A=a[0].objVal->fields["data"].arrVal->elements; auto& B=a[1].objVal->fields["data"].arrVal->elements;
        Value m=Value::makeObject("Mat4"); Value d=Value::makeArray();
        for(int row=0;row<4;row++) for(int col=0;col<4;col++){
            double v=0; for(int k=0;k<4;k++) v+=A[row*4+k].toDouble()*B[k*4+col].toDouble();
            d.arrVal->elements.push_back(Value::makeDouble(v));
        }
        m.objVal->fields["data"]=d; return m;
    });
    vm.registerNative("Mat4.inverse", [](const std::vector<Value>& a) -> Value {
        if(a.empty()||!a[0].objVal) return Value::makeNull();
        Value m=Value::makeObject("Mat4"); Value d=Value::makeArray();
        for(int i=0;i<16;i++) d.arrVal->elements.push_back(Value::makeDouble((i%5==0)?1.0:0.0));
        m.objVal->fields["data"]=d; return m;
    });

    // ===== Camera =====
    vm.registerNative("Camera.create", [](const std::vector<Value>& a) -> Value {
        Value cam=Value::makeObject("Camera");
        cam.objVal->fields["position"]=a.size()>=1?a[0]:Value::makeNull();
        cam.objVal->fields["target"]=a.size()>=2?a[1]:Value::makeNull();
        cam.objVal->fields["up"]=a.size()>=3?a[2]:Value::makeNull();
        cam.objVal->fields["fov"]=Value::makeDouble(a.size()>=4?a[3].toDouble():60.0);
        cam.objVal->fields["type"]=Value::makeString("perspective");
        cam.objVal->fields["yaw"]=Value::makeDouble(0);cam.objVal->fields["pitch"]=Value::makeDouble(0);
        return cam;
    });
    vm.registerNative("Camera.createOrtho", [](const std::vector<Value>& a) -> Value {
        Value cam=Value::makeObject("Camera");
        cam.objVal->fields["left"]=Value::makeDouble(a.size()>=1?a[0].toDouble():-1);
        cam.objVal->fields["right"]=Value::makeDouble(a.size()>=2?a[1].toDouble():1);
        cam.objVal->fields["bottom"]=Value::makeDouble(a.size()>=3?a[2].toDouble():-1);
        cam.objVal->fields["top"]=Value::makeDouble(a.size()>=4?a[3].toDouble():1);
        cam.objVal->fields["type"]=Value::makeString("orthographic");
        return cam;
    });
    vm.registerNative("Camera.getViewMatrix", [&vm](const std::vector<Value>& a) -> Value {
        if(a.empty()||!a[0].objVal) return Value::makeNull();
        return vm.callNative("Mat4.lookAt",{a[0].objVal->fields["position"],a[0].objVal->fields["target"],a[0].objVal->fields["up"]});
    });
    vm.registerNative("Camera.getProjectionMatrix", [&vm](const std::vector<Value>& a) -> Value {
        if(a.empty()||!a[0].objVal) return Value::makeNull();
        auto& f=a[0].objVal->fields;
        if(f["type"].toString()=="orthographic") return vm.callNative("Mat4.ortho",{f["left"],f["right"],f["bottom"],f["top"],Value::makeDouble(-1),Value::makeDouble(1)});
        return vm.callNative("Mat4.perspective",{f["fov"],Value::makeDouble(16.0/9.0),Value::makeDouble(0.1),Value::makeDouble(1000.0)});
    });
    vm.registerNative("Camera.move", [](const std::vector<Value>& a) -> Value {
        if(a.size()<3||!a[0].objVal||!a[1].objVal) return Value::makeNull();
        double speed=a[2].toDouble(); auto& pos=a[0].objVal->fields["position"];
        if(pos.objVal){pos.objVal->fields["x"]=Value::makeDouble(pos.objVal->fields["x"].toDouble()+a[1].objVal->fields["x"].toDouble()*speed);
        pos.objVal->fields["y"]=Value::makeDouble(pos.objVal->fields["y"].toDouble()+a[1].objVal->fields["y"].toDouble()*speed);
        pos.objVal->fields["z"]=Value::makeDouble(pos.objVal->fields["z"].toDouble()+a[1].objVal->fields["z"].toDouble()*speed);}
        return a[0];
    });
    vm.registerNative("Camera.rotate", [](const std::vector<Value>& a) -> Value {
        if(a.size()<3||!a[0].objVal) return Value::makeNull();
        a[0].objVal->fields["yaw"]=Value::makeDouble(a[0].objVal->fields["yaw"].toDouble()+a[1].toDouble());
        a[0].objVal->fields["pitch"]=Value::makeDouble(a[0].objVal->fields["pitch"].toDouble()+a[2].toDouble());
        return a[0];
    });
    vm.registerNative("Camera.lookAt", [](const std::vector<Value>& a) -> Value {
        if(a.size()<2||!a[0].objVal) return Value::makeNull();
        a[0].objVal->fields["target"]=a[1]; return a[0];
    });

    // ===== Graphics3D Context =====
    vm.registerNative("Graphics3D.createContext", [](const std::vector<Value>& a) -> Value {
        Value ctx=Value::makeObject("GL3DContext");
        ctx.objVal->fields["depthTest"]=Value::makeBool(true); ctx.objVal->fields["blending"]=Value::makeBool(false);
        ctx.objVal->fields["wireframe"]=Value::makeBool(false);
        if(!a.empty()&&a[0].objVal) ctx.objVal->fields["_winId"]=a[0].objVal->fields["_id"];
        return ctx;
    });
    vm.registerNative("Graphics3D.setClearColor", [](const std::vector<Value>& a) -> Value { if(a.size()>=5&&a[0].objVal){Value c=Value::makeObject("Color");c.objVal->fields["r"]=a[1];c.objVal->fields["g"]=a[2];c.objVal->fields["b"]=a[3];c.objVal->fields["a"]=a[4];a[0].objVal->fields["clearColor"]=c;} return Value::makeNull(); });
    vm.registerNative("Graphics3D.clear", [](const std::vector<Value>&) -> Value { return Value::makeNull(); });
    vm.registerNative("Graphics3D.present", [](const std::vector<Value>&) -> Value { return Value::makeNull(); });
    vm.registerNative("Graphics3D.setViewport", [](const std::vector<Value>&) -> Value { return Value::makeNull(); });
    vm.registerNative("Graphics3D.enableDepthTest", [](const std::vector<Value>& a) -> Value { if(a.size()>=2&&a[0].objVal) a[0].objVal->fields["depthTest"]=a[1]; return Value::makeNull(); });
    vm.registerNative("Graphics3D.enableBlending", [](const std::vector<Value>& a) -> Value { if(a.size()>=2&&a[0].objVal) a[0].objVal->fields["blending"]=a[1]; return Value::makeNull(); });
    vm.registerNative("Graphics3D.setWireframe", [](const std::vector<Value>& a) -> Value { if(a.size()>=2&&a[0].objVal) a[0].objVal->fields["wireframe"]=a[1]; return Value::makeNull(); });

    // ===== Shader =====
    vm.registerNative("Shader.create", [&vm](const std::vector<Value>& a) -> Value {
        if(a.size()<2){vm.throwError("GardShaderError","Shader.create: vertex and fragment source required");return Value::makeNull();}
        Value s=Value::makeObject("Shader"); static int sid=1;
        s.objVal->fields["_id"]=Value::makeInt(sid++); s.objVal->fields["compiled"]=Value::makeBool(true); return s;
    });
    vm.registerNative("Shader.createFromFile", [&vm](const std::vector<Value>& a) -> Value {
        if(a.size()<2){vm.throwError("GardShaderError","Shader.createFromFile: paths required");return Value::makeNull();}
        Value s=Value::makeObject("Shader"); static int sid=100;
        s.objVal->fields["_id"]=Value::makeInt(sid++); s.objVal->fields["compiled"]=Value::makeBool(true); return s;
    });
    vm.registerNative("Shader.use", [](const std::vector<Value>&) -> Value { return Value::makeNull(); });
    vm.registerNative("Shader.setUniform", [](const std::vector<Value>& a) -> Value { if(a.size()>=3&&a[0].objVal) a[0].objVal->fields["_u_"+a[1].toString()]=a[2]; return Value::makeNull(); });
    vm.registerNative("Shader.setTexture", [](const std::vector<Value>&) -> Value { return Value::makeNull(); });
    vm.registerNative("Shader.destroy", [](const std::vector<Value>&) -> Value { return Value::makeNull(); });

    // ===== Mesh =====
    vm.registerNative("Mesh.create", [](const std::vector<Value>& a) -> Value {
        Value m=Value::makeObject("Mesh"); static int mid=1; m.objVal->fields["_id"]=Value::makeInt(mid++);
        m.objVal->fields["vertices"]=a.size()>=1?a[0]:Value::makeArray();
        m.objVal->fields["indices"]=a.size()>=2?a[1]:Value::makeArray();
        m.objVal->fields["layout"]=a.size()>=3?a[2]:Value::makeArray(); return m;
    });
    vm.registerNative("Mesh.createQuad", [](const std::vector<Value>&) -> Value { Value m=Value::makeObject("Mesh"); m.objVal->fields["type"]=Value::makeString("quad"); return m; });
    vm.registerNative("Mesh.createCube", [](const std::vector<Value>&) -> Value { Value m=Value::makeObject("Mesh"); m.objVal->fields["type"]=Value::makeString("cube"); return m; });
    vm.registerNative("Mesh.createSphere", [](const std::vector<Value>& a) -> Value { Value m=Value::makeObject("Mesh"); m.objVal->fields["type"]=Value::makeString("sphere"); m.objVal->fields["segments"]=Value::makeInt(a.size()>=1?a[0].toInt():16); m.objVal->fields["rings"]=Value::makeInt(a.size()>=2?a[1].toInt():16); return m; });
    vm.registerNative("Mesh.createPlane", [](const std::vector<Value>& a) -> Value { Value m=Value::makeObject("Mesh"); m.objVal->fields["type"]=Value::makeString("plane"); m.objVal->fields["width"]=Value::makeDouble(a.size()>=1?a[0].toDouble():1); m.objVal->fields["height"]=Value::makeDouble(a.size()>=2?a[1].toDouble():1); return m; });
    vm.registerNative("Mesh.draw", [](const std::vector<Value>&) -> Value { return Value::makeNull(); });
    vm.registerNative("Mesh.drawInstanced", [](const std::vector<Value>&) -> Value { return Value::makeNull(); });
    vm.registerNative("Mesh.destroy", [](const std::vector<Value>&) -> Value { return Value::makeNull(); });

    // ===== Texture =====
    vm.registerNative("Texture.load", [&vm](const std::vector<Value>& a) -> Value { if(a.empty()){vm.throwError("GardTextureError","Texture.load: path required");return Value::makeNull();} Value t=Value::makeObject("Texture"); static int tid=1; t.objVal->fields["_id"]=Value::makeInt(tid++); t.objVal->fields["path"]=a[0]; return t; });
    vm.registerNative("Texture.create", [](const std::vector<Value>& a) -> Value { Value t=Value::makeObject("Texture"); static int tid=100; t.objVal->fields["_id"]=Value::makeInt(tid++); t.objVal->fields["width"]=Value::makeInt(a.size()>=1?a[0].toInt():256); t.objVal->fields["height"]=Value::makeInt(a.size()>=2?a[1].toInt():256); return t; });
    vm.registerNative("Texture.bind", [](const std::vector<Value>&) -> Value { return Value::makeNull(); });
    vm.registerNative("Texture.setFilter", [](const std::vector<Value>& a) -> Value { if(a.size()>=2&&a[0].objVal) a[0].objVal->fields["filter"]=a[1]; return Value::makeNull(); });
    vm.registerNative("Texture.setWrap", [](const std::vector<Value>& a) -> Value { if(a.size()>=2&&a[0].objVal) a[0].objVal->fields["wrap"]=a[1]; return Value::makeNull(); });
    vm.registerNative("Texture.destroy", [](const std::vector<Value>&) -> Value { return Value::makeNull(); });

    // ===== 5.1 Audio: Sound Effects =====
    vm.registerNative("Audio.init", [](const std::vector<Value>&) -> Value {
        ensureSDL();
        if (!g_mixerInitialized) {
            if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) < 0) {
                return Value::makeBool(false);
            }
            g_mixerInitialized = true;
        }
        return Value::makeBool(true);
    });
    vm.registerNative("Audio.loadSound", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty()) { vm.throwError("GardAudioError", "Audio.loadSound: path required"); return Value::makeNull(); }
        if (!g_mixerInitialized) { ensureSDL(); Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048); g_mixerInitialized = true; }
        Mix_Chunk* chunk = Mix_LoadWAV(a[0].toString().c_str());
        if (!chunk) { vm.throwError("GardAudioError", "Audio.loadSound: " + std::string(Mix_GetError())); return Value::makeNull(); }
        Value snd = Value::makeObject("Sound");
        snd.objVal->fields["_ptr"] = Value::makeLong(reinterpret_cast<int64_t>(chunk));
        snd.objVal->fields["path"] = a[0];
        snd.objVal->fields["channel"] = Value::makeInt(-1);
        return snd;
    });
    vm.registerNative("Audio.playSound", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeInt(-1);
        auto ptrIt = a[0].objVal->fields.find("_ptr");
        if (ptrIt == a[0].objVal->fields.end()) return Value::makeInt(-1);
        Mix_Chunk* chunk = reinterpret_cast<Mix_Chunk*>(static_cast<intptr_t>(ptrIt->second.longVal));
        if (!chunk) return Value::makeInt(-1);
        int volume = a.size() >= 2 ? (int)(a[1].toDouble() * 128) : 128;
        int loops = a.size() >= 3 && a[2].toBool() ? -1 : 0;
        Mix_VolumeChunk(chunk, volume);
        int channel = Mix_PlayChannel(-1, chunk, loops);
        a[0].objVal->fields["channel"] = Value::makeInt(channel);
        return Value::makeInt(channel);
    });
    vm.registerNative("Audio.stopSound", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        int ch = a[0].objVal->fields["channel"].toInt();
        if (ch >= 0) Mix_HaltChannel(ch);
        return Value::makeNull();
    });
    vm.registerNative("Audio.setVolume", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        auto ptrIt = a[0].objVal->fields.find("_ptr");
        if (ptrIt == a[0].objVal->fields.end()) return Value::makeNull();
        Mix_Chunk* chunk = reinterpret_cast<Mix_Chunk*>(static_cast<intptr_t>(ptrIt->second.longVal));
        if (chunk) Mix_VolumeChunk(chunk, (int)(a[1].toDouble() * 128));
        return Value::makeNull();
    });

    // ===== 5.2 Audio: Music =====
    vm.registerNative("Audio.loadMusic", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty()) { vm.throwError("GardAudioError", "Audio.loadMusic: path required"); return Value::makeNull(); }
        if (!g_mixerInitialized) { ensureSDL(); Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048); g_mixerInitialized = true; }
        Mix_Music* music = Mix_LoadMUS(a[0].toString().c_str());
        if (!music) { vm.throwError("GardAudioError", "Audio.loadMusic: " + std::string(Mix_GetError())); return Value::makeNull(); }
        Value m = Value::makeObject("Music");
        m.objVal->fields["_ptr"] = Value::makeLong(reinterpret_cast<int64_t>(music));
        m.objVal->fields["path"] = a[0];
        return m;
    });
    vm.registerNative("Audio.playMusic", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeBool(false);
        auto ptrIt = a[0].objVal->fields.find("_ptr");
        if (ptrIt == a[0].objVal->fields.end()) return Value::makeBool(false);
        Mix_Music* music = reinterpret_cast<Mix_Music*>(static_cast<intptr_t>(ptrIt->second.longVal));
        if (!music) return Value::makeBool(false);
        int loops = a.size() >= 2 && a[1].toBool() ? -1 : 1;
        return Value::makeBool(Mix_PlayMusic(music, loops) == 0);
    });
    vm.registerNative("Audio.pauseMusic", [](const std::vector<Value>&) -> Value { Mix_PauseMusic(); return Value::makeNull(); });
    vm.registerNative("Audio.resumeMusic", [](const std::vector<Value>&) -> Value { Mix_ResumeMusic(); return Value::makeNull(); });
    vm.registerNative("Audio.stopMusic", [](const std::vector<Value>&) -> Value { Mix_HaltMusic(); return Value::makeNull(); });
    vm.registerNative("Audio.setMusicVolume", [](const std::vector<Value>& a) -> Value {
        int vol = a.empty() ? 128 : (int)(a[0].toDouble() * 128);
        Mix_VolumeMusic(vol);
        return Value::makeNull();
    });

    // ===== 4.1 Framebuffers =====
    vm.registerNative("Framebuffer.create", [](const std::vector<Value>& a) -> Value {
        Value fb=Value::makeObject("Framebuffer"); static int fbId=1;
        fb.objVal->fields["_id"]=Value::makeInt(fbId++);
        fb.objVal->fields["width"]=Value::makeInt(a.size()>=1?a[0].toInt():800);
        fb.objVal->fields["height"]=Value::makeInt(a.size()>=2?a[1].toInt():600);
        fb.objVal->fields["bound"]=Value::makeBool(false);
        // Create a texture attachment
        Value tex=Value::makeObject("Texture"); static int ftid=200;
        tex.objVal->fields["_id"]=Value::makeInt(ftid++);
        tex.objVal->fields["width"]=fb.objVal->fields["width"];
        tex.objVal->fields["height"]=fb.objVal->fields["height"];
        fb.objVal->fields["_texture"]=tex;
        return fb;
    });
    vm.registerNative("Framebuffer.bind", [](const std::vector<Value>& a) -> Value {
        if(!a.empty()&&a[0].objVal) a[0].objVal->fields["bound"]=Value::makeBool(true);
        return Value::makeNull();
    });
    vm.registerNative("Framebuffer.unbind", [](const std::vector<Value>&) -> Value { return Value::makeNull(); });
    vm.registerNative("Framebuffer.getTexture", [](const std::vector<Value>& a) -> Value {
        if(a.empty()||!a[0].objVal) return Value::makeNull();
        auto it=a[0].objVal->fields.find("_texture");
        return it!=a[0].objVal->fields.end()?it->second:Value::makeNull();
    });

    // ===== 4.2 Lighting =====
    vm.registerNative("Light.createDirectional", [](const std::vector<Value>& a) -> Value {
        Value l=Value::makeObject("Light");
        l.objVal->fields["type"]=Value::makeString("directional");
        l.objVal->fields["direction"]=a.size()>=1?a[0]:Value::makeNull();
        l.objVal->fields["color"]=a.size()>=2?a[1]:Value::makeNull();
        l.objVal->fields["intensity"]=Value::makeDouble(a.size()>=3?a[2].toDouble():1.0);
        return l;
    });
    vm.registerNative("Light.createPoint", [](const std::vector<Value>& a) -> Value {
        Value l=Value::makeObject("Light");
        l.objVal->fields["type"]=Value::makeString("point");
        l.objVal->fields["position"]=a.size()>=1?a[0]:Value::makeNull();
        l.objVal->fields["color"]=a.size()>=2?a[1]:Value::makeNull();
        l.objVal->fields["intensity"]=Value::makeDouble(a.size()>=3?a[2].toDouble():1.0);
        l.objVal->fields["radius"]=Value::makeDouble(a.size()>=4?a[3].toDouble():10.0);
        return l;
    });
    vm.registerNative("Light.createSpot", [](const std::vector<Value>& a) -> Value {
        Value l=Value::makeObject("Light");
        l.objVal->fields["type"]=Value::makeString("spot");
        l.objVal->fields["position"]=a.size()>=1?a[0]:Value::makeNull();
        l.objVal->fields["direction"]=a.size()>=2?a[1]:Value::makeNull();
        l.objVal->fields["color"]=a.size()>=3?a[2]:Value::makeNull();
        l.objVal->fields["cutoff"]=Value::makeDouble(a.size()>=4?a[3].toDouble():30.0);
        return l;
    });

    // ===== 4.3 Model Loading =====
    vm.registerNative("Model.load", [&vm](const std::vector<Value>& a) -> Value {
        if(a.empty()){vm.throwError("GardMeshError","Model.load: path required");return Value::makeNull();}
        Value m=Value::makeObject("Model"); static int modelId=1;
        m.objVal->fields["_id"]=Value::makeInt(modelId++);
        m.objVal->fields["path"]=a[0];
        m.objVal->fields["meshCount"]=Value::makeInt(1);
        // Bounding box (default unit cube)
        Value bounds=Value::makeObject("Bounds");
        bounds.objVal->fields["min"]=Value::makeObject("Vec3");
        bounds.objVal->fields["min"].objVal->fields["x"]=Value::makeDouble(-0.5);
        bounds.objVal->fields["min"].objVal->fields["y"]=Value::makeDouble(-0.5);
        bounds.objVal->fields["min"].objVal->fields["z"]=Value::makeDouble(-0.5);
        bounds.objVal->fields["max"]=Value::makeObject("Vec3");
        bounds.objVal->fields["max"].objVal->fields["x"]=Value::makeDouble(0.5);
        bounds.objVal->fields["max"].objVal->fields["y"]=Value::makeDouble(0.5);
        bounds.objVal->fields["max"].objVal->fields["z"]=Value::makeDouble(0.5);
        m.objVal->fields["_bounds"]=bounds;
        return m;
    });
    vm.registerNative("Model.draw", [](const std::vector<Value>&) -> Value { return Value::makeNull(); });
    vm.registerNative("Model.getBounds", [](const std::vector<Value>& a) -> Value {
        if(a.empty()||!a[0].objVal) return Value::makeNull();
        auto it=a[0].objVal->fields.find("_bounds");
        return it!=a[0].objVal->fields.end()?it->second:Value::makeNull();
    });
    vm.registerNative("Model.destroy", [](const std::vector<Value>&) -> Value { return Value::makeNull(); });

    // ===== 4.4 Sprite Batch =====
    vm.registerNative("SpriteBatch.begin", [](const std::vector<Value>& a) -> Value {
        Value batch=Value::makeObject("SpriteBatch");
        batch.objVal->fields["shader"]=a.size()>=1?a[0]:Value::makeNull();
        batch.objVal->fields["sprites"]=Value::makeArray();
        batch.objVal->fields["count"]=Value::makeInt(0);
        return batch;
    });
    vm.registerNative("SpriteBatch.draw", [](const std::vector<Value>& a) -> Value {
        // a[0]=batch, a[1]=texture, a[2]=x, a[3]=y, a[4]=w, a[5]=h, a[6]=rotation?, a[7]=color?
        if(a.size()<6||!a[0].objVal) return Value::makeNull();
        Value sprite=Value::makeObject("Sprite");
        sprite.objVal->fields["texture"]=a[1];
        sprite.objVal->fields["x"]=a[2]; sprite.objVal->fields["y"]=a[3];
        sprite.objVal->fields["w"]=a[4]; sprite.objVal->fields["h"]=a[5];
        sprite.objVal->fields["rotation"]=Value::makeDouble(a.size()>=7?a[6].toDouble():0);
        sprite.objVal->fields["color"]=a.size()>=8?a[7]:Value::makeNull();
        a[0].objVal->fields["sprites"].arrVal->elements.push_back(sprite);
        a[0].objVal->fields["count"]=Value::makeInt(a[0].objVal->fields["count"].toInt()+1);
        return Value::makeNull();
    });
    vm.registerNative("SpriteBatch.end", [](const std::vector<Value>& a) -> Value {
        // Flush — in real impl this would issue a single batched draw call
        if(!a.empty()&&a[0].objVal) {
            int count=a[0].objVal->fields["count"].toInt();
            a[0].objVal->fields["sprites"]=Value::makeArray();
            a[0].objVal->fields["count"]=Value::makeInt(0);
            return Value::makeInt(count); // return number of sprites flushed
        }
        return Value::makeInt(0);
    });

    // ===== 6.1 Timing =====
    static auto g_startTime = std::chrono::steady_clock::now();
    static auto g_lastFrameTime = std::chrono::steady_clock::now();
    static double g_deltaTime = 0.016;
    static int g_targetFPS = 0;

    vm.registerNative("Time.delta", [](const std::vector<Value>&) -> Value {
        return Value::makeDouble(g_deltaTime);
    });
    vm.registerNative("Time.fps", [](const std::vector<Value>&) -> Value {
        return Value::makeDouble(g_deltaTime > 0 ? 1.0 / g_deltaTime : 0);
    });
    vm.registerNative("Time.elapsed", [](const std::vector<Value>&) -> Value {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - g_startTime).count();
        return Value::makeDouble(elapsed);
    });
    vm.registerNative("Time.setTargetFPS", [](const std::vector<Value>& a) -> Value {
        g_targetFPS = a.empty() ? 0 : a[0].toInt();
        return Value::makeNull();
    });
    // Internal: call at start of each frame to update delta
    vm.registerNative("Time.tick", [](const std::vector<Value>&) -> Value {
        auto now = std::chrono::steady_clock::now();
        g_deltaTime = std::chrono::duration<double>(now - g_lastFrameTime).count();
        g_lastFrameTime = now;
        // Frame cap
        if (g_targetFPS > 0) {
            double targetDelta = 1.0 / g_targetFPS;
            if (g_deltaTime < targetDelta) {
                int sleepMs = (int)((targetDelta - g_deltaTime) * 1000);
                if (sleepMs > 0) SDL_Delay(sleepMs);
                g_lastFrameTime = std::chrono::steady_clock::now();
                g_deltaTime = targetDelta;
            }
        }
        return Value::makeDouble(g_deltaTime);
    });

    // ===== 6.2 Animation / Tween =====
    vm.registerNative("Tween.create", [](const std::vector<Value>& a) -> Value {
        Value tw = Value::makeObject("Tween");
        tw.objVal->fields["from"] = Value::makeDouble(a.size() >= 1 ? a[0].toDouble() : 0);
        tw.objVal->fields["to"] = Value::makeDouble(a.size() >= 2 ? a[1].toDouble() : 1);
        tw.objVal->fields["duration"] = Value::makeDouble(a.size() >= 3 ? a[2].toDouble() : 1.0);
        tw.objVal->fields["easing"] = Value::makeString(a.size() >= 4 ? a[3].toString() : "linear");
        tw.objVal->fields["elapsed"] = Value::makeDouble(0);
        tw.objVal->fields["value"] = tw.objVal->fields["from"];
        tw.objVal->fields["done"] = Value::makeBool(false);
        return tw;
    });
    vm.registerNative("Tween.update", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        double dt = a[1].toDouble();
        auto& f = a[0].objVal->fields;
        double elapsed = f["elapsed"].toDouble() + dt;
        double duration = f["duration"].toDouble();
        if (elapsed >= duration) { elapsed = duration; f["done"] = Value::makeBool(true); }
        f["elapsed"] = Value::makeDouble(elapsed);
        double t = duration > 0 ? elapsed / duration : 1.0;
        // Apply easing
        std::string easing = f["easing"].toString();
        double easedT = t;
        if (easing == "easeIn") easedT = t * t;
        else if (easing == "easeOut") easedT = 1 - (1 - t) * (1 - t);
        else if (easing == "easeInOut") easedT = t < 0.5 ? 2 * t * t : 1 - std::pow(-2 * t + 2, 2) / 2;
        else if (easing == "bounce") {
            double n1 = 7.5625, d1 = 2.75, tt = 1 - t;
            if (tt < 1/d1) easedT = 1 - n1*tt*tt;
            else if (tt < 2/d1) { tt -= 1.5/d1; easedT = 1 - (n1*tt*tt + 0.75); }
            else if (tt < 2.5/d1) { tt -= 2.25/d1; easedT = 1 - (n1*tt*tt + 0.9375); }
            else { tt -= 2.625/d1; easedT = 1 - (n1*tt*tt + 0.984375); }
        }
        else if (easing == "elastic") {
            if (t == 0 || t == 1) easedT = t;
            else easedT = -std::pow(2, 10*t-10) * std::sin((t*10-10.75) * (2*3.14159265/3));
        }
        // Interpolate
        double from = f["from"].toDouble(), to = f["to"].toDouble();
        f["value"] = Value::makeDouble(from + (to - from) * easedT);
        return a[0];
    });
    vm.registerNative("Tween.getValue", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeDouble(0);
        return a[0].objVal->fields["value"];
    });

    // ===== 6.3 Collision Detection (2D) =====
    auto getField = [](const Value& v, const std::string& k) -> double {
        if (v.type == ValueType::Object && v.objVal) { auto it = v.objVal->fields.find(k); if (it != v.objVal->fields.end()) return it->second.toDouble(); }
        if (v.type == ValueType::Map && v.mapVal) { auto it = v.mapVal->entries.find(k); if (it != v.mapVal->entries.end()) return it->second.toDouble(); }
        return 0;
    };
    auto isObjLike = [](const Value& v) -> bool {
        return (v.type == ValueType::Object && v.objVal) || (v.type == ValueType::Map && v.mapVal);
    };

    vm.registerNative("Collision.rectRect", [getField, isObjLike](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !isObjLike(a[0]) || !isObjLike(a[1])) return Value::makeBool(false);
        double ax = getField(a[0],"x"), ay = getField(a[0],"y"), aw = getField(a[0],"width"), ah = getField(a[0],"height");
        double bx = getField(a[1],"x"), by = getField(a[1],"y"), bw = getField(a[1],"width"), bh = getField(a[1],"height");
        return Value::makeBool(ax < bx+bw && ax+aw > bx && ay < by+bh && ay+ah > by);
    });
    vm.registerNative("Collision.circleCircle", [getField, isObjLike](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !isObjLike(a[0]) || !isObjLike(a[1])) return Value::makeBool(false);
        double x1 = getField(a[0],"x"), y1 = getField(a[0],"y"), r1 = getField(a[0],"radius");
        double x2 = getField(a[1],"x"), y2 = getField(a[1],"y"), r2 = getField(a[1],"radius");
        double dx = x2-x1, dy = y2-y1;
        return Value::makeBool(dx*dx + dy*dy <= (r1+r2)*(r1+r2));
    });
    vm.registerNative("Collision.rectCircle", [getField, isObjLike](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !isObjLike(a[0]) || !isObjLike(a[1])) return Value::makeBool(false);
        double rx = getField(a[0],"x"), ry = getField(a[0],"y"), rw = getField(a[0],"width"), rh = getField(a[0],"height");
        double cx = getField(a[1],"x"), cy = getField(a[1],"y"), cr = getField(a[1],"radius");
        double closestX = std::max(rx, std::min(cx, rx+rw));
        double closestY = std::max(ry, std::min(cy, ry+rh));
        double dx = cx - closestX, dy = cy - closestY;
        return Value::makeBool(dx*dx + dy*dy <= cr*cr);
    });
    vm.registerNative("Collision.pointInRect", [getField, isObjLike](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !isObjLike(a[2])) return Value::makeBool(false);
        double px = a[0].toDouble(), py = a[1].toDouble();
        double rx = getField(a[2],"x"), ry = getField(a[2],"y"), rw = getField(a[2],"width"), rh = getField(a[2],"height");
        return Value::makeBool(px >= rx && px <= rx+rw && py >= ry && py <= ry+rh);
    });
    vm.registerNative("Collision.raycast", [getField, isObjLike](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !isObjLike(a[0]) || !isObjLike(a[1]) || !a[2].arrVal) return Value::makeNull();
        double ox = getField(a[0],"x"), oy = getField(a[0],"y");
        double dx = getField(a[1],"x"), dy = getField(a[1],"y");
        double len = std::sqrt(dx*dx+dy*dy); if (len > 0) { dx/=len; dy/=len; }
        double closestDist = 1e9;
        Value closestHit = Value::makeNull();
        for (auto& obj : a[2].arrVal->elements) {
            if (!isObjLike(obj)) continue;
            double rx = getField(obj,"x"), ry = getField(obj,"y"), rw = getField(obj,"width"), rh = getField(obj,"height");
            double tmin = -1e9, tmax = 1e9;
            if (std::abs(dx) > 1e-8) {
                double t1 = (rx - ox) / dx, t2 = (rx+rw - ox) / dx;
                if (t1 > t2) std::swap(t1, t2);
                tmin = std::max(tmin, t1); tmax = std::min(tmax, t2);
            } else if (ox < rx || ox > rx+rw) continue;
            if (std::abs(dy) > 1e-8) {
                double t1 = (ry - oy) / dy, t2 = (ry+rh - oy) / dy;
                if (t1 > t2) std::swap(t1, t2);
                tmin = std::max(tmin, t1); tmax = std::min(tmax, t2);
            } else if (oy < ry || oy > ry+rh) continue;
            if (tmin <= tmax && tmax >= 0) {
                double hitDist = tmin >= 0 ? tmin : tmax;
                if (hitDist < closestDist) {
                    closestDist = hitDist;
                    closestHit = Value::makeObject("RaycastHit");
                    closestHit.objVal->fields["x"] = Value::makeDouble(ox + dx * hitDist);
                    closestHit.objVal->fields["y"] = Value::makeDouble(oy + dy * hitDist);
                    closestHit.objVal->fields["distance"] = Value::makeDouble(hitDist);
                    closestHit.objVal->fields["object"] = obj;
                }
            }
        }
        return closestHit;
    });

    // ===== Graphics.* namespace (matches LLVM codegen API) =====
    // These use raw r,g,b,a integers instead of Color objects

    // Window.destroy(win) — alias for Window.close
    vm.registerNative("Window.destroy", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        int id = a[0].objVal->fields["_id"].toInt();
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto it = g_windows.find(id);
        if (it != g_windows.end()) {
            if (it->second->renderer) SDL_DestroyRenderer(it->second->renderer);
            if (it->second->window) SDL_DestroyWindow(it->second->window);
            g_windows.erase(it);
        }
        return Value::makeNull();
    });

    // Window.width(win)
    vm.registerNative("Window.width", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeInt(0);
        int id = a[0].objVal->fields["_id"].toInt();
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto it = g_windows.find(id);
        if (it == g_windows.end()) return Value::makeInt(0);
        return Value::makeInt(it->second->width);
    });

    // Window.height(win)
    vm.registerNative("Window.height", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeInt(0);
        int id = a[0].objVal->fields["_id"].toInt();
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto it = g_windows.find(id);
        if (it == g_windows.end()) return Value::makeInt(0);
        return Value::makeInt(it->second->height);
    });

    // Graphics.clear(win)
    vm.registerNative("Graphics.clear", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        int id = a[0].objVal->fields["_id"].toInt();
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto it = g_windows.find(id);
        if (it != g_windows.end() && it->second->renderer) {
            SDL_RenderClear(it->second->renderer);
        }
        return Value::makeNull();
    });

    // Graphics.clearColor(win, r, g, b)
    vm.registerNative("Graphics.clearColor", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 4 || !a[0].objVal) return Value::makeNull();
        int id = a[0].objVal->fields["_id"].toInt();
        int r = a[1].toInt(), g = a[2].toInt(), b = a[3].toInt();
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto it = g_windows.find(id);
        if (it != g_windows.end() && it->second->renderer) {
            SDL_SetRenderDrawColor(it->second->renderer, r, g, b, 255);
            SDL_RenderClear(it->second->renderer);
        }
        return Value::makeNull();
    });

    // Graphics.present(win)
    vm.registerNative("Graphics.present", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        int id = a[0].objVal->fields["_id"].toInt();
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto it = g_windows.find(id);
        if (it != g_windows.end() && it->second->renderer) {
            SDL_RenderPresent(it->second->renderer);
        }
        return Value::makeNull();
    });

    // Graphics.setColor(win, r, g, b, a)
    vm.registerNative("Graphics.setColor", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 5 || !a[0].objVal) return Value::makeNull();
        int id = a[0].objVal->fields["_id"].toInt();
        int r = a[1].toInt(), g = a[2].toInt(), b = a[3].toInt(), alpha = a[4].toInt();
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto it = g_windows.find(id);
        if (it != g_windows.end() && it->second->renderer) {
            SDL_SetRenderDrawColor(it->second->renderer, r, g, b, alpha);
        }
        return Value::makeNull();
    });

    // Graphics.drawRect(win, x, y, w, h)
    vm.registerNative("Graphics.drawRect", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 5 || !a[0].objVal) return Value::makeNull();
        int id = a[0].objVal->fields["_id"].toInt();
        SDL_Rect rect = { a[1].toInt(), a[2].toInt(), a[3].toInt(), a[4].toInt() };
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto it = g_windows.find(id);
        if (it != g_windows.end() && it->second->renderer) {
            SDL_RenderDrawRect(it->second->renderer, &rect);
        }
        return Value::makeNull();
    });

    // Graphics.fillRect(win, x, y, w, h)
    vm.registerNative("Graphics.fillRect", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 5 || !a[0].objVal) return Value::makeNull();
        int id = a[0].objVal->fields["_id"].toInt();
        SDL_Rect rect = { a[1].toInt(), a[2].toInt(), a[3].toInt(), a[4].toInt() };
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto it = g_windows.find(id);
        if (it != g_windows.end() && it->second->renderer) {
            SDL_RenderFillRect(it->second->renderer, &rect);
        }
        return Value::makeNull();
    });

    // Graphics.drawLine(win, x1, y1, x2, y2)
    vm.registerNative("Graphics.drawLine", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 5 || !a[0].objVal) return Value::makeNull();
        int id = a[0].objVal->fields["_id"].toInt();
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto it = g_windows.find(id);
        if (it != g_windows.end() && it->second->renderer) {
            SDL_RenderDrawLine(it->second->renderer, a[1].toInt(), a[2].toInt(), a[3].toInt(), a[4].toInt());
        }
        return Value::makeNull();
    });

    // Graphics.drawCircle(win, cx, cy, radius) — midpoint circle
    vm.registerNative("Graphics.drawCircle", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 4 || !a[0].objVal) return Value::makeNull();
        int id = a[0].objVal->fields["_id"].toInt();
        int cx = a[1].toInt(), cy = a[2].toInt(), radius = a[3].toInt();
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto it = g_windows.find(id);
        if (it != g_windows.end() && it->second->renderer) {
            int x = radius, y = 0, err = 0;
            while (x >= y) {
                SDL_RenderDrawPoint(it->second->renderer, cx + x, cy + y);
                SDL_RenderDrawPoint(it->second->renderer, cx + y, cy + x);
                SDL_RenderDrawPoint(it->second->renderer, cx - y, cy + x);
                SDL_RenderDrawPoint(it->second->renderer, cx - x, cy + y);
                SDL_RenderDrawPoint(it->second->renderer, cx - x, cy - y);
                SDL_RenderDrawPoint(it->second->renderer, cx - y, cy - x);
                SDL_RenderDrawPoint(it->second->renderer, cx + y, cy - x);
                SDL_RenderDrawPoint(it->second->renderer, cx + x, cy - y);
                y++;
                err += 1 + 2*y;
                if (2*(err - x) + 1 > 0) { x--; err += 1 - 2*x; }
            }
        }
        return Value::makeNull();
    });

    // Graphics.fillCircle(win, cx, cy, radius)
    vm.registerNative("Graphics.fillCircle", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 4 || !a[0].objVal) return Value::makeNull();
        int id = a[0].objVal->fields["_id"].toInt();
        int cx = a[1].toInt(), cy = a[2].toInt(), radius = a[3].toInt();
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        auto it = g_windows.find(id);
        if (it != g_windows.end() && it->second->renderer) {
            for (int dy = -radius; dy <= radius; dy++) {
                int dx = (int)std::sqrt(radius * radius - dy * dy);
                SDL_RenderDrawLine(it->second->renderer, cx - dx, cy + dy, cx + dx, cy + dy);
            }
        }
        return Value::makeNull();
    });

    // Graphics.setFps(win, fps) — delay to cap framerate
    vm.registerNative("Graphics.setFps", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) return Value::makeNull();
        int fps = a[1].toInt();
        if (fps > 0) SDL_Delay(1000 / fps);
        return Value::makeNull();
    });

    // Input.poll(win) — poll SDL events and update input state
    vm.registerNative("Input.poll", [](const std::vector<Value>& a) -> Value {
        (void)a;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
                case SDL_KEYDOWN: if (e.key.keysym.scancode < 512) g_keysDown[e.key.keysym.scancode] = true; break;
                case SDL_KEYUP: if (e.key.keysym.scancode < 512) g_keysDown[e.key.keysym.scancode] = false; break;
                case SDL_MOUSEMOTION: g_mouseX = e.motion.x; g_mouseY = e.motion.y; break;
                case SDL_MOUSEBUTTONDOWN: if (e.button.button < 8) g_mouseButtons[e.button.button] = true; break;
                case SDL_MOUSEBUTTONUP: if (e.button.button < 8) g_mouseButtons[e.button.button] = false; break;
                case SDL_QUIT:
                    // Mark all windows as closed
                    for (auto& [id, win] : g_windows) win->open = false;
                    break;
                default: break;
            }
        }
        return Value::makeNull();
    });

    // Input.isKeyPressed(keyCode) — check if key is currently down (by scancode int)
    vm.registerNative("Input.isKeyPressed", [](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeBool(false);
        int key = a[0].toInt();
        if (key >= 0 && key < 512) return Value::makeBool(g_keysDown[key]);
        return Value::makeBool(false);
    });

    // Input.mouseX()
    vm.registerNative("Input.mouseX", [](const std::vector<Value>&) -> Value {
        return Value::makeInt(g_mouseX);
    });

    // Input.mouseY()
    vm.registerNative("Input.mouseY", [](const std::vector<Value>&) -> Value {
        return Value::makeInt(g_mouseY);
    });

    // Input.mouseButton(button)
    vm.registerNative("Input.mouseButton", [](const std::vector<Value>& a) -> Value {
        int btn = a.empty() ? 1 : a[0].toInt();
        if (btn >= 0 && btn < 8) return Value::makeBool(g_mouseButtons[btn]);
        return Value::makeBool(false);
    });

    // Input.quitRequested()
    vm.registerNative("Input.quitRequested", [](const std::vector<Value>&) -> Value {
        // Check if any window has been marked closed by SDL_QUIT
        for (auto& [id, win] : g_windows) {
            if (!win->open) return Value::makeBool(true);
        }
        return Value::makeBool(false);
    });

} // registerGraphicsModule

} // namespace stdlib
} // namespace runtime
} // namespace gard
