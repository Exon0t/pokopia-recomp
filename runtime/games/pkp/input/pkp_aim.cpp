// SPDX-License-Identifier: GPL-3.0-or-later
// Camera aim and touch aim: the highlighted cell or area of an ability or tool is aimed along a ray instead of where the character faces.
// [pkp] camera_aim (on by default; 0 turns it off): the ray is the camera's own, through the middle of the picture.
// [pkp] touch_aim (on by default): the ray goes from the camera's eye through the finger on the touch screen, while a finger is on the
// picture and for touch_aim_hold seconds after it lifts (so the target stays while the button is pressed), within touch_aim_reach blocks of
// the character. A live touch takes over from camera aim; with camera_aim off the sticks and buttons keep the game's own targeting until a
// finger touches the screen, and get it back when the hold runs out. The same hooks serve both: only the ray differs (aim_for below). The
// finger comes from play mode (play_game.h GameHost::touch_point: SDL's touch mouse events), the eye and the picture's projection from
// the render view the game built last (render/pkp_view_projection.h). A tap fires no button (play mode's touch_route.h): the finger aims,
// the buttons act. The game's own target highlight is the feedback. A touch picks a cell, not a ray: the ray is walked once when the finger
// lands or moves, and the cell it hit stays the target while the touch is live, so a block that breaks leaves the target on the air where
// it was (and a placement goes there) instead of sliding to the floor behind it.
//
// The game's command target query 0x71002e19a0 (x1 = the character; every ability, tool and item preview and action asks it which
// cells are in front) reads the character's heading through the character's virtual +0x18 (the call returns at 0x71002e1a14 with s0 =
// the heading, radians, atan2(x, z) like player + 0x2404) and picks the forward cells from it. The mid hook at 0x71002e1a14, for the
// local player while the embedded follow camera is the active one, replaces s0 by the follow camera's yaw: atan2 of the horizontal part
// of (look-at + 0x50 - eye + 0x60). Nothing else changes: the character does not turn, the shapes, validity and the breaking and placing
// are the game's, and so is the reach without camera_aim_reach (below).
//
// The swing tools (the command-less actions 881..885: Rock Smash, Cut and their upgrades) do not ask that query. Their area is the
// attack's hit shape placed by the character's transform (virtual +0x38; +0x64 = the heading): the preview in 0x7100721e40 (ldr s9,
// [x0, #0x64] at 0x71007220d8; x22 = the character) and the live hit in 0x71006d9b40 (ldr s8, [x0, #0x64] at 0x71006d9d6c; x19 = the
// character), both feeding the shape constructor 0x71009db4c. Two more mid hooks right after those loads replace that heading by the
// same camera yaw, so the game still builds its own shape, reach and damage, only turned to the camera.
//
// The row (cell y) is chosen by the shared helper 0x71006f77b0 (called by the query, the swing shapes 0x7100721e40 / 0x71006d9b40 and
// the placement collectors): it reads the active camera's pitch (follow camera + 0xb0, negated, wrapped to +-pi/2) and sorts it into
// rows by five tuning angles (tuning1 + 0x414..0x424, degrees: up extreme, +2, +1, 0 lower bound, down extreme), giving the row offset
// -1 / 0 / +1 / +2 from the character's feet row, or the cell under the character for the down extreme. The mid hook at 0x71006f7918,
// right after the wrap (s5 = pitch; s4, s2, s1, s3, s0 = the five angles), walks the cell grid along the camera ray (eye + 0x60 ->
// look-at + 0x50); the first solid cell near the character gives the row, and s5 becomes the middle of the angle band
// that selects it (within 1 + camera_aim_reach cells, below). With no solid cell near, or a row out of the bands' range, the game's own
// band stays. The game keeps its shapes and validity.
//
// A floor cell the ray hits beside the character also gives the heading (the direction to that cell), so looking down picks that cell.
// When an ability's action really starts (0x710027e5d8 in StartAction, right before it records the action id: w28 = id, x19 = the
// action component) the character snaps to the same heading (its copy, the movement controller's and, in the save-data mode, the save's), so the animation plays toward the target. It does not turn otherwise.
//
// Reach, [pkp] camera_aim_reach (extra cells, default 0 = the game's own range). The game targets the forward cell next to the
// character: the query 0x71002e19a0 starts from the character's cell plus one cardinal step (kept at [x29 - 0xb8] x, y and
// [x29 - 0xb4] z) and the swing shapes sit about one cell ahead. With extra reach the ray accepts solid cells up to 1 + extra cells
// from the character's column (cells past the old two-cell box only in front of the character). When the first solid cell is outside
// the character's column, within 1 + extra cells (so one cell out without extra reach: the game's own cardinal step missed diagonal hits)
// and in a row the row hook can select, three more mid hooks move only the column to it: the query's start cell at
// 0x71002e1de8 (right after the game's own mouse-mode start cell, the only other writer; not for the nearby-object interaction
// 0x71002f7340, whose A-button target keeps the forward cell), the swing preview's shape position at 0x71007226bc (right before
// 0x710009db4c, x29 - 0x60) and the live hit's at 0x71006da610 (x29 - 0x80, tool path only: w22 set, x27 = the character). The shape
// moves by whole cells, so its size, rotation, row and in-cell offset stay the game's; the row, validity, breaking and placing too.
// The range holds up and down too (the owner's call: camera aim overrides the game's row limit): a hit past the bands the game can
// select, within 1 + extra rows (never fewer than the game's own), gets band 0 and its row moved in: the query's start row
// ([x29 - 0xb6]) and the swing shape's height (rows_down / rows_up / past_bands).
// With no such hit, the query's start cell stays the game's (already a step along the camera's yaw, above). The swing shapes' position
// does not come from that heading: the live hit takes the character's stored attack position (+0x35c0), which lies in front of where the
// character faces, i.e. where it walks. So the two position hooks, with or without extra reach, turn that position around the character
// to the camera's yaw, keeping its distance (turn_shape): the selection follows the camera, never the walking direction.
//
// With the over-the-shoulder camera (pkp_shoulder_camera.cpp) the view is moved off the follow camera by an offset (pkp_view_offset.h): the
// ray of the row and reach hooks starts at the eye plus that offset, parallel to the camera's own, so it still goes through the crosshair.
// The yaw uses only the direction, which the offset does not change.
//
// [pkp] camera_aim_debug draws the ray, its cells and these hooks' choices over the picture (pkp_aim_debug.h); the hooks report to it only
// while it is on.
//
// The game's own Joy-Con 2 mouse mode (character + 0xaa81, the Mouse mode key: pkp_mouse_mode.cpp) aims with its pointer: while it is on,
// every hook here leaves the game alone (follow_camera answers 0).
#include "input/pkp_aim.h"
#include "input/pkp_aim_debug.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "core/pkp_settings.h"
#include "core/pkp_view_offset.h"
#include "render/pkp_view_projection.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "nxrt/hooks.h"
#include "nxrt/host_device.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"

