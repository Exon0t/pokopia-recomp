// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode's half of the game boundary (docs/GAME_BOUNDARY.md, nxrt/game.h Info::play).
//
// Play mode itself is the game-independent PC front end: the window, the SDL event pump, the frame limiter, the binding engine that turns
// keys, mouse buttons and pads into Switch pad input, the menu pointer state, the F1 overlay and its settings file. What only makes sense for
// one game goes through GamePlay: the hooks on that game's functions (raw mouse aim, camera look, lock-on, display options, menu widgets, the
// tone curve), what the game's state says about the context and its parts of the F1 page. A game without a
// GamePlay table still gets a window, pad input, the frame limiter and the overlay; the context is then always the menu.
//
// Order at start (play.cpp start()): install_render (before the frame-boundary hook), the frame-boundary hook at frame_update, install.
// Order at every frame boundary (main thread): settings_changed (when the page applied new settings), events, pads, frame_begin, the engine's
// tick, frame (the context), then the menu pointer (menu_frame).
#pragma once
#include <cstddef>
#include <cstdint>

#include "nxrt/overlay_canvas.h"

#include "menu_mouse.h"
#include "play_display.h"
#include "play_types.h"

namespace nxrt::play {

// A finger on the touch screen, for a game's touch aiming: SDL turns a touch into mouse events marked input::kFlagTouch (touch_route.h), and
// play mode keeps the last one. u, v: where it touched the game picture, 0..1 (v down), clamped; inside: it was on the picture (not a bar);
// down: a finger is on the screen now; age_ns: since the last touch event (a press, a move or the lift).
struct TouchPoint {
    float u = 0, v = 0;
    bool inside = false;
    bool down = false;
    uint64_t age_ns = 0;
};

// What play mode offers the game module. Main thread (the thread the game's frame update and input hooks run on) unless noted.
class GameHost {
public:
    virtual ~GameHost() = default;
    virtual const Settings& settings() const = 0;                 // the settings in force
    virtual bool touch_point(TouchPoint*) const { return false; }   // the last touch on the screen; false until a finger has touched it
    // The free pointer's motion and left button count as the finger too (touch_point, and no left-click action in play), and play mode
    // does not capture the pointer for the camera: for touch screens the window gets as a plain mouse (SteamOS game mode: gamescope turns a
    // touch into pointer events, so SDL marks nothing as touch).
    virtual void set_finger_pointer(bool) {}
    virtual bool action_active(Act a, uint64_t now_ns) const = 0;  // a binding of the action is held
    // The right stick as the game gets it (dead zone, invert and sensitivity applied), -1..1, + = right and up.
    // Activation edges of the action since start (a binding of it became active: a press, a tap, a wheel notch). A game-side toggle counts these.
    virtual uint64_t action_activations(Act a) const = 0;
    virtual void right_stick(float* x, float* y) const = 0;
    // A pad is looking up or down right now (the right stick's vertical axis off centre, or the gyro turning about the pad's pitch axis).
    virtual bool pad_look(bool gyro_enabled) const = 0;
    virtual bool has_input() const = 0;               // play mode's input source is up (pad_look and the game's own look checks apply only then)
    // The game's per-tick player-input function was entered (the game is in play: the context detection and Stats count it). Returns the mouse
    // counts gathered since the last frame boundary on the first call after it (pumping the window's events once more first), zero after that.
    virtual void input_tick(double* dx, double* dy) = 0;
    virtual void aim_applied(double dx, double dy) = 0;           // Stats: the aim used these counts
    virtual bool log_status() const = 0;                          // [play] log_status
    virtual void request_quit_prompt() = 0;                       // opens the "Close the game?" prompt (overlay.h open_quit_prompt)
    // Rebinding from a game's own menu, the same capture as the settings page (play_ui.h UiHost): after begin_capture() the next key press,
    // mouse button or wheel notch does not reach the game; capture_result() returns it once. Escape cancels (capturing() turns false).
    virtual void begin_capture() {}
    virtual bool capturing() const { return false; }
    virtual bool capture_result(InputId*) { return false; }
    virtual void cancel_capture() {}
    virtual bool reserved_key(const InputId&) const { return false; }   // the settings page's own key (F1): never a binding
};

// The game's answer at one frame boundary.
struct GameFrame {
    float look_scale = 1.f;          // the gyro's pitch rate is multiplied by this before the game reads it
    Ctx play_ctx = Ctx::Play;        // the context while the game is in play
};

struct GameStatus {
    const char* aim = "";            // why the mouse aim does or does not apply now (settings page)
    double pitch_rad = 0;            // the view pitch the aim holds
};

// The display options only a game's own hooks can honour (the F1 settings page lists them only when the game says it has them). Dynamic
// resolution, the render size, anisotropy and LOD bias work for every game and are always listed.
enum : unsigned {
    kDisplayOptFov = 1u << 0,        // [display] fov_offset
    kDisplayOptAspect = 1u << 1,     // [display] aspect
    kDisplayOptBloom = 1u << 2,      // [display] bloom
    kDisplayOptChromatic = 1u << 3,  // [display] chromatic_aberration
    kDisplayOptAll = kDisplayOptFov | kDisplayOptAspect | kDisplayOptBloom | kDisplayOptChromatic,
    kDisplayOptQuality = 1u << 4,    // [display] quality_preset (the game's quality hooks; GamePlay::quality_preset)
};

struct GamePlay {
    unsigned display_options = 0;    // kDisplayOpt* bits: which of the options above the game's display hooks apply (0: none)
    uint64_t frame_update = 0;       // entry hook site (offset in main): once per rendered frame, on the main thread; 0 = play mode is never pumped
    // Before the frame-boundary hook is added: hooks that do not need the host (display options, the tone curve). window_w/h: the client size.
    void (*install_render)(uint64_t base, const DisplaySettings& d, int window_w, int window_h) = nullptr;
    // After it: input, camera, lock-on, menu widgets.
    void (*install)(uint64_t base, GameHost* host, const Settings& s) = nullptr;
    void (*remove)() = nullptr;
    void (*settings_changed)(const Settings& s) = nullptr;       // new settings are in force
    void (*frame_begin)() = nullptr;                             // after the pads are polled
    void (*frame)(bool in_game, double step_seconds, GameFrame* out) = nullptr;
    DisplayLive (*display_live)() = nullptr;                     // what the display hooks use (the menu pointer's geometry)
    uint32_t (*display_hook_calls)() = nullptr;
    // The menu pointer on the game's own menus (menu_mouse.h, the game side): once per frame, the button the click stands for.
    Act (*menu_frame)(bool menu_up, const mm::Geometry& g) = nullptr;
    bool (*menu_over_value)() = nullptr;                          // the pointer is over a slider's track: the wheel steps its value
    bool (*menu_active)() = nullptr;                              // the menu hooks saw an active pointer this frame
    // Once per frame before the context is decided: true while one of the game's menus is up although the game is in play (it keeps calling
    // its input function under an in-game menu). Play mode then treats the frame as a menu: menu context, the pointer, no mouse capture or look.
    bool (*menu_in_game)(bool in_game) = nullptr;
    void (*status)(GameStatus* out) = nullptr;
    // The game's part of the [play] log_status line, appended after the base fields.
    void (*status_text)(char* buf, size_t n) = nullptr;
    // F1 page, Mouse tab: where = 0 after the sensitivity sliders, 1 after the invert boxes, 2 at the end. Returns true when edit changed.
    bool (*mouse_tab)(int where, Settings& edit) = nullptr;
    // F1 page, Graphics tab (kDisplayOptQuality): writes a quality preset's values into s (the ids of quality_presets(), play_types.h: the display
    // options and the anti-aliasing; a device preset also writes render_scale and fps_cap). False: unknown name.
    bool (*quality_preset)(const char* name, Settings& s) = nullptr;
    // A layer over the game's picture (nxrt/overlay_canvas.h: debug views). The overlay's presenting thread, not the main thread: active() once per
    // presented frame, draw() when it said true. Null: nothing drawn.
    gfx::GameLayer overlay_layer;
};

const GamePlay* game_play();         // the registered game's table, or nullptr

// For settings shown in the game's own menus. Thread safe; the same path as the F1 page (applied at the next frame, written to the file).
Settings current_settings();
void apply_settings(const Settings& s);
uint64_t game_menu_edits();          // how many times apply_settings ran: the F1 page reloads its working copy when this moves

}  // namespace nxrt::play
