// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, the PC features of play mode (pkp_play.h): native render resolution, frame rates above 60 at the right game speed and the
// raw mouse camera. All addresses are offsets in `main` of the 2.0.1 update (build 76a489f3...), found in IDA (work/re/pc_features.md);
// the sites are in recomp/games/pkp/main.sites.
//
// Render resolution. KTGL's dynamic-resolution manager (the object at [0x35618d0]) sizes everything from a level table of 34 sizes
// (0x2849658 widths, 0x28496e0 heights: 960x540 + 32x18 per level, then 2560x1440, 3840x2160, 7680x4320), read only through two getters
// (0x1129990 width, 0x11299d0 height). At start it is set up with level 30 (1920x1080) as the target size (the swap chain, the scene and UI
// targets: manager +16/+20, copied to +72/+76 which the window set-up and every pass read) and as the largest scene size (+24/+28). Each frame
// it picks a lower level when the GPU is late (docked tables top out at level 30) and scales by a quality factor, giving the scene's viewport
// (+32/+36). The hooks: the two getters answer the configured size for level 30 and scale the levels below it in proportion (so the
// game's own dynamic resolution still steps down smoothly from the new top), the fallbacks 0x1129980 / 0x11299c0 (1920 / 1080 while the
// manager does not exist yet) answer the same size, and the per-frame copy of the parameters (0x22d1e0) gets the full size as the viewport
// while [display] dynamic_resolution is off. The scene passes keep viewports authored for 1920x1080: the command context's SetViewport(s)
// (0x17b509c / 0x17b5240, which also derive the 2/w, -2/h screen constants from the rect) get the target's size, and the render thread's
// replay (0x17b6c98) stretches what still arrives sized for 1920x1080. The internal size is fixed at start (the targets are created once).
// The terrain occlusion pass's copy into its fixed 80x45 targets takes its source rect from a viewport recorded as 1920x1080 (hook_low_copy
// below; without it a hard-edged dark patch showed on the ground). [pkp] render_override (default 1 since the
// 2026-11-02 run confirmed that fix; 0 = the game's 1080p layout) switches all of it on.
//
// Frame rate. The main loop's frame step (0xbfc404) turns the measured frame time into a frame multiplier in 60 Hz units that 90 functions
// read (the float at 0x34c03f0): 1.0 within 1.5 ms of 1/60 s, else dt * 60 clamped to [1, 3]. Faster than 60 fps the multiplier stays 1
// and the game runs fast. The mid hook at the join point (0xbfc4fc: s0 the multiplier, s8 the frame time) lets frames shorter than 1/60 s
// count as dt * 60 (down to 1/8). At 60 fps and below nothing changes.
// The 60 fps cap is the game's own: the main loop's frame step (0x89def8, profiler zone "Sync") waits, sleeping in 200 us task-graph waits,
// until tick frequency / 60 ticks (60 = 60 / 0x13fc00(), a constant 1.0) have passed since the previous frame began. The mid hook right
// after that division (0x89e27c, x22 = the period in ticks) sets the period to 0, so the runtime's frame limiter at the frame boundary
// ([display] fps_limit) alone paces the game ([pkp] game_frame_limit = 1 keeps the game's 60 Hz wait). In the frame step's fixed-step mode
// (byte +0xb4 of the main-loop object, copied to sp+0xc) the frame time is 1 / 60 whatever the real time; the mid hook before that division
// (0x89e398, s0 = 60.0, x28 now, x25 the previous frame's tick, d8 the tick frequency) puts the measured rate in s0 for frames shorter
// than 1/60 s, the same rule as the multiplier.
//
// Mouse camera. The follow camera's update (0x87840, vtable slot of the class at 0x322cd58) turns the right stick (pad state +0x278 / +0x27c)
// into its yaw (+0xa4, radians, 0..2pi, stick right lowers it) and pitch (+0xa0, clamped to [+0xd8, +0xd4]), with the twin angles +0x128
// / +0x124 when the strafe style option is on, and marks manual input (+0xff = 1, +0x13c = 0: the auto-adjust delay). The mid hook right
// after it read the stick (0x87d30, inside the branch that runs only while the player may turn the camera) adds the mouse counts the same
// way: exact radians per count from [mouse] yaw / pitch, independent of the game's camera speed.
// What the game does with the angles (IDA): each frame runs the camera manager's update (0x71000b80c0 -> the update above), then
// 0x7100249780, then the view write (0x7100083b40 -> 0x7100088aac copies the eye +0x60 / target +0x50 into the view block). There is no
// easing on yaw or pitch: the follow placement 0x7100078440 (via 0x71000783c0) builds the eye straight from them (0x710007bef0: target +
// R(pitch, yaw) * distance), pushes it in for collisions and writes the angles back from the result; its recentre helpers (0x710007c0c0,
// 0x710007d720) stay off while +0xff (manual) is set. But 0x7100249780 runs that placement only when its tick (0x7100249960) fires, and the
// tick fires when the summed frame time passes 1/120 s, then restarts from 0: above 120 fps the eye moved every second or third frame (at
// ~185 fps about 92 times a second, unevenly) while the angles moved every frame, and the placement's own time steps (it gets the frame
// time, not the summed one) ran slow. The mid hook after that tick (0x7100249848, w0 bit 0 = run the placement) runs the placement every
// frame ([pkp] camera_every_frame, default 1); the tick's other work (collision lists) keeps its own rate.
// The turn branch (0x87d30) is closed whenever the game's own stick is ignored (+0x111 / +0x112 flags: player state 0x88, scene conditions;
// 0x7100300a6c; 0x71000887f0). It was also the only place that told play mode the game is in play, and play mode trusts that for 4 frames
// (21 ms at 185 fps): a short closure dropped the context to the menu (cursor released and shown, counts discarded). The mid hook at 0x87cd0
// (every follow-camera update past its scene check, before the gate) keeps reading the mouse for [pkp] camera_grace_ms (default 250) after
// the branch last ran: counts read while the branch is closed are applied when it opens again within that time (the view ends where the
// hand moved it) and dropped after it (a scripted camera does not jump when control comes back).
#include "pkp_play.h"
#include "pkp_kbm.h"
#include "pkp_movie_test.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/extras.h"
#include "nxrt/gfx_tuning.h"
#include "nxrt/hooks.h"
#include "nxrt/host_device.h"
#include "nxrt/jc2_mouse.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "play_display.h"