namespace nxrt::games::pkp {
namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kHeadingRead = 0x2e1a14;      // mid: in the target query after the heading call (s0 = heading, x28 = character)
constexpr uint64_t kSwingPreview = 0x7220dc;     // mid: in the swing preview shape after the heading load (s9 = heading, x22 = character)
constexpr uint64_t kSwingLive = 0x6d9d70;        // mid: in the live hit shape after the heading load (s8 = heading, x19 = character)
constexpr uint64_t kPitchRead = 0x6f7918;        // mid: in the row helper after the pitch wrap (s5 = pitch, x22 = character, w21 = extremes allowed)
using layout::kWorld;
constexpr uint64_t kCollisionMeta = 0x355d3c6;   // u16[block id]: bit 0 = solid for the tool rays
constexpr uint64_t kActionStart = 0x27e5d8;      // mid: StartAction, the action really starts (w28 = action id, x19 = action component)
constexpr uint64_t kActionOwner = 0x978;         // action component: the character
constexpr uint32_t kFirstAbility = 878, kLastAbility = 885;  // the abilities' actions (Leafage, Rototiller, Water Gun, swing tools)
// character heading (layout::kHeading), save heading: both kPlayerMode and kPlayerModeByte 0 = the heading lives in the save data
using layout::kHeading;
using layout::kSaveRoot;
using layout::kSaveHeading;
using layout::kPlayerMode;
using layout::kPlayerModeByte;
constexpr uint64_t kMoveTable = 0x356a778;       // qword: + 0x58 + slot * 8 = the movement component of a local slot
using layout::kController;                       // movement component: the controller; + kControllerYaw its heading (what the model and attacks use)
constexpr uint64_t kControllerYaw = 0x64;
using layout::kPosition;
using layout::kPlayerManager;
constexpr uint64_t kCameraManager = 0x3561948;   // qword: the camera manager
using layout::kManagerPlayer;
using layout::kMouseModeOn;
constexpr uint64_t kFollow = 0x10;               // camera manager + 0x10: the embedded follow camera of slot 0
constexpr uint64_t kActiveTable = 0xc58;         // + signed byte(manager + 0xcc8) * 8: the active camera pointer of slot 0
constexpr uint64_t kActiveIndex = 0xcc8;
constexpr uint64_t kLook = 0x50, kEye = 0x60;    // follow camera: look-at and eye (vec4)
constexpr uint64_t kSwingPreviewReturn = 0x7223cc, kSwingLiveReturn = 0x6da458;  // the row helper's return into the swing shapes
constexpr uint64_t kStepRows = 0x5a84, kStepFlag = 0x5a8c;  // character: step count ahead (int) and its flag; the swing shapes raise by it
constexpr int kReach = 2;                          // cells from the character's column within which a hit is accepted without extra reach
constexpr uint64_t kQueryBase = 0x2e1de8;        // mid: in the target query after the start cell is final (x28 = character, cell at x29 - 0xb8 / - 0xb4)
constexpr uint64_t kSwingPreviewPos = 0x7226bc;  // mid: in the swing preview right before the shape constructor (x22 = character, position at x29 - 0x60)
constexpr uint64_t kSwingLivePos = 0x6da610;     // mid: in the live hit after the position is chosen (w22 = tool path, x27 = character, position at x29 - 0x80)
constexpr uint64_t kQueryRowReturn = 0x2e1d78;   // the row helper's return into the target query
constexpr uint64_t kInteractReturn = 0x2f73a8;   // the target query's return into the nearby-object interaction 0x2f7340
constexpr uint64_t kReachLogs = 8;               // reach column moves logged (on change)

using guest::rd;
using guest::wr;

struct State {
    uint64_t base = 0;
    play::GameHost* host = nullptr;
    uint64_t logged = 0;
    uint64_t logged_pitch = 0;
    uint64_t logged_reach = 0;
    uint64_t logged_touch = 0;
    bool cam_on = false;        // [pkp] camera_aim
    int extra = 0;              // [pkp] camera_aim_reach
    int reach = kReach;
    bool touch_on = false;      // [pkp] touch_aim
    int touch_reach = 4;        // [pkp] touch_aim_reach: cells from the character's column a touched cell may be
    uint64_t touch_hold_ns = 0; // [pkp] touch_aim_hold
    // the touch target (frame(), main thread -> the hooks, game thread)
    std::mutex touch_mu;
    bool touch_live = false;    // a finger is on the picture, or lifted within the hold
    float touch_u = 0, touch_v = 0;
    uint64_t touch_gen = 0;     // counts up when a finger lands or moves: a new touch to resolve into a cell
    // the cell the current touch resolved to (the game thread's hooks, aim_for): once per touch_gen
    struct TouchCell {
        uint64_t gen = ~0ull;
        bool hit = false;
        int x = 0, y = 0, z = 0;
    } touch_cell;
    uint64_t logged_cell = 0;
};
State g;

// A ray in world units: origin and direction (not normalised).
struct Ray {
    float ox = 0, oy = 0, oz = 0, dx = 0, dy = 0, dz = 0;
};

enum class Source : uint8_t { None, Camera, Touch };

// What the hooks aim with for the local player's character: nothing (the game's own targeting), the camera's ray or the finger's.
struct Aim {
    Source src = Source::None;
    uint64_t cam = 0;      // the follow camera (follow_camera)
    Ray ray;
    int reach = kReach;    // cells from the character's column within which the ray walk accepts a hit
    int extra = 0;         // the extra cells of reach the column and row hooks move to (1 + extra cells)
    bool fixed = false;    // touch: the target is this cell (resolved when the finger landed), not the ray
    int cx = 0, cy = 0, cz = 0;
};

// The follow camera of the local player's character ch while it is the active one, else 0.
uint64_t follow_camera(uint64_t ch) {
    const uint64_t mgr = rd<uint64_t>(g.base + kPlayerManager);
    if (!mgr || ch != rd<uint64_t>(mgr + kManagerPlayer)) return 0;
    if (rd<uint8_t>(ch + kMouseModeOn)) return 0;   // the game's mouse mode aims with its pointer
    const uint64_t cm = rd<uint64_t>(g.base + kCameraManager);
    if (!cm || !mem::is_mapped(cm, kActiveIndex + 1)) return 0;
    const int8_t slot = rd<int8_t>(cm + kActiveIndex);
    if (slot < 0 || rd<uint64_t>(cm + kActiveTable + static_cast<uint64_t>(slot) * 8) != cm + kFollow) return 0;
    return cm + kFollow;
}

// True when cell (x, y, z) holds a block that is solid for the tool rays.
bool solid_cell(int x, int y, int z) {
    if (x < 0 || x > 2047 || z < 0 || z > 2047 || y < 0 || y > 127) return false;
    const uint64_t world = rd<uint64_t>(g.base + kWorld);
    if (!world) return false;
    const uint64_t map = rd<uint64_t>(world + 0x6000030), cells = rd<uint64_t>(world + 0x6000018);
    if (!map || !cells) return false;
    const uint16_t chunk = rd<uint16_t>(map + static_cast<uint64_t>((x >> 5) + (z >> 5) * 64) * 2);
    if (chunk >= 256) return false;
    const uint16_t id = rd<uint16_t>(cells + chunk * 0x60000ull + static_cast<uint64_t>((x & 31) | ((z & 31) << 5) | (y << 10)) * 3) & 0x7ff;
    return id != 0 && (rd<uint16_t>(g.base + kCollisionMeta + 2ull * id) & 1);
}

int feet_row(uint64_t ch) { return static_cast<int>(std::floor((rd<float>(ch + kPosition + 4) + 0.2f) / 100.f)); }

struct RayHit { int x = 0, y = 0, z = 0, h = 0; };  // h: cells from the character's column

// The camera's ray: through the crosshair, the view's centre. With the over-the-shoulder camera (pkp_view_offset.h) the view is moved off
// the follow camera's eye and look-at by an offset, so the ray starts there and runs parallel (zero offset: the follow camera's own eye).
Ray camera_ray_of(uint64_t cam) {
    const view_offset::Offset vo = view_offset::get(0);   // the follow camera is the first player's
    const float fx = rd<float>(cam + kEye), fy = rd<float>(cam + kEye + 4), fz = rd<float>(cam + kEye + 8);
    Ray r;
    r.ox = fx + vo.x, r.oy = fy + vo.y, r.oz = fz + vo.z;
    r.dx = rd<float>(cam + kLook) - fx, r.dy = rd<float>(cam + kLook + 4) - fy, r.dz = rd<float>(cam + kLook + 8) - fz;   // the direction is the camera's own
    return r;
}

float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
void cross3(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}
bool norm3(float v[3]) {
    const float len = std::sqrt(dot3(v, v));
    if (!(len > 1e-6f) || !std::isfinite(len)) return false;
    v[0] /= len, v[1] /= len, v[2] /= len;
    return true;
}

// The finger's ray: from the render view's eye through the point of the picture the finger touched (u, v in 0..1, v down). The view is the
// one the game built last for player 0 (pkp_view_projection.h: eye, look-at, up, and the projection). The direction is f + a r + b u in the
// view's basis (f forward, r right, u up on the picture). With the view-projection rows (the matrix way: they carry the aspect fix and the
// shoulder offset) a and b solve the two clip equations x / w = xc, y / w = yc for a direction, which has no translation part; else the
// look-at way's scales give them directly (clip x = a * sx, y = b * sy at w = 1).
bool touch_ray_of(float tu, float tv, Ray& out) {
    const view_projection::View v = view_projection::capture(g.base, 0);
    if (!v.valid) return false;
    float f[3] = {v.look[0] - v.eye[0], v.look[1] - v.eye[1], v.look[2] - v.eye[2]}, r[3], u[3];
    if (!norm3(f)) return false;
    cross3(f, v.up, r);
    if (!norm3(r)) return false;
    cross3(r, f, u);
    if (!norm3(u)) return false;
    const float xc = 2.f * tu - 1.f, yc = v.y_sign * (1.f - 2.f * tv);
    float a = 0, b = 0;
    bool solved = false;
    if (v.matrix) {
        // the linear part of the rows for a direction d: x = d . (m0, m4, m8), y = d . (m1, m5, m9), w = d . (m3, m7, m11)
        auto lin = [&](const float d[3], float& x, float& y, float& w) {
            x = d[0] * v.vp[0] + d[1] * v.vp[4] + d[2] * v.vp[8];
            y = d[0] * v.vp[1] + d[1] * v.vp[5] + d[2] * v.vp[9];
            w = d[0] * v.vp[3] + d[1] * v.vp[7] + d[2] * v.vp[11];
        };
        float fx, fy, fw, rx, ry, rw, ux, uy, uw;
        lin(f, fx, fy, fw);
        lin(r, rx, ry, rw);
        lin(u, ux, uy, uw);
        // a (rx - xc rw) + b (ux - xc uw) = xc fw - fx ;  a (ry - yc rw) + b (uy - yc uw) = yc fw - fy
        const float m00 = rx - xc * rw, m01 = ux - xc * uw, m10 = ry - yc * rw, m11 = uy - yc * uw;
        const float c0 = xc * fw - fx, c1 = yc * fw - fy;
        const float det = m00 * m11 - m01 * m10;
        if (std::isfinite(det) && std::fabs(det) > 1e-9f) {
            a = (c0 * m11 - c1 * m01) / det;
            b = (m00 * c1 - m10 * c0) / det;
            solved = std::isfinite(a) && std::isfinite(b) && std::fabs(a) < 50.f && std::fabs(b) < 50.f;
        }
    }
    if (!solved) {
        if (!(v.sx > 1e-3f) || !(v.sy > 1e-3f)) return false;
        a = xc / v.sx;
        b = (1.f - 2.f * tv) / v.sy;   // the look-at way: up on the picture is the up vector
    }
    out.ox = v.eye[0], out.oy = v.eye[1], out.oz = v.eye[2];
    out.dx = f[0] + r[0] * a + u[0] * b;
    out.dy = f[1] + r[1] * a + u[1] * b;
    out.dz = f[2] + r[2] * a + u[2] * b;
    return std::isfinite(out.dx) && std::isfinite(out.dy) && std::isfinite(out.dz);
}

bool walk_ray(const Ray& ray, uint64_t ch, RayHit& r, int reach, bool record = true);

// The ray the hooks aim with for character ch: the finger's while a touch target is live (touch aim), else the camera's (camera aim), else
// none (the game's own targeting). Both need the local player's follow camera to be the active one (follow_camera: not in the game's own
// mouse mode either).
Aim aim_for(uint64_t ch) {
    Aim a;
    a.cam = follow_camera(ch);
    if (!a.cam) return a;
    if (g.touch_on) {
        bool live = false;
        float tu = 0, tv = 0;
        uint64_t gen = 0;
        {
            std::lock_guard<std::mutex> lk(g.touch_mu);
            live = g.touch_live, tu = g.touch_u, tv = g.touch_v, gen = g.touch_gen;
        }
        if (live) {
            const int reach = std::max(kReach, g.touch_reach);
            if (gen != g.touch_cell.gen) {   // a new touch (the finger landed or moved): the cell its ray hits, once
                RayHit r;
                const bool hit = touch_ray_of(tu, tv, a.ray) && walk_ray(a.ray, ch, r, reach, true);
                g.touch_cell = {gen, hit, r.x, r.y, r.z};
                if (g.logged_cell < 8) {
                    g.logged_cell++;
                    if (hit) NXLOG_I(kTag, "touch aim: touch at %.3f, %.3f -> cell %d,%d,%d (%d cells out)", static_cast<double>(tu), static_cast<double>(tv), r.x, r.y, r.z, r.h);
                    else NXLOG_I(kTag, "touch aim: touch at %.3f, %.3f -> nothing within %d cells", static_cast<double>(tu), static_cast<double>(tv), reach);
                }
            }
            if (g.touch_cell.hit) {
                a.src = Source::Touch;
                a.fixed = true;
                a.cx = g.touch_cell.x, a.cy = g.touch_cell.y, a.cz = g.touch_cell.z;
                a.reach = reach;
                a.extra = std::max(0, g.touch_reach - 1);
                return a;
            }
            // a finger on nothing within reach: camera aim (if on) or the game's own targeting
        }
    }
    if (g.cam_on) {
        a.ray = camera_ray_of(a.cam);
        a.src = Source::Camera;
        a.reach = g.reach;
        a.extra = g.extra;
    }
    return a;
}

// Walk the cell grid along the ray from its origin; the first solid cell within reach cells of the character's column is the hit.
// Cells before the ray gets near the character are skipped; past the kReach box only cells in front of the character count. record: the
// walk is reported to the debug view (not for the nearby-object interaction).
bool walk_ray(const Ray& ray, uint64_t ch, RayHit& r, int reach, bool record) {
    const float ex = ray.ox, ey = ray.oy, ez = ray.oz;
    const float dx = ray.dx, dy = ray.dy, dz = ray.dz;
    const float px = rd<float>(ch + kPosition), pz = rd<float>(ch + kPosition + 8);
    if (!(dx * dx + dz * dz > 1e-6f) || !std::isfinite(ey) || !std::isfinite(px) || !std::isfinite(pz)) return false;
    const int cx = static_cast<int>(std::floor((px + 102400.f) / 100.f)), cz = static_cast<int>(std::floor((pz + 102400.f) / 100.f));
    int x = static_cast<int>(std::floor((ex + 102400.f) / 100.f)), y = static_cast<int>(std::floor(ey / 100.f)),
        z = static_cast<int>(std::floor((ez + 102400.f) / 100.f));
    const int sx = dx > 0.f ? 1 : -1, sy = dy > 0.f ? 1 : -1, sz = dz > 0.f ? 1 : -1;
    const float inf = 1e30f;
    const float tdx = std::fabs(dx) > 1e-6f ? 100.f / std::fabs(dx) : inf, tdy = std::fabs(dy) > 1e-6f ? 100.f / std::fabs(dy) : inf,
                tdz = std::fabs(dz) > 1e-6f ? 100.f / std::fabs(dz) : inf;
    float tx = tdx < inf ? ((dx > 0.f ? static_cast<float>(x + 1) * 100.f - 102400.f - ex : ex - (static_cast<float>(x) * 100.f - 102400.f)) / std::fabs(dx)) : inf;
    float ty = tdy < inf ? ((dy > 0.f ? static_cast<float>(y + 1) * 100.f - ey : ey - static_cast<float>(y) * 100.f) / std::fabs(dy)) : inf;
    float tz = tdz < inf ? ((dz > 0.f ? static_cast<float>(z + 1) * 100.f - 102400.f - ez : ez - (static_cast<float>(z) * 100.f - 102400.f)) / std::fabs(dz)) : inf;
    const bool dbg = record && aim_debug::on();
    if (dbg) {
        const float o[3] = {ex, ey, ez}, d[3] = {dx, dy, dz}, p[3] = {px, rd<float>(ch + kPosition + 4), pz};
        aim_debug::ray_begin(o, d, p, feet_row(ch), reach);
    }
    using aim_debug::Step;
    bool near = false;
    for (int n = 0; n < 128; n++) {
        if (tx <= ty && tx <= tz) { x += sx; tx += tdx; }
        else if (ty <= tz) { y += sy; ty += tdy; }
        else { z += sz; tz += tdz; }
        const int h = std::max(std::abs(x - cx), std::abs(z - cz));
        if (h > reach) {
            if (near) break;
            if (dbg) aim_debug::ray_step(x, y, z, Step::Far);
            continue;
        }
        near = true;
        if (h > kReach && (static_cast<float>(x) * 100.f - 102350.f - px) * dx + (static_cast<float>(z) * 100.f - 102350.f - pz) * dz <= 0.f) {
            if (dbg) aim_debug::ray_step(x, y, z, Step::Behind);
            continue;
        }
        if (solid_cell(x, y, z)) {
            r = {x, y, z, h};
            if (dbg) aim_debug::ray_hit(x, y, z, h);
            return true;
        }
        if (dbg) aim_debug::ray_step(x, y, z, Step::Tested);
    }
    return false;
}

// The cell the aim targets for character ch: a touch's fixed cell (its distance from the character's column measured now, so walking away
// takes it out of reach), else the first solid cell the ray hits (walk_ray).
bool aim_walk(const Aim& a, uint64_t ch, RayHit& r, int reach, bool record = true) {
    if (!a.fixed) return walk_ray(a.ray, ch, r, reach, record);
    const float px = rd<float>(ch + kPosition), pz = rd<float>(ch + kPosition + 8);
    if (!std::isfinite(px) || !std::isfinite(pz)) return false;
    const int cx = static_cast<int>(std::floor((px + 102400.f) / 100.f)), cz = static_cast<int>(std::floor((pz + 102400.f) / 100.f));
    r = {a.cx, a.cy, a.cz, std::max(std::abs(a.cx - cx), std::abs(a.cz - cz))};
    return r.h <= reach;
}

// The heading the aim gives the local player's character ch, or false when nothing aims (not the local player, the follow camera not the
// active one, or neither mod live). Camera: the camera's yaw, or the direction to a floor cell the ray hits beside the character. Touch:
// the direction to the touched cell; a finger on nothing within reach leaves the game's own heading. interaction: the nearby-object
// interaction's query keeps the old box (kReach) and is not reported to the debug view.
bool aim_yaw(uint64_t ch, float& yaw, bool interaction = false) {
    const Aim a = aim_for(ch);
    if (a.src == Source::None) return false;
    if (!(a.ray.dx * a.ray.dx + a.ray.dz * a.ray.dz > 1e-6f)) return false;
    yaw = std::atan2(a.ray.dx, a.ray.dz);
    RayHit r;
    const bool hit = aim_walk(a, ch, r, interaction ? kReach : a.reach, !interaction);
    const bool to_cell = a.src == Source::Touch ? hit && r.h > 0 : hit && r.h > 0 && r.y < feet_row(ch);
    if (a.src == Source::Touch && !to_cell) return false;
    if (to_cell) {
        yaw = std::atan2(static_cast<float>(r.x) * 100.f - 102350.f - rd<float>(ch + kPosition),
                         static_cast<float>(r.z) * 100.f - 102350.f - rd<float>(ch + kPosition + 8));
    }
    return std::isfinite(yaw);
}

void replace(::Ctx* c, unsigned reg, uint64_t ch, const char* what, bool interaction = false) {
    float yaw;
    if (!aim_yaw(ch, yaw, interaction)) return;
    if (g.logged < 8) {
        g.logged++;
        NXLOG_I(kTag, "camera aim: %s heading %.3f -> camera yaw %.3f", what, static_cast<double>(c->v[reg].f[0]), static_cast<double>(yaw));
    }
    c->v[reg].f[0] = yaw;
}

// The target query whose frame pointer is fp was called by the nearby-object interaction: it keeps the old box (kReach).
bool interaction(uint64_t fp) { return rd<uint64_t>(fp + 8) - g.base == kInteractReturn; }

int hook_heading(::Ctx* c, uint64_t, void*) {
    replace(c, 0, c->x[28], "query", interaction(c->x[29]));
    return 0;
}
int hook_swing_preview(::Ctx* c, uint64_t, void*) { replace(c, 9, c->x[22], "swing preview"); return 0; }
int hook_swing_live(::Ctx* c, uint64_t, void*) { replace(c, 8, c->x[19], "swing hit"); return 0; }

// The row the swing shapes apply the band offset to: the feet row plus the step count when the step flag is set.
int swing_base_row(uint64_t ch) {
    const int steps = rd<uint8_t>(ch + kStepFlag) ? rd<int32_t>(ch + kStepRows) : 0;
    return feet_row(ch) + (steps >= -8 && steps <= 8 ? steps : 0);
}

// The camera ray's first solid cell outside the character's own column and within its range (1 + extra reach cells), in a row the row
// hook selects (lo..hi rows from base). One cell out counts too: the game's own pick is the character's cell plus a cardinal step of the
// heading, so a hit diagonal to the character (or at an angle that rounds to another side) was not the cell selected.
// Vertical reach: the aim's range is the same up and down as sideways (1 + extra cells), never less than the game's own rows (the
// query's bands reach -1..+2 rows from the feet, a swing's shape -1..+3 from its base). A hit past the bands (and inside that range)
// gets band 0 (hook_pitch) and its row moved in by whole cells: the query's start row, the swing shape's height.
int rows_down(int extra) { return -(1 + extra); }
int rows_up(bool swing, int extra) { return std::max(swing ? 3 : 2, 1 + extra); }
bool past_bands(int d, bool swing) { return swing ? d < -1 || d > 3 : d < -1 || d > 2; }

bool aim_hit(uint64_t ch, int base, bool swing, RayHit& r, Aim& a, aim_debug::Use use) {
    a = aim_for(ch);
    const bool hit = a.src != Source::None && aim_walk(a, ch, r, a.reach);
    const int d = r.y - base, lo = rows_down(a.extra), hi = rows_up(swing, a.extra);
    const char* reject = !a.cam                      ? "follow camera not active"
                         : a.src == Source::None     ? "no aim (camera aim off, no touch)"
                         : !hit                      ? "no hit"
                         : r.h < 1                   ? "own column"
                         : r.h > 1 + a.extra         ? "out of reach (1 + extra cells)"
                         : d < lo || d > hi          ? "row outside the hook's range"
                                                     : nullptr;
    if (aim_debug::on()) aim_debug::pick(use, hit, r.x, r.y, r.z, r.h, d, reject);
    return !reject;
}

// A column move, logged when it differs from the last one logged.
void log_reach(const char* what, int ox, int oz, const RayHit& r) {
    static const char* last_what = nullptr;
    static int last[3] = {};
    if (g.logged_reach >= kReachLogs || (what == last_what && r.x == last[0] && r.y == last[1] && r.z == last[2])) return;
    g.logged_reach++;
    last_what = what, last[0] = r.x, last[1] = r.y, last[2] = r.z;
    NXLOG_I(kTag, "camera aim: reach %s column %d,%d -> %d,%d (hit %d,%d,%d, %d cells out)", what, ox, oz, r.x, r.z, r.x, r.y, r.z, r.h);
}

// Target query: its start cell (the forward cell) moves to the column of the ray hit; the row is still chosen after this (a hit past the
// bands: band 0 and the start row is the hit's). The nearby-object interaction keeps the game's forward cell.
int hook_query_base(::Ctx* c, uint64_t, void*) {
    const uint64_t ch = c->x[28], fp = c->x[29];
    if (interaction(fp)) return 0;
    const int ox = rd<int16_t>(fp - 0xb8), oz = rd<int16_t>(fp - 0xb4);
    RayHit r;
    Aim a;
    const int feet = feet_row(ch);
    const bool move = aim_hit(ch, feet, false, r, a, aim_debug::Use::Query);
    const int oy = rd<int16_t>(fp - 0xb6);
    const int ny = move && past_bands(r.y - feet, false) ? r.y : oy;
    if (aim_debug::on()) aim_debug::query_start(ox, oy, oz, move ? r.x : ox, ny, move ? r.z : oz);
    if (!move) return 0;
    if (ny != oy) wr<int16_t>(fp - 0xb6, static_cast<int16_t>(ny));
    wr<int16_t>(fp - 0xb8, static_cast<int16_t>(r.x));
    wr<int16_t>(fp - 0xb4, static_cast<int16_t>(r.z));
    log_reach("query", ox, oz, r);
    return 0;
}

// No ray hit to move to (camera aim): the game's position (the character's stored one, +0x35c0, is placed in front of where the character
// faces, so while walking it is where the character walks) turned around the character to the camera's yaw, its distance kept. Without
// this the shape flipped between the aimed column and the walking direction each time a far hit came in and out of reach.
void turn_shape(uint64_t ch, uint64_t pos, float px, float pz, const Aim& a) {
    const float dx = a.ray.dx, dz = a.ray.dz;
    const float len = std::sqrt(dx * dx + dz * dz);
    const float cx = rd<float>(ch + kPosition), cz = rd<float>(ch + kPosition + 8);
    const float ox = px - cx, oz = pz - cz, dist = std::sqrt(ox * ox + oz * oz);
    if (!(len > 1e-3f) || !std::isfinite(cx) || !std::isfinite(cz) || !(dist > 1.f) || dist > 400.f) return;   // at the character, or not near it
    wr<float>(pos, cx + dx / len * dist);
    wr<float>(pos + 8, cz + dz / len * dist);
}

// Swing shape position (vec4 at pos): moved by whole cells into the column of the ray hit (and, past the bands, its height: the shape
// spans the band-0 row and the one above, moved to the hit's row and the one below), else turned to the camera.
void shift_shape(uint64_t ch, uint64_t pos, const char* what, aim_debug::Use use) {
    const float px = rd<float>(pos), pz = rd<float>(pos + 8);
    if (!std::isfinite(px) || !std::isfinite(pz)) return;
    const float before[3] = {px, rd<float>(pos + 4), pz};
    const char* how = "moved to the hit column";
    RayHit r;
    Aim a;
    // the swing asks for the band below the hit row (hook_pitch), bands -1..+2
    const int base = swing_base_row(ch);
    if (!aim_hit(ch, base, true, r, a, use)) {
        // a finger on nothing within reach leaves the game's own shape; the camera turns it to its yaw
        if (a.src == Source::Camera) turn_shape(ch, pos, px, pz, a), how = "turned to the camera";
        else how = "left as the game's";
    } else if (past_bands(r.y - base, true)) {
        const int cx = static_cast<int>(std::floor((px + 102400.f) / 100.f)), cz = static_cast<int>(std::floor((pz + 102400.f) / 100.f));
        wr<float>(pos, px + static_cast<float>(r.x - cx) * 100.f);
        wr<float>(pos + 4, rd<float>(pos + 4) + static_cast<float>(r.y - 1 - base) * 100.f);
        wr<float>(pos + 8, pz + static_cast<float>(r.z - cz) * 100.f);
        how = "moved to the hit column and row";
        log_reach(what, cx, cz, r);
    } else if (const int cx = static_cast<int>(std::floor((px + 102400.f) / 100.f)), cz = static_cast<int>(std::floor((pz + 102400.f) / 100.f));
               cx == r.x && cz == r.z) {
        how = "already in the hit column";
    } else {
        wr<float>(pos, px + static_cast<float>(r.x - cx) * 100.f);
        wr<float>(pos + 8, pz + static_cast<float>(r.z - cz) * 100.f);
        log_reach(what, cx, cz, r);
    }
    if (aim_debug::on()) {
        const float after[3] = {rd<float>(pos), rd<float>(pos + 4), rd<float>(pos + 8)};
        aim_debug::swing_pos(use, before, after, how);
    }
}

int hook_swing_preview_pos(::Ctx* c, uint64_t, void*) {
    shift_shape(c->x[22], c->x[29] - 0x60, "swing preview", aim_debug::Use::SwingPreview);
    return 0;
}

int hook_swing_live_pos(::Ctx* c, uint64_t, void*) {
    const uint64_t ch = c->x[27];
    if ((c->x[22] & 0xff) == 0 || !ch) return 0;
    // the position is the character's stored one (+0x35c0, w = 1) or the transform's; the move to the hit column is the same for both
    shift_shape(ch, c->x[29] - 0x80, "swing hit", aim_debug::Use::SwingHit);
    return 0;
}

// The row helper sorted the camera pitch (s5) into a row band; replace it by the middle of the band whose row holds the cell the
// camera ray hits.
int hook_pitch(::Ctx* c, uint64_t, void*) {
    const uint64_t ch = c->x[22];
    const Aim a = aim_for(ch);
    if (a.src == Source::None) return 0;
    const int row = feet_row(ch);
    // the row the game applies the band offset to: the feet row; the swing shapes add the step count when the step flag is set
    const uint64_t ret = rd<uint64_t>(c->x[29] + 8) - g.base;
    const bool swing = ret == kSwingPreviewReturn || ret == kSwingLiveReturn;
    const int base = swing ? swing_base_row(ch) : row;
    const bool ia = ret == kQueryRowReturn && interaction(rd<uint64_t>(c->x[29]));
    const aim_debug::Use use = ret == kQueryRowReturn ? aim_debug::Use::Query : ret == kSwingPreviewReturn ? aim_debug::Use::SwingPreview
                               : ret == kSwingLiveReturn ? aim_debug::Use::SwingHit : aim_debug::Use::Other;
    RayHit r;
    const bool hit = aim_walk(a, ch, r, ia ? kReach : a.reach, !ia);
    const int d = r.y - base;
    // A swing's shape spans the band's row and the one above, and the game keeps only the upper one when it holds a block (0x7100409160):
    // ask for the row below the hit. The band of the lowest row keeps the floor in front when the row above it is empty.
    int band = swing ? std::max(d - 1, -1) : d;
    const float t0 = c->v[0].f[0], t1 = c->v[3].f[0], t2 = c->v[1].f[0], t3 = c->v[2].f[0], t4 = c->v[4].f[0];
    float pitch = NAN;                               // not finite: the game's own band stays
    const char* why = "row outside the bands: the game's band";
    if (!hit) why = "nothing near: the game's band";
    else if (r.h == 0 && r.y != row - 1) why = "own column above the floor: the game's band";
    else if (r.h == 0) pitch = t0 - 0.01f, band = aim_debug::kUnder, why = "down extreme: the cell under the character";
    else if (!ia && r.h <= 1 + a.extra && past_bands(d, swing) && d >= rows_down(a.extra) && d <= rows_up(swing, a.extra))
        pitch = (t1 + t2) * 0.5f, band = 0, why = "past the bands: band 0, the row moved to the hit";
    else if (band == -1) pitch = (t0 + t1) * 0.5f, why = "band -1";
    else if (band == 0) pitch = (t1 + t2) * 0.5f, why = "band 0";
    else if (band == 1) pitch = (t2 + t3) * 0.5f, why = "band +1";
    else if (band == 2) pitch = t4 > t3 ? (t3 + t4) * 0.5f : t3, why = "band +2";
    if (!ia && aim_debug::on()) aim_debug::row(use, hit, base, d, band, why, c->v[5].f[0], pitch);
    if (!std::isfinite(pitch)) return 0;
    if (g.logged_pitch < 8) {
        g.logged_pitch++;
        NXLOG_I(kTag, "camera aim: row pitch %.3f -> %.3f (row %+d of base %d, hit %d,%d,%d, bands %.2f %.2f %.2f %.2f %.2f, extremes %u)",
                static_cast<double>(c->v[5].f[0]), static_cast<double>(pitch), d, base, r.x, r.y, r.z, static_cast<double>(t0), static_cast<double>(t1),
                static_cast<double>(t2), static_cast<double>(t3), static_cast<double>(t4), static_cast<unsigned>(c->x[21] & 0xffffffffu));
    }
    c->v[5].f[0] = pitch;
    return 0;
}

// An ability's action really starts: the character turns to the aim.
int hook_action_start(::Ctx* c, uint64_t, void*) {
    const uint32_t id = static_cast<uint32_t>(c->x[28]) & 0xffff;
    if (id < kFirstAbility || id > kLastAbility) return 0;
    const uint64_t ch = rd<uint64_t>(c->x[19] + kActionOwner);
    if (!ch) return 0;
    float yaw;
    if (!aim_yaw(ch, yaw)) return 0;
    std::memcpy(reinterpret_cast<void*>(ch + kHeading), &yaw, sizeof yaw);
    const uint64_t moves = rd<uint64_t>(g.base + kMoveTable);
    const uint64_t move = moves ? rd<uint64_t>(moves + 0x58) : 0;  // ponytail: slot 0, the only player with a follow camera here
    if (move) std::memcpy(reinterpret_cast<void*>(move + kController + kControllerYaw), &yaw, sizeof yaw);
    const uint64_t save = rd<uint64_t>(g.base + kSaveRoot);
    const bool alt = rd<uint32_t>(ch + kPlayerMode) == 0 && rd<uint8_t>(ch + kPlayerModeByte) == 0;
    if (alt && save) std::memcpy(reinterpret_cast<void*>(save + kSaveHeading), &yaw, sizeof yaw);
    return 0;
}

// The touch target, once a frame on the main thread: a finger on the picture, or one that lifted within the hold, keeps the target live.
// Touches outside play (a menu, a loading screen) do not count.
void frame(bool in_game, double, play::GameFrame*) {
    if (!g.touch_on || !g.host) return;
    play::TouchPoint tp;
    bool live = false;
    float u = 0, v = 0;
    if (g.host->touch_point(&tp) && in_game && tp.inside) {
        live = tp.down || (g.touch_hold_ns > 0 && tp.age_ns <= g.touch_hold_ns);
        u = tp.u, v = tp.v;
    }
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(g.touch_mu);
        changed = live != g.touch_live;
        // a new touch: the finger landed (the target was not live, or the finger is down again at another spot) or moved while down
        if (live && (!g.touch_live || (tp.down && (std::fabs(u - g.touch_u) > 0.002f || std::fabs(v - g.touch_v) > 0.002f)))) g.touch_gen++;
        g.touch_live = live;
        if (live) g.touch_u = u, g.touch_v = v;
    }
    if (changed && g.logged_touch < 8) {
        g.logged_touch++;
        NXLOG_I(kTag, "touch aim: target %s%s at %.3f, %.3f of the picture", live ? "live" : "released", live && tp.down ? " (finger down)" : "",
                static_cast<double>(u), static_cast<double>(v));
    }
}

