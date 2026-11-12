// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: what the game module (GameHost) and the settings page (UiHost) get from play mode.
#include "play_state.h"

namespace nxrt::play {
using namespace detail;
namespace detail {

// ---- what both hosts share: the rebinding capture (G::capture_state, cap_have, cap_id) and the quit prompt -----------------------------------
// The game's own menus (GameHost) and the settings page (UiHost) rebind through the same one capture: handle_event hands the next input to it.
void capture_begin() {
    G& st = g();
    std::lock_guard<std::mutex> lk(st.cap_mu);
    st.cap_have = false;
    st.capture_state.store(1);
}
bool capture_active() { return g().capture_state.load() == 1; }
bool capture_take(InputId* out) {
    G& st = g();
    std::lock_guard<std::mutex> lk(st.cap_mu);
    if (!st.cap_have) return false;
    st.cap_have = false;
    if (out) *out = st.cap_id;
    return true;
}
void capture_cancel() { g().capture_state.store(0); }
bool key_reserved(const InputId& id) {
    G& st = g();
    return st.overlay && id.kind == InKind::Key && id.code == st.overlay->toggle_scancode();
}
void quit_prompt_request() {
    if (G& st = g(); st.overlay) st.overlay->open_quit_prompt();
}

// ---- what the game module gets from play mode (play_game.h GameHost) ----------------------------------------------------------------------
class GameHostImpl : public GameHost {
public:
    const Settings& settings() const override { return g().settings; }
    bool touch_point(TouchPoint* out) const override {
        const G& st = g();
        if (!st.touch_seen) return false;
        out->u = st.touch_u;
        out->v = st.touch_v;
        out->inside = st.touch_inside;
        out->down = st.touch_down;
        const uint64_t now = now_ns();
        out->age_ns = now > st.touch_ns ? now - st.touch_ns : 0;
        return true;
    }
    void set_finger_pointer(bool on) override { g().finger_pointer = on; }
    bool action_active(Act a, uint64_t now_ns) const override { return g().engine && g().engine->act_active(a, now_ns); }
    uint64_t action_activations(Act a) const override { return g().engine ? g().engine->activations(a) : 0; }
    void right_stick(float* x, float* y) const override {
        *x = *y = 0;
        if (g().source_owner) g().source_owner->right_stick(x, y);
    }
    bool pad_look(bool gyro_enabled) const override { return g().source_owner && g().source_owner->pad_look(gyro_enabled); }
    bool has_input() const override { return g().source_owner != nullptr; }
    void input_tick(double* dx, double* dy) override {
        G& st = g();
        const uint64_t now = now_ns();
        st.last_pi_ns.store(now);
        st.last_pi_frame.store(st.frames.load());
        st.pi_calls++;
        *dx = *dy = 0;
        if (st.update_seq != st.aim_seq) {                // the first input tick since the frame boundary takes the mouse counts
            st.aim_seq = st.update_seq;
            // Late sample: the frame boundary pumped the events at the start of the frame update; the tick that reaches the input function is a little
            // later (the whole update body runs first). Pump again so the counts include whatever the mouse did in between. Main thread only, like the
            // pump in the frame-boundary hook; the key and button events it queues are handled at the next boundary.
            if (st.late_sample && st.window && std::this_thread::get_id() == st.pump_thread) platform::pump_events();
            if (extras::LowLatency* ll = extras::lowlat()) ll->input_sample();
            const input::MouseDelta d = input::mouse_delta_take();
            *dx = d.dx;
            *dy = d.dy;
        }
    }
    void aim_applied(double dx, double dy) override {
        G& st = g();
        st.aim_frames++;
        st.counts += static_cast<uint64_t>(std::fabs(dx) + std::fabs(dy));
    }
    bool log_status() const override { return g().log_status; }
    void request_quit_prompt() override { quit_prompt_request(); }
    void begin_capture() override { capture_begin(); }
    bool capturing() const override { return capture_active(); }
    bool capture_result(InputId* out) override { return capture_take(out); }
    void cancel_capture() override { capture_cancel(); }
    bool reserved_key(const InputId& id) const override { return key_reserved(id); }
};
GameHostImpl g_game_host;

// ---- the settings page host -----------------------------------------------------------------------------------------------------------
class Host : public UiHost {
public:
    Settings settings() const override { G& st = g(); std::lock_guard<std::mutex> lk(st.mu); return st.have_pending ? st.pending : st.settings; }
    Settings factory() const override { return default_settings(); }
    void apply(const Settings& s) override { G& st = g(); std::lock_guard<std::mutex> lk(st.mu); st.pending = s; st.have_pending = true; }
    LiveStatus status() const override {
        G& st = g();
        LiveStatus s;
        s.ctx = st.engine ? st.engine->context() : Ctx::Menu;
        GameStatus gs;
        if (const GamePlay* gp = game_play(); gp && gp->status) gp->status(&gs);
        s.aim = gs.aim;
        s.prompt_now = prompts_effective_name();
        if (st.pi_calls.load() == 0) s.aim = "not in game";
        s.mouse_captured = st.window && platform::relative_mouse();
        input::GamepadInfo infos[4];
        s.pads = static_cast<int>(input::gamepad_count());
        if (input::gamepad_list(infos, 4) > 0) {
            s.pad_name = infos[0].name;
            s.pad_gyro = infos[0].gyro;
            s.pad_rumble = infos[0].rumble;
        }
        if (st.renderer) {
            s.present_mode = gfx::present_mode_name(st.renderer->present_mode());
            const uint64_t now = now_ns();
            const uint64_t frames = st.renderer->stats().frames;
            std::lock_guard<std::mutex> lk(st.st_mu);
            if (now - st.fps_last_ns > 1000 * kMs) {
                if (st.fps_last_ns) st.fps = static_cast<double>(frames - st.fps_last_frames) * 1e9 / static_cast<double>(now - st.fps_last_ns);
                st.fps_last_frames = frames;
                st.fps_last_ns = now;
            }
            s.fps = st.fps;
        }
        if (st.window) {
            s.window_mode = window_mode_name(platform::fullscreen_mode());
            s.refresh_hz = platform::display_refresh_hz();
            DisplaySettings disp;   // copied under the lock: update_hook assigns st.settings on the main thread while this runs on the presenting thread
            {
                std::lock_guard<std::mutex> lk(st.mu);
                disp = st.settings.display;
            }
            if (disp.vsync && disp.fps_cap > 0) {
                double sc = 0;
                const char* why = "";
                s.snap_k = frame_snap_k(disp, disp.fps_cap, s.refresh_hz, &sc, &why);
                s.snap_cap = sc;
            }
            platform::pixel_size(&s.width, &s.height);
        }
        s.out = st.engine->output(now_ns());
        s.pitch_deg = gs.pitch_rad * 180.0 / kPi;
        s.input_calls = st.pi_calls.load();
        return s;
    }
    std::string settings_path() const override { return g().path; }
    void begin_capture() override { capture_begin(); }
    bool capturing() const override { return capture_active(); }
    bool capture_result(InputId* out) override { return capture_take(out); }
    void cancel_capture() override { capture_cancel(); }
    bool reserved_key(const InputId& id) const override { return key_reserved(id); }
    void request_quit_prompt() override { quit_prompt_request(); }
};
Host g_host;

void toggle_fps_counter() {
    Settings s = g_host.settings();
    s.fps.show = !s.fps.show;
    g_host.apply(s);
}

GameHost& game_host() { return g_game_host; }
UiHost& ui_host() { return g_host; }

}  // namespace detail
}  // namespace nxrt::play