namespace nxrt::games::pkp {

namespace {

constexpr const char* kTag = "pkp";

// ---- offsets in main ----
constexpr uint64_t kLevelWidth = 0x1129990, kLevelHeight = 0x11299d0;       // u32 (u8 level)
constexpr uint64_t kFullWidth = 0x1129980, kFullHeight = 0x11299c0;         // u32 (): 1920 / 1080
constexpr uint64_t kCopyParams = 0x22d1e0;                                  // dynres manager: copy the frame's size parameters (x0 = manager)
constexpr uint64_t kReplayViewports = 0x17b6c98;                            // mid: render-thread pass replay, before SetViewports
constexpr uint64_t kSetViewport = 0x17b50ac, kSetViewports = 0x17b5254;     // mid: the command context's SetViewport (x1 rect) / SetViewports (w1 n, x2 rects)
constexpr uint64_t kRecordViewports = 0x17c0420;                            // entry: records viewports into the pass state (x0 ctx + 0x10, w1 n, x2 rects)
constexpr uint64_t kLowCopy = 0x73d824;                                    // mid: 0x73d73c calling the 80x45 copy 0x10f48d8 (x3 source texture, x5 source rect)
constexpr uint64_t kFrameMultiplier = 0xbfc4fc;                             // mid: s0 multiplier, s8 frame time
constexpr uint64_t kSyncPeriod = 0x89e27c;                                  // mid: x22 the Sync wait's frame period in ticks (frequency / 60)
constexpr uint64_t kFixedStepRate = 0x89e398;                               // mid: s0 60.0, x28 now, x25 previous frame tick, d8 tick frequency
constexpr uint64_t kCamStick = 0x87d30;                                     // mid: x19 camera, s0 right stick x
constexpr uint64_t kCamField = 0x87cd0;                                     // mid: x19 camera, the update past its scene check, before the turn gate
constexpr uint64_t kFollowGate = 0x249848;                                  // mid: w0 bit 0 = run the follow placement (0x783c0) this frame
constexpr uint64_t kViewsGlobal = 0x3561880;                                // object whose byte +562 says two views (split screen)
constexpr uint64_t kSaveGlobal = 0x3561930;
constexpr uint64_t kTagCallback = 0x1f90fec;                                // mid: text layout, BLR to the tag handler (w1 type, x2 args, x0 layout)                                 // [ptr] the save/options block
constexpr int kFullLevel = 30;
constexpr int kTableW = 1920, kTableH = 1080;

// dynres manager layout: per view (stride 28) +16 target w/h, +24 largest scene w/h, +32 scene w/h, +40 levels (target, largest, current)
constexpr size_t kViewStride = 28, kMaxW = 24, kMaxH = 28, kCurW = 32, kCurH = 36, kLvlMax = 41, kLvlCur = 42;

// follow camera layout
constexpr size_t kCamPlayer = 8, kCamPitch = 0xa0, kCamYaw = 0xa4, kCamPitchMax = 0xd4, kCamPitchMin = 0xd8, kCamManual = 0xff,
                 kCamTwinPitch = 0x124, kCamTwinYaw = 0x128, kCamIdle = 0x13c;
constexpr double kTwoPi = 6.283185307179586;
constexpr float kHalfPi = 1.5707964f;

struct State {
    uint64_t base = 0x7100000000ull;
    play::GameHost* host = nullptr;
    std::atomic<int> w{kTableW}, h{kTableH};
    std::atomic<bool> dynres{false};
    std::atomic<bool> fps_fix{true};
    std::atomic<bool> game_limit{false};
    std::atomic<uint64_t> sync_calls{0}, fixed_calls{0};
    std::atomic<bool> mouse_cam{true};
    std::atomic<bool> cam_every_frame{true};
    uint64_t grace_ns = 250000000ull;
    // main thread (the follow camera's update): counts read before the turn branch and not applied yet
    double pend_dx = 0, pend_dy = 0;
    uint64_t last_turn_ns = 0;
    std::atomic<uint64_t> cam_field{0}, follow_forced{0}, cam_dropped{0};
    std::vector<uint32_t> hooks;
    std::atomic<uint64_t> res_calls{0}, copy_calls{0}, vp_fixes{0}, fps_calls{0}, cam_calls{0}, cam_moves{0};
    std::atomic<uint64_t> vp_shapes[16]{};   // distinct scissor sizes the viewport stretch has touched (logged once each)
    std::atomic<uint32_t> vp_shape_n{0};
    std::atomic<float> last_mult{1.f};
    // Joy-Con 2 mouse mode (frame boundary, main thread): the Mouse mode key flips `mm_want`; the activation count tells how often it fired
    uint64_t mm_seen = 0;
    bool mm_want = false;
    uint64_t mm_log_ns = 0;
    std::atomic<uint64_t> mm_dropped{0};
    bool render_installed = false, installed = false;
};

State& st() {
    static State s;
    return s;
}

template <class T> T rd(uint64_t a) { T v; std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof(T)); return v; }
template <class T> void wr(uint64_t a, T v) { std::memcpy(reinterpret_cast<void*>(a), &v, sizeof(T)); }

// ---- render resolution ----
uint32_t level_size(int level, bool width) {
    const int full = width ? st().w.load(std::memory_order_relaxed) : st().h.load(std::memory_order_relaxed);
    if (level == kFullLevel) return static_cast<uint32_t>(full);
    // the game's own table: levels 0..29 are 960x540 + 32x18 per level (scaled with the new top), 31..33 are 1440p, 4K, 8K (kept)
    static constexpr int kHighW[3] = {2560, 3840, 7680}, kHighH[3] = {1440, 2160, 4320};
    if (level > 33) return static_cast<uint32_t>(full);              // the game's own out-of-range answer is level 30's
    if (level > kFullLevel) return static_cast<uint32_t>(width ? kHighW[level - 31] : kHighH[level - 31]);
    const int table = width ? 960 + 32 * level : 540 + 18 * level;
    const double ref = width ? kTableW : kTableH;
    return static_cast<uint32_t>(std::lround(table * full / ref)) & ~1u;
}

int hook_level_w(::Ctx* c, uint64_t, void*) {
    c->x[0] = level_size(static_cast<int>(c->x[0] & 0xff), true);
    st().res_calls.fetch_add(1, std::memory_order_relaxed);
    return 1;
}
int hook_level_h(::Ctx* c, uint64_t, void*) {
    c->x[0] = level_size(static_cast<int>(c->x[0] & 0xff), false);
    st().res_calls.fetch_add(1, std::memory_order_relaxed);
    return 1;
}
int hook_full_w(::Ctx* c, uint64_t, void*) { c->x[0] = static_cast<uint64_t>(st().w.load()); return 1; }
int hook_full_h(::Ctx* c, uint64_t, void*) { c->x[0] = static_cast<uint64_t>(st().h.load()); return 1; }

int hook_copy_params(::Ctx* c, uint64_t, void*) {
    State& s = st();
    if (s.dynres.load(std::memory_order_relaxed)) return 0;
    const uint64_t mgr = c->x[0];
    if (!mgr || !mem::is_mapped(mgr, 16 + 2 * kViewStride)) return 0;
    int views = 1;
    const uint64_t vo = rd<uint64_t>(s.base + kViewsGlobal);
    if (vo && mem::is_mapped(vo + 562, 1) && rd<uint8_t>(vo + 562)) views = 2;
    for (int i = 0; i < views; ++i) {
        const uint64_t v = mgr + i * kViewStride;
        wr<uint32_t>(v + kCurW, rd<uint32_t>(v + kMaxW));
        wr<uint32_t>(v + kCurH, rd<uint32_t>(v + kMaxH));
        wr<uint8_t>(v + kLvlCur, rd<uint8_t>(v + kLvlMax));
    }
    s.copy_calls.fetch_add(1, std::memory_order_relaxed);
    return 0;                                       // the game's copy runs with the full size in place
}

// The scene passes store their viewports as authored for 1920x1080 (the render thread replays them; the targets themselves follow the new
// size). Right before the replay hands them to nvnCommandBufferSetViewports (sp+216: count x {x, y, w, h} floats; sp+24: the scissors as ints,
// which default to the target's size): a viewport at the origin whose size is the target's size scaled by 1920x1080 / the internal size is
// stretched to the whole target. Smaller fractional targets (half size and so on) get the same treatment; anything else is left alone.
// The command context's SetViewport(s) (0x17b509c / 0x17b5240) record the pass's viewport and, from its size, the screen constants the
// shaders read (2/w, -2/h in vs ubo0). Rects are {w, h, x, y, min, max} (ints, then floats; stride 24). The pass state in use
// (ctx [+0x18] + 616 * ctx [+0x24]) holds the bound target at +0x50, its size at +0x58 / +0x5c. A rect at the origin sized for a
// 1920x1080 frame on a target of the render size is made the target's size here, so the constants follow it too.
// The other viewport setter, 0x17c0420 (64 callers: the G-buffer, the deferred passes and most scene passes; x0 = ctx + 0x10, w1 count,
// x2 rects), only records the rects into the pass state (+0x60 viewports, +0x58 scissors). The screen constants are derived later from
// that record (0x17b3000, 0x17b45c0, 0x17b9700, 0x17bac00: 2 / pass[+0x60].w, -2 / h), so a rect kept at 1920x1080 there gave the
// render-size passes 2/1920, -2/1080 while the replay stretched the viewport itself: a quad in pixel units (vs 821538a4) then covered
// 1.333x the target from the top-left, and its UVs with it. At 2560x1440 the G-buffer AO composite (fs 90107dd3, t428 into the
// albedo's alpha) showed the AO of the characters as dark blue silhouettes at 1.333x their screen position and moved the ground's
// AO the same way (the mottled, world-fixed haze of upstream #17). The entry hook there fixes the rects the same way, before they are
// recorded, so the constants, the replayed viewport and the terrain occlusion copy's source rect all follow.
std::atomic<uint64_t> g_set_vp_fixes{0}, g_rec_vp_fixes{0};
void fix_rect(::Ctx* c, uint64_t rect, uint64_t ctx = 0) {
    State& s = st();
    const int W = s.w.load(std::memory_order_relaxed), H = s.h.load(std::memory_order_relaxed);
    if (!ctx) ctx = c->x[0];
    const int32_t idx = rd<int32_t>(ctx + 0x24);
    const uint64_t pass = idx < 0 ? 0 : rd<uint64_t>(ctx + 0x18) + 616ull * static_cast<uint32_t>(idx);
    if (!pass) return;
    const uint64_t tgt = rd<uint64_t>(pass + 0x50);
    if (!tgt || !mem::is_mapped(tgt + 0x58, 8)) return;
    const int32_t tw = rd<int32_t>(tgt + 0x58), th = rd<int32_t>(tgt + 0x5c);
    int32_t* r = reinterpret_cast<int32_t*>(rect);
    if (r[2] != 0 || r[3] != 0 || tw <= 0 || th <= 0 || (r[0] == tw && r[1] == th)) return;
    const double ew = static_cast<double>(tw) * kTableW / W, eh = static_cast<double>(th) * kTableH / H;
    if (std::fabs(r[0] - ew) > 1.0 || std::fabs(r[1] - eh) > 1.0) return;
    r[0] = tw;
    r[1] = th;
    g_set_vp_fixes.fetch_add(1, std::memory_order_relaxed);
}
int hook_set_viewport(::Ctx* c, uint64_t, void*) {
    const State& s = st();
    if (s.w.load(std::memory_order_relaxed) == kTableW && s.h.load(std::memory_order_relaxed) == kTableH) return 0;
    fix_rect(c, c->x[1]);
    return 0;
}
int hook_set_viewports(::Ctx* c, uint64_t, void*) {
    const State& s = st();
    if (s.w.load(std::memory_order_relaxed) == kTableW && s.h.load(std::memory_order_relaxed) == kTableH) return 0;
    const uint32_t n = static_cast<uint32_t>(c->x[1]);
    for (uint32_t i = 0; i < n && i < 16; ++i) fix_rect(c, c->x[2] + 24ull * i);
    return 0;
}
int hook_record_viewports(::Ctx* c, uint64_t, void*) {
    const State& s = st();
    if (s.w.load(std::memory_order_relaxed) == kTableW && s.h.load(std::memory_order_relaxed) == kTableH) return 0;
    const uint64_t ctx = c->x[0] - 0x10, rects = c->x[2];
    const uint32_t n = static_cast<uint32_t>(c->x[1]);
    if (!rects || n == 0 || !mem::is_mapped(ctx + 0x18, 16) || !mem::is_mapped(rects, 24ull * std::min<uint32_t>(n, 16))) return 0;
    const uint64_t before = g_set_vp_fixes.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < n && i < 16; ++i) fix_rect(c, rects + 24ull * i, ctx);
    if (g_set_vp_fixes.load(std::memory_order_relaxed) != before && g_rec_vp_fixes.fetch_add(1, std::memory_order_relaxed) == 0)
        NXLOG_I(kTag, "recorded viewport: 1920x1080-sized rect made the target's size (0x17c0420, screen constants follow)");
    return 0;
}