void install(const FeatureInstall& in) {
    const uint64_t base = in.base;
    g.base = base;
    g.host = in.host;
    g.cam_on = settings::get_bool(settings::Key::CameraAim);
    g.touch_on = settings::get_bool(settings::Key::TouchAim);
    g.extra = static_cast<int>(settings::get_int(settings::Key::CameraAimReach));
    g.reach = std::max(kReach, 1 + g.extra);
    g.touch_reach = static_cast<int>(std::clamp<int64_t>(settings::get_int(settings::Key::TouchAimReach), 1, 8));
    const double hold = std::clamp(settings::get_double(settings::Key::TouchAimHold), 0.0, 10.0);
    g.touch_hold_ns = static_cast<uint64_t>(hold * 1e9);
    // Where the finger comes from: SDL's touch events always; on a handheld (or touch_aim_pointer = on) the plain pointer too, since
    // SteamOS's game mode hands a touch to the window as a mouse. Play mode then leaves the pointer free in play (GameHost::set_finger_pointer).
    const std::string pm = settings::get_str(settings::Key::TouchAimPointer);
    const bool finger_pointer = g.touch_on && (pm == "on" || (pm != "off" && host_is_handheld()));
    if (g.host) g.host->set_finger_pointer(finger_pointer);
    if (!g.cam_on && !g.touch_on) {
        NXLOG_I(kTag, "camera aim: off; touch aim: off");
        return;
    }
    in.hooks.add_mid(base + kSwingPreview, &hook_swing_preview);
    in.hooks.add_mid(base + kSwingLive, &hook_swing_live);
    in.hooks.add_mid(base + kActionStart, &hook_action_start);
    in.hooks.add_mid(base + kHeadingRead, &hook_heading);
    in.hooks.add_mid(base + kPitchRead, &hook_pitch);
    // the swing positions: the reach's column move, else turned to the camera (turn_shape), with or without extra reach
    in.hooks.add_mid(base + kSwingPreviewPos, &hook_swing_preview_pos);
    in.hooks.add_mid(base + kSwingLivePos, &hook_swing_live_pos);
    in.hooks.add_mid(base + kQueryBase, &hook_query_base);
    if (g.cam_on)
        NXLOG_I(kTag, "camera aim: on (hooks main+0x%llx, +0x%llx, +0x%llx, +0x%llx, +0x%llx), reach +%d cells",
                static_cast<unsigned long long>(kHeadingRead), static_cast<unsigned long long>(kSwingPreview),
                static_cast<unsigned long long>(kSwingLive), static_cast<unsigned long long>(kPitchRead),
                static_cast<unsigned long long>(kActionStart), g.extra);
    else
        NXLOG_I(kTag, "camera aim: off (the hooks serve touch aim)");
    if (g.touch_on)
        NXLOG_I(kTag, "touch aim: on, reach %d cells, hold %.1f s after the finger lifts, the finger is %s (the finger aims through the render view, the buttons act)",
                g.touch_reach, hold, finger_pointer ? "the touch events and the free pointer (touch_aim_pointer = on, or auto on a handheld)" : "the touch events");
    else
        NXLOG_I(kTag, "touch aim: off");
}

}  // namespace

const Feature kAimFeature = {.name = "aim", .install = &install, .frame = &frame};

}  // namespace nxrt::games::pkp