int hook_replay_viewports(::Ctx* c, uint64_t, void*) {
    State& s = st();
    const int W = s.w.load(std::memory_order_relaxed), H = s.h.load(std::memory_order_relaxed);
    if (W == kTableW && H == kTableH) return 0;
    const uint32_t n = static_cast<uint32_t>(c->x[23]);
    if (n == 0 || n > 16) return 0;
    const uint64_t sp = c->x[31];
    for (uint32_t i = 0; i < n; ++i) {
        float* vp = reinterpret_cast<float*>(sp + 216 + 16 * i);
        const int32_t* sc = reinterpret_cast<const int32_t*>(sp + 24 + 16 * i);
        if (vp[0] != 0.f || vp[1] != 0.f || sc[0] != 0 || sc[1] != 0 || sc[2] <= 0 || sc[3] <= 0) continue;
        const double ew = static_cast<double>(sc[2]) * kTableW / W, eh = static_cast<double>(sc[3]) * kTableH / H;
        if (std::fabs(vp[2] - ew) <= 1.0 && std::fabs(vp[3] - eh) <= 1.0) {
            const uint64_t key = (static_cast<uint64_t>(sc[2]) << 32) | static_cast<uint32_t>(sc[3]);
            bool seen = false;
            const uint32_t nk = std::min<uint32_t>(s.vp_shape_n.load(std::memory_order_relaxed), 16);
            for (uint32_t k = 0; k < nk && !seen; ++k) seen = s.vp_shapes[k].load(std::memory_order_relaxed) == key;
            if (!seen) {
                const uint32_t slot = s.vp_shape_n.fetch_add(1, std::memory_order_relaxed);
                if (slot < 16) {
                    s.vp_shapes[slot].store(key, std::memory_order_relaxed);
                    NXLOG_I(kTag, "viewport stretch: %.0fx%.0f -> %dx%d (viewport %u of %u)", vp[2], vp[3], sc[2], sc[3], i, n);
                }
            }
            vp[2] = static_cast<float>(sc[2]);
            vp[3] = static_cast<float>(sc[3]);
            s.vp_fixes.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return 0;
}

// The terrain occlusion pass's 80x45 copy. At BeginScene_GBuffer, 0x73d73c copies the G-buffer colour and depth into the fixed 80x45 targets
// (manager +0x40f0 / +0x40f8) that the terrain lighting (fs 37c9671c, t282.w) reads at screen UV. Through 0x10f48d8(dst, dst depth, dst
// rect, src colour, src depth, src rect {x, y, w, h}), it gives the source rect as the current pass's recorded viewport (pass +96) times the
// view's scale. 0x10edc40 records the G-buffer viewport through the light SetViewports 0x17c0420 as 1920 x 1080, and nothing changes that copy,
// so on render-size G-buffer targets the copy samples UV 0..1920/W (1.5 at 1280x720, nvn.trace: a2[0..1.5]). The visible frame then lands in
// the top-left of the 80x45 targets and the rest is the sampler's border; the terrain drawn on top is at the right place. The lighting
// reads that mix, which puts a hard-edged dark patch on the ground. The mid hook on the call (0x73d824: x3 the source colour texture, u16
// width / height at +48 / +50; x5 the source rect) turns a rect at the origin sized for 1920x1080 on a render-size source into the source's size.
std::atomic<uint64_t> g_lowcopy_fixes{0};
int hook_low_copy(::Ctx* c, uint64_t, void*) {
    const State& s = st();
    const int W = s.w.load(std::memory_order_relaxed), H = s.h.load(std::memory_order_relaxed);
    if (W == kTableW && H == kTableH) return 0;
    const uint64_t src = c->x[3], rect = c->x[5];
    if (!src || !rect || !mem::is_mapped(src + 48, 4)) return 0;
    const int tw = rd<uint16_t>(src + 48), th = rd<uint16_t>(src + 50);
    int32_t* r = reinterpret_cast<int32_t*>(rect);                   // {x, y, w, h}
    if (r[0] != 0 || r[1] != 0 || tw <= 0 || th <= 0 || (r[2] == tw && r[3] == th)) return 0;
    const double ew = static_cast<double>(tw) * kTableW / W, eh = static_cast<double>(th) * kTableH / H;
    if (std::fabs(r[2] - ew) > 1.0 || std::fabs(r[3] - eh) > 1.0) return 0;
    if (g_lowcopy_fixes.fetch_add(1, std::memory_order_relaxed) == 0)
        NXLOG_I(kTag, "terrain occlusion copy: source rect %dx%d -> %dx%d", r[2], r[3], tw, th);
    r[2] = tw;
    r[3] = th;
    return 0;
}

// ---- aspect ratio (ultrawide, 16:10) ----
// KTGL's camera class (init 0x1f12368: +16 eye, +32 target, +48 up, +320 near, +324 far, +328 vertical FOV in radians, +332 aspect, +340 zoom,
// +344 bit 0 orthographic; +128 view rows, +192 projection rows, +256 view-projection rows = P0 * V.x + P1 * V.y + P2 * V.z + P3 * V.w per view
// row) builds its projection in 0x1f12114 from the FOV and the aspect: P[0][0] = cot(fov / 2) / aspect, P[1][1] = cot(fov / 2). Every game
// camera is created with the aspect 1.7778 written as a constant. The scene's cameras (the camera manager's block [0x3483e18] + 18864 + 608 * n,
// or the view's own camera) reach the renderer through 0x8cbe64, which hands the camera's view-projection to the render view
// (0x18d3868(view slot, camera + 256, ...)): the mid hook there (0x8cc124, x20 = the camera) widens a 16:9 camera to the display's aspect.
// Wider than 16:9 (Hor+): the vertical FOV stays, P[0][0] = P[1][1] / aspect and +332 = aspect, so the game's own later rebuilds stay wide
// (and the mid hook in the builder, 0x1f12130 with x0 = the camera, puts the aspect back into a known scene camera whenever the game resets it)
// and everything that culls from the camera sees the wider frustum. Narrower than 16:9: the horizontal reach stays and the view grows above and
// below (P[1][1] = P[0][0] * aspect; +332 is left alone, so this one is matrix-only).
// The UI's screen-layout camera (0x1fbab60(out, eye, int size[2]): an orthographic camera whose P[0][0] / P[1][1] are 2 / w, 2 / h of the size,
// origin at the canvas centre) gets a size of the display's shape when it is asked for the 1920x1080 canvas (mid hook 0x1fbab98, x2 the size):
// the 16:9 layouts stay 16:9, centred over the wider (or taller) scene.
constexpr uint64_t kCamSubmit = 0x8cc124;   // mid: x20 = the camera whose view-projection goes to the render view
constexpr uint64_t kProjBuild = 0x1f12130;  // mid: x0 = the camera, the projection builder after its prologue
constexpr uint64_t kUiCamera = 0x1fbab98;   // mid: x2 = int[2] screen size of the layout camera
constexpr size_t kCamFov = 328, kCamAspect = 332, kCamView = 128, kCamProj = 192, kCamViewProj = 256;
constexpr double kAspect169 = 16.0 / 9.0;

struct Wide {
    double aspect = kAspect169;               // the display's aspect (16:9 = nothing to do)
    std::atomic<uint64_t> cams[8]{};          // scene cameras seen at the render-view hand-over
    std::atomic<uint64_t> cam_fixes{0}, aspect_puts{0}, ui_fixes{0};
    std::atomic<bool> fov_logged{false};
    uint64_t ui_size = 0;                     // guest int[2] the layout camera reads instead of 1920x1080
};
Wide& wide() {
    static Wide w;
    return w;
}
bool close_to(double a, double b) { return std::fabs(a - b) < 0.02; }
bool wide_on() { return !close_to(wide().aspect, kAspect169); }

bool known_cam(uint64_t cam) {
    for (auto& c : wide().cams)
        if (c.load(std::memory_order_relaxed) == cam) return true;
    return false;
}
void remember_cam(uint64_t cam) {
    if (known_cam(cam)) return;
    for (auto& c : wide().cams) {
        uint64_t z = 0;
        if (c.compare_exchange_strong(z, cam, std::memory_order_relaxed)) return;
    }
}

// Widens (or heightens) one camera's projection and view-projection from 16:9 to the display's aspect. Idempotent: a camera already at the
// display's shape, or at any shape other than 16:9, is left alone.
void fix_camera(uint64_t cam) {
    Wide& w = wide();
    float* P = reinterpret_cast<float*>(cam + kCamProj);
    const float p00 = P[0], p11 = P[5];
    if (!(std::fabs(p00) > 1e-6f) || !(std::fabs(p11) > 1e-6f)) return;
    const double own = std::fabs(p11 / p00);
    if (close_to(own, w.aspect) || !close_to(own, kAspect169)) return;
    const bool wider = w.aspect > kAspect169;
    const int lane = wider ? 0 : 1;
    if (wider) P[0] = std::copysign(static_cast<float>(std::fabs(p11) / w.aspect), p00);
    else P[5] = std::copysign(static_cast<float>(std::fabs(p00) * w.aspect), p11);
    const float* V = reinterpret_cast<const float*>(cam + kCamView);
    float* VP = reinterpret_cast<float*>(cam + kCamViewProj);
    for (int i = 0; i < 4; ++i) {
        float sum = 0;
        for (int k = 0; k < 4; ++k) sum += P[4 * k + lane] * V[4 * i + k];
        VP[4 * i + lane] = sum;
    }
    if (wider && close_to(rd<float>(cam + kCamAspect), kAspect169)) wr<float>(cam + kCamAspect, static_cast<float>(w.aspect));
    w.cam_fixes.fetch_add(1, std::memory_order_relaxed);
    if (!w.fov_logged.exchange(true)) {
        const double fov = rd<float>(cam + kCamFov);
        const double kDeg = 180.0 / 3.14159265358979323846;
        const double vt = 2.0 * std::atan(1.0 / std::fabs(P[5])), ht = 2.0 * std::atan(1.0 / std::fabs(P[0]));
        NXLOG_I(kTag, "aspect %.4f: scene camera 0x%llx widened, game FOV %.1f deg vertical; now %.1f deg vertical x %.1f deg horizontal (16:9 was %.1f "
                "horizontal)", w.aspect, static_cast<unsigned long long>(cam), fov * kDeg, vt * kDeg, ht * kDeg,
                2.0 * std::atan(std::tan(fov * 0.5) * kAspect169) * kDeg);
    }
}

int hook_cam_submit(::Ctx* c, uint64_t, void*) {
    const uint64_t cam = c->x[20];
    if (!cam || !mem::is_mapped(cam, 360)) return 0;
    remember_cam(cam);
    fix_camera(cam);
    return 0;
}

int hook_proj_build(::Ctx* c, uint64_t, void*) {
    Wide& w = wide();
    const uint64_t cam = c->x[0];
    if (!(w.aspect > kAspect169) || !cam || !known_cam(cam)) return 0;
    if (close_to(rd<float>(cam + kCamAspect), kAspect169)) {
        wr<float>(cam + kCamAspect, static_cast<float>(w.aspect));
        w.aspect_puts.fetch_add(1, std::memory_order_relaxed);
    }
    return 0;
}

int hook_ui_camera(::Ctx* c, uint64_t, void*) {
    Wide& w = wide();
    const uint64_t sz = c->x[2];
    if (!sz || !w.ui_size || !mem::is_mapped(sz, 8)) return 0;
    if (rd<int32_t>(sz) != kTableW || rd<int32_t>(sz + 4) != kTableH) return 0;
    c->x[2] = w.ui_size;
    w.ui_fixes.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

// ---- frame rate ----
int hook_frame_multiplier(::Ctx* c, uint64_t, void*) {
    State& s = st();
    kbm_frame(c);
    movie_test_frame(c);   // [test] movie (pkp_movie_test.cpp)
    if (!s.fps_fix.load(std::memory_order_relaxed)) return 0;
    const float dt = c->v[8].f32[0];
    // the game's own rule keeps 1.0 within 1.5 ms of 1/60 s; only shorter frames change
    if (dt > 0.f && dt < (1.f / 60.f - 0.0015152f)) {
        const float m = std::max(dt * 60.f, 0.125f);
        c->v[0].f32[0] = m;
        s.last_mult.store(m, std::memory_order_relaxed);
        s.fps_calls.fetch_add(1, std::memory_order_relaxed);
    } else {
        s.last_mult.store(c->v[0].f32[0], std::memory_order_relaxed);
    }
    return 0;
}

// the game's own 60 Hz frame wait: off unless [pkp] game_frame_limit (the runtime's limiter paces to [display] fps_limit)
int hook_sync_period(::Ctx* c, uint64_t, void*) {
    State& s = st();
    s.sync_calls.fetch_add(1, std::memory_order_relaxed);
    if (!s.game_limit.load(std::memory_order_relaxed)) c->x[22] = 0;
    return 0;
}

// fixed-step frame time 1 / 60: frames shorter than 1/60 s get their measured rate
int hook_fixed_step_rate(::Ctx* c, uint64_t, void*) {
    State& s = st();
    s.fixed_calls.fetch_add(1, std::memory_order_relaxed);
    if (!s.fps_fix.load(std::memory_order_relaxed)) return 0;
    const uint64_t now = c->x[28], prev = c->x[25];
    const double freq = c->v[8].f64[0];
    if (now <= prev || freq <= 0) return 0;
    const double rate = freq / static_cast<double>(now - prev);
    if (rate > 60.0 / (1.0 - 0.0015152 * 60.0)) c->v[0].f32[0] = static_cast<float>(std::min(rate, 480.0));
    return 0;
}

// ---- mouse camera ----
float wrap_angle(float a) {
    if (a > static_cast<float>(kTwoPi)) a -= static_cast<float>(kTwoPi);
    if (a < 0.f) a += static_cast<float>(kTwoPi);
    return a;
}

// The strafe style option the camera reads through 0x27abc8 (player 1 reads its own block, the others the shared one).
bool twin_angles(uint8_t player) {
    const uint64_t save = rd<uint64_t>(st().base + kSaveGlobal);
    if (!save) return false;
    if (player == 1) return mem::is_mapped(save + 24218468, 1) && rd<uint8_t>(save + 24218468) != 0;
    return mem::is_mapped(save + 7142872, 1) && (rd<uint8_t>(save + 7142872) & 2) != 0;
}

uint64_t mono_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::atomic<uint64_t> g_follow_ns{0};   // the last follow-camera update (play_follow_live)

// Every follow-camera update past its scene check: while the turn branch ran within the grace time, read the mouse here (this keeps play mode's
// "in game", and so the capture, through short closures of the branch) and keep the counts for the branch; after it, drop what was kept.
int hook_cam_field(::Ctx*, uint64_t, void*) {
    State& s = st();
    if (!s.host) return 0;
    s.cam_field.fetch_add(1, std::memory_order_relaxed);
    g_follow_ns.store(mono_ns(), std::memory_order_relaxed);
    if (s.last_turn_ns == 0 || mono_ns() - s.last_turn_ns > s.grace_ns) {
        if (s.pend_dx != 0 || s.pend_dy != 0) s.cam_dropped.fetch_add(1, std::memory_order_relaxed);
        s.pend_dx = s.pend_dy = 0;
        return 0;
    }
    double dx = 0, dy = 0;
    s.host->input_tick(&dx, &dy);                   // the first call after the frame boundary takes the counts (late sample), later ones get 0
    if (hid::jc2_mouse_active()) {                  // the Joy-Con 2 mouse has the mouse: it is not the camera's
        if (dx != 0 || dy != 0) s.mm_dropped.fetch_add(1, std::memory_order_relaxed);
        dx = dy = 0;
    }
    s.pend_dx += dx;
    s.pend_dy += dy;
    return 0;
}

// The follow placement runs every frame instead of on the 120 Hz tick, so the eye moves with the angles on every frame.
int hook_follow_gate(::Ctx* c, uint64_t, void*) {
    State& s = st();
    if (!s.cam_every_frame.load(std::memory_order_relaxed)) return 0;
    if ((c->x[0] & 1) == 0) s.follow_forced.fetch_add(1, std::memory_order_relaxed);
    c->x[0] |= 1;
    return 0;
}

int hook_cam_stick(::Ctx* c, uint64_t, void*) {
    State& s = st();
    if (!s.host) return 0;
    s.cam_calls.fetch_add(1, std::memory_order_relaxed);
    double dx = 0, dy = 0;
    s.host->input_tick(&dx, &dy);                   // the game is in play: play mode captures the mouse and hands over the counts (0 when the field hook took them)
    dx += s.pend_dx;
    dy += s.pend_dy;
    s.pend_dx = s.pend_dy = 0;
    if (hid::jc2_mouse_active()) {                  // the Joy-Con 2 mouse has the mouse: drop the counts
        if (dx != 0 || dy != 0) s.mm_dropped.fetch_add(1, std::memory_order_relaxed);
        dx = dy = 0;
    }
    s.last_turn_ns = mono_ns();
    if (!s.mouse_cam.load(std::memory_order_relaxed) || (dx == 0 && dy == 0)) return 0;
    const uint64_t cam = c->x[19];
    if (!cam || !mem::is_mapped(cam, kCamIdle + 4)) return 0;
    const play::MouseSettings& m = s.host->settings().mouse;
    // mouse right = stick right (the yaw goes down); mouse up = stick up (the pitch goes up)
    const float dyaw = static_cast<float>(dx * m.yaw_rad * (m.invert_x ? -1.0 : 1.0));
    const float dpitch = static_cast<float>(dy * m.pitch_rad * (m.invert_y ? -1.0 : 1.0));
    wr<float>(cam + kCamYaw, wrap_angle(rd<float>(cam + kCamYaw) - dyaw));
    const bool twin = twin_angles(rd<uint8_t>(cam + kCamPlayer));
    if (twin) wr<float>(cam + kCamTwinYaw, wrap_angle(rd<float>(cam + kCamTwinYaw) - dyaw));
    if (dpitch != 0.f) {
        float p = rd<float>(cam + kCamPitch) + dpitch;
        p = std::min(p, rd<float>(cam + kCamPitchMax));
        p = std::max(p, rd<float>(cam + kCamPitchMin));
        wr<float>(cam + kCamPitch, p);
        if (twin) wr<float>(cam + kCamTwinPitch, std::clamp(rd<float>(cam + kCamTwinPitch) + dpitch, -kHalfPi, kHalfPi));
    }
    wr<uint32_t>(cam + kCamIdle, 0);
    wr<uint8_t>(cam + kCamManual, 1);
    s.cam_moves.fetch_add(1, std::memory_order_relaxed);
    s.host->aim_applied(dx, dy);
    return 0;
}

// The 16:9 internal size: the base rule (play_display.h internal_size), kept at 16:9 (the game's cameras and UI are authored for it) and,
// unless a smaller size was asked for explicitly, at least the console's 1920x1080.
void pick_size(const play::DisplaySettings& d, int window_w, int window_h, int* out_w, int* out_h) {
    int w = kTableW, h = kTableH;
    play::internal_size(d, window_w, window_h, &w, &h);
    const bool fixed = d.render_width > 0 && d.render_height > 0;
    h = std::min(h, static_cast<int>(static_cast<int64_t>(w) * 9 / 16));
    if (!fixed && (d.render_scale <= 0 || d.render_scale >= 100)) h = std::max(h, kTableH);
    h = std::clamp(h / 18 * 18, 360, 4320);                          // 16:9 in whole pixels
    *out_w = h * 16 / 9;
    *out_h = h;
}

}  // namespace

void play_install_render(uint64_t base, const play::DisplaySettings& d, int window_w, int window_h) {
    State& s = st();
    if (s.render_installed) return;
    s.base = base;
    int w = kTableW, h = kTableH;
    pick_size(d, window_w, window_h, &w, &h);
    s.dynres.store(d.dynamic_resolution);
    {
        // [hdr]: the host's HDR scene pipeline (src/gfx/extras): float post targets, and the composite pass's LUT tone map (cbComposite,
        // g_tHdrLut: a log shaper into a 32^3 grade that clips flat at 1 from scene 0.85-1.1) runs as an HDR variant whose highlights climb
        // above paper white to the display's peak (extras spirv_hdr_tonemap_lut; no game hook). The game's own "true HDR" constants
        // (fKIDSTrueHDR, fMaxLum, SIMHDR_PARAMS set at 0x2580198; HdrTvNit / BaseTvNit at 0x2581580) are looked up by name and no shader
        // in the game's data declares them, so driving them would change nothing. Pokopia never asks whether the
        // TV is HDR (no settings / vi / oe / NVN capability query in main) and never calls the NVN2 window HDR procs; what it does declare are the
        // nn::vi layer content luminance ranges (standard 0..300, extended 0..400 nits), which [hdr] mode = native uses (paper white, content max).
        const extras::HdrState hs = extras::hdr_state();
        NXLOG_I(kTag, "HDR: scene %s, output %s, paper white %.0f nits, peak %.0f nits (peak ratio %.2f: %s)", hs.scene_hdr ? "HDR" : "SDR",
                extras::output_kind_name(hs.output), static_cast<double>(hs.paper_white_nits), static_cast<double>(hs.peak_nits),
                static_cast<double>(hs.peak_ratio),
                hs.scene_hdr && hs.peak_ratio > 1.001f ? "the composite LUT tone map's highlights reach the peak" : "highlights stay at paper white");
    }
    // The display's aspect ([display] aspect: auto = the window's, or the fixed render size's when one is set). Anything but 16:9 gives the
    // game targets of that shape (1080 high when wider, 1920 wide when narrower; the back end's scale on top) and the camera / UI hooks above.
    const bool fixed_size = d.render_width > 0 && d.render_height > 0;
    const double aspect = fixed_size ? play::display_aspect(d, d.render_width, d.render_height) : play::display_aspect(d, window_w, window_h);
    Wide& wd = wide();
    wd.aspect = close_to(aspect, kAspect169) ? kAspect169 : aspect;
    bool override_size = config_get_bool("pkp", "render_override", true);
    // A handheld (nxrt/host_device.h), render_scale auto: the window's size (a Steam Deck: 1280x720 in Game Mode; its GPU held ~40-50 fps
    // in the world at 1920x1080) through the back end's scale over the game's own 1920x1080 layout. Game targets below 1920x1080 (the override path
    // with a scale under 100) leave some pass at the console size: the player's see-through-walls silhouette landed on the wall at 2/3 of
    // its place and size, and scaled copies of the scene showed over the picture.
    if (override_size && host_is_handheld() && d.render_scale <= 0 && !fixed_size) override_size = false;
    if (wide_on()) {   // a non-16:9 shape always takes the wide path (render_override only picks the size for 16:9)
        int gw = kTableW, gh = kTableH;
        if (wd.aspect > kAspect169) gw = static_cast<int>(std::lround(kTableH * wd.aspect)) & ~1;
        else gh = static_cast<int>(std::lround(kTableW / wd.aspect)) & ~1;
        gw = std::clamp(gw, 640, 8192);
        gh = std::clamp(gh, 360, 8192);
        s.w.store(gw);
        s.h.store(gh);
        const struct { uint64_t off; SiteHookFn fn; } table[] = {
            {kLevelWidth, hook_level_w}, {kLevelHeight, hook_level_h}, {kFullWidth, hook_full_w}, {kFullHeight, hook_full_h},
            {kCopyParams, hook_copy_params},
        };
        for (const auto& t : table) s.hooks.push_back(hook_add_entry(base + t.off, t.fn));
        s.hooks.push_back(hook_add_mid(base + kSetViewport, &hook_set_viewport));
        s.hooks.push_back(hook_add_mid(base + kSetViewports, &hook_set_viewports));
        s.hooks.push_back(hook_add_entry(base + kRecordViewports, &hook_record_viewports));
        s.hooks.push_back(hook_add_mid(base + kReplayViewports, &hook_replay_viewports));
        s.hooks.push_back(hook_add_mid(base + kLowCopy, &hook_low_copy));
        s.hooks.push_back(hook_add_mid(base + kCamSubmit, &hook_cam_submit));
        s.hooks.push_back(hook_add_mid(base + kProjBuild, &hook_proj_build));
        wd.ui_size = mem::alloc_kernel(16);
        if (wd.ui_size) {
            wr<int32_t>(wd.ui_size, gw);
            wr<int32_t>(wd.ui_size + 4, gh);
            s.hooks.push_back(hook_add_mid(base + kUiCamera, &hook_ui_camera));
        }
        float scale = 1.0f;
        if (fixed_size) scale = static_cast<float>(d.render_height) / gh;
        else if (d.render_scale <= 0) {
            scale = std::min(static_cast<float>(window_h) / gh, static_cast<float>(window_w) / gw);
            if (!host_is_handheld()) scale = std::max(1.0f, scale);   // see pick_size
        }
        else scale = static_cast<float>(std::clamp(d.render_scale, 25, 400)) / 100.0f;
        gfx::tuning().render_scale.store(scale);
        NXLOG_I(kTag, "aspect %.4f (%s, window %dx%d): %dx%d game targets at scale %.3f, 3D view %s, UI kept 16:9 and centred, dynamic resolution %s",
                wd.aspect, d.aspect > 0 ? "[display] aspect" : "auto", window_w, window_h, gw, gh, scale,
                wd.aspect > kAspect169 ? "Hor+ (same vertical FOV, wider horizontal)" : "16:9 width kept, taller", d.dynamic_resolution ? "on" : "off");
        s.render_installed = true;
        return;
    }
    if (!override_size) {
        // The game's own size (the back end's [gfx] render_scale enlarges it). Its dynamic resolution still drops the scene viewport below
        // 1920x1080 when frames run late (1664x936 seen at 1440p: the scene then covers part of the targets, a hard-edged band): with
        // [display] dynamic_resolution off, the per-frame copy of the parameters keeps the top level.
        if (!d.dynamic_resolution) s.hooks.push_back(hook_add_entry(base + kCopyParams, hook_copy_params));
        // [display] render_scale for the NVN back end, which renders the game's targets at that factor: a fixed size by its height, auto
        // by the window's height (at least 1), a percentage of the console's 1920x1080
        float scale = 1.0f;
        if (d.render_width > 0 && d.render_height > 0) scale = static_cast<float>(d.render_height) / kTableH;
        else if (d.render_scale <= 0) scale = host_is_handheld() ? static_cast<float>(window_h) / kTableH : std::max(1.0f, static_cast<float>(window_h) / kTableH);
        else scale = static_cast<float>(std::clamp(d.render_scale, 25, 400)) / 100.0f;
        gfx::tuning().render_scale.store(scale);
        NXLOG_I(kTag, "aspect 16:9 (%s, window %dx%d): letterboxed on other window shapes", d.aspect > 0 ? "[display] aspect" : "auto", window_w, window_h);
        NXLOG_I(kTag, "render resolution: 1920x1080 targets at scale %.3f (window %dx%d, [display] render_scale %s%s), dynamic resolution %s", scale,
                window_w, window_h, d.render_scale <= 0 ? "auto" : std::to_string(d.render_scale).c_str(),
                d.render_scale <= 0 && host_is_handheld() ? ", handheld: the window's size" : "", d.dynamic_resolution ? "on" : "off");
        s.render_installed = true;
        return;
    }
    s.w.store(w);
    s.h.store(h);
    const struct { uint64_t off; SiteHookFn fn; } table[] = {
        {kLevelWidth, hook_level_w}, {kLevelHeight, hook_level_h}, {kFullWidth, hook_full_w}, {kFullHeight, hook_full_h},
        {kCopyParams, hook_copy_params},
    };
    for (const auto& t : table) s.hooks.push_back(hook_add_entry(base + t.off, t.fn));
    s.hooks.push_back(hook_add_mid(base + kSetViewport, &hook_set_viewport));
    s.hooks.push_back(hook_add_mid(base + kSetViewports, &hook_set_viewports));
    s.hooks.push_back(hook_add_entry(base + kRecordViewports, &hook_record_viewports));
    s.hooks.push_back(hook_add_mid(base + kReplayViewports, &hook_replay_viewports));
    s.hooks.push_back(hook_add_mid(base + kLowCopy, &hook_low_copy));
    s.render_installed = true;
    NXLOG_I(kTag, "render resolution %dx%d (window %dx%d, scale %d%%%s), dynamic resolution %s", w, h, window_w, window_h, d.render_scale,
            d.render_width > 0 && d.render_height > 0 ? ", fixed size" : "", d.dynamic_resolution ? "on" : "off");
}

void play_install(uint64_t base, play::GameHost* host, const play::Settings& set) {
    State& s = st();
    if (s.installed) return;
    s.base = base;
    s.host = host;
    s.fps_fix.store(config_get_bool("pkp", "frame_time_fix", true));
    s.mouse_cam.store(config_get_bool("pkp", "mouse_camera", true));
    s.game_limit.store(config_get_bool("pkp", "game_frame_limit", false));
    s.cam_every_frame.store(config_get_bool("pkp", "camera_every_frame", true));
    s.grace_ns = static_cast<uint64_t>(std::clamp<int64_t>(config_get_int("pkp", "camera_grace_ms", 250), 0, 5000)) * 1000000ull;
    s.hooks.push_back(hook_add_mid(base + kFrameMultiplier, &hook_frame_multiplier));
    s.hooks.push_back(hook_add_mid(base + kSyncPeriod, &hook_sync_period));
    s.hooks.push_back(hook_add_mid(base + kFixedStepRate, &hook_fixed_step_rate));
    s.hooks.push_back(hook_add_mid(base + kCamField, &hook_cam_field));
    s.hooks.push_back(hook_add_mid(base + kCamStick, &hook_cam_stick));
    s.hooks.push_back(hook_add_mid(base + kFollowGate, &hook_follow_gate));
    kbm_install(base, &s.hooks);
    s.installed = true;
    play_settings_changed(set);
    NXLOG_I(kTag, "frame-time fix %s, game's 60 Hz wait %s (fps limit %d, vsync %s), mouse camera %s, camera placement %s, grace %llu ms",
            s.fps_fix.load() ? "on" : "off", s.game_limit.load() ? "kept" : "off", set.display.fps_cap, set.display.vsync ? "on" : "off",
            s.mouse_cam.load() ? "on" : "off", s.cam_every_frame.load() ? "every frame" : "game's 120 Hz tick",
            static_cast<unsigned long long>(s.grace_ns / 1000000ull));
}

// ---- Joy-Con 2 mouse mode ----
// The Mouse mode key (Act::MouseMode, middle button in play) flips the request for the Joy-Con 2 mouse: the game then sees the PC mouse on its
// surface, enters its own mouse mode (block placement, Rock Smash cursor) and the camera lets go of the mouse (hook_cam_*: counts dropped
// while jc2_mouse_active(); play mode stops capturing and hides the OS cursor). The request ends when the key is pressed again, or when the
// game stops being in play (a menu, the pause screen, a loading screen).
void mouse_mode_log(State& s, const char* what) {
    const uint64_t now = mono_ns();
    if (s.mm_log_ns != 0 && now - s.mm_log_ns < 250000000ull) return;      // rate limit: a stuck key must not flood the log
    s.mm_log_ns = now;
    NXLOG_I(kTag, "mouse mode %s (jc2_mouse %s, active %d, camera counts dropped %llu)", what, hid::jc2_mouse_mode_name(hid::jc2_mouse_mode()),
            hid::jc2_mouse_active() ? 1 : 0, static_cast<unsigned long long>(s.mm_dropped.load()));
}

void mouse_mode_set(State& s, bool want, const char* why) {
    if (want == s.mm_want) return;
    s.mm_want = want;
    hid::jc2_mouse_request(want);
    mouse_mode_log(s, why);
}

void play_frame(bool in_game, double, play::GameFrame*) {
    State& s = st();
    if (!s.host) return;
    const uint64_t n = s.host->action_activations(play::Act::MouseMode);
    const bool pressed = n != s.mm_seen;
    s.mm_seen = n;
    if (hid::jc2_mouse_mode() == hid::Jc2MouseMode::Off) {
        if (pressed) mouse_mode_log(s, "key ignored: [input] jc2_mouse = off");
        mouse_mode_set(s, false, "off (jc2_mouse = off)");
        return;
    }
    if (!in_game) {                                  // a menu, the pause screen or a loading screen: the camera side takes the mouse back
        mouse_mode_set(s, false, "off (the game left play)");
        return;
    }
    if (pressed) {
        if (hid::jc2_mouse_mode() == hid::Jc2MouseMode::On) mouse_mode_log(s, "key ignored: [input] jc2_mouse = on keeps the Joy-Con 2 mouse always");
        else mouse_mode_set(s, !s.mm_want, s.mm_want ? "off (key)" : "ON (key)");
    }
}

void play_remove() {
    State& s = st();
    if (s.mm_want) hid::jc2_mouse_request(false);
    s.mm_want = false;
    for (uint32_t id : s.hooks) hook_remove(id);
    s.hooks.clear();
    s.installed = s.render_installed = false;
    s.host = nullptr;
}

void play_settings_changed(const play::Settings& set) {
    st().dynres.store(set.display.dynamic_resolution);
    kbm_settings_changed(set);
}

play::DisplayLive play_display_live() {
    play::DisplayLive l;
    l.render_w = st().w.load();
    l.render_h = st().h.load();
    if (wide_on()) l.aspect = wide().aspect;
    l.dynamic_resolution = st().dynres.load();
    return l;
}

uint32_t play_display_calls() { return static_cast<uint32_t>(st().res_calls.load() + st().copy_calls.load()); }


void play_status_text(char* buf, size_t n) {
    const State& s = st();
    const int k = std::snprintf(buf, n, " render=%dx%d res_hooks=%llu copies=%llu set_vp=%llu vp_fixes=%llu mult=%.3f short_frames=%llu sync=%llu fixed_step=%llu cam=%llu cam_moves=%llu cam_field=%llu follow_forced=%llu cam_dropped=%llu mouse_mode=%d mm_dropped=%llu", s.w.load(), s.h.load(),
                  static_cast<unsigned long long>(s.res_calls.load()), static_cast<unsigned long long>(s.copy_calls.load()), static_cast<unsigned long long>(g_set_vp_fixes.load()), static_cast<unsigned long long>(s.vp_fixes.load()),
                  static_cast<double>(s.last_mult.load()), static_cast<unsigned long long>(s.fps_calls.load()),
                  static_cast<unsigned long long>(s.sync_calls.load()), static_cast<unsigned long long>(s.fixed_calls.load()),
                  static_cast<unsigned long long>(s.cam_calls.load()), static_cast<unsigned long long>(s.cam_moves.load()),
                  static_cast<unsigned long long>(s.cam_field.load()), static_cast<unsigned long long>(s.follow_forced.load()),
                  static_cast<unsigned long long>(s.cam_dropped.load()), s.mm_want ? 1 : 0, static_cast<unsigned long long>(s.mm_dropped.load()));
    int k2 = k;
    if (k > 0 && static_cast<size_t>(k) < n && wide_on()) {
        const Wide& w = wide();
        const int m = std::snprintf(buf + k, n - k, " aspect=%.3f cam_fixes=%llu aspect_puts=%llu ui_fixes=%llu", w.aspect,
                                    static_cast<unsigned long long>(w.cam_fixes.load()), static_cast<unsigned long long>(w.aspect_puts.load()),
                                    static_cast<unsigned long long>(w.ui_fixes.load()));
        if (m > 0) k2 += m;
    }
    if (k2 > 0 && static_cast<size_t>(k2) < n) kbm_status_text(buf + k2, n - k2);
}

bool play_follow_live() {
    const uint64_t t = g_follow_ns.load(std::memory_order_relaxed);
    return t != 0 && mono_ns() - t < 1000000000ull;
}

}  // namespace nxrt::games::pkp
