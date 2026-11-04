// SPDX-License-Identifier: GPL-3.0-or-later
// The machine the runtime runs on, where it changes a default (src/host_device.cpp).
#pragma once

namespace nxrt {

// A gaming handheld played with its own controls: a Steam Deck (LCD or OLED) or a Lenovo Legion Go S, under SteamOS/Proton or
// Windows. Detected from Steam's SteamDeck=1 in the environment or the board (SMBIOS product name Jupiter or Galileo, family
// "Legion Go S ..."), read from the registry's BIOS key (Windows, and Wine fills it from SMBIOS) or Wine's Z:\sys\class\dmi.
// [host] handheld = auto (default) | on | off overrides it. What it changes: [display] render_scale auto renders at the window's
// size instead of at least 1920x1080 (a handheld's GPU), and [prompts] style auto keeps controller prompts when the trackpad,
// touchscreen or on-screen keyboard send mouse or keyboard input.
bool host_is_handheld();

// Steam's on-screen keyboard for text the game asks for (the software keyboard applet). Wanted on a handheld (host_is_handheld), on
// SteamOS (SteamOS=1) and in Steam's gamepad UI (SteamGamepadUI=1); [applet] steam_keyboard = auto (default) | on | off overrides it.
bool host_steam_keyboard_wanted();
// Shows Steam's keyboard (steam://open/keyboard), keeping the text field at window pixels (x, y, w, h) uncovered; Steam types into the
// window like a real keyboard. Close hides it again (steam://close/keyboard). Both return at once: a worker hands the URLs to Steam in
// order (under Proton through the prefix's steam.exe, Proton's helper that forwards steam:// to the native client).
void host_steam_keyboard_open(int x, int y, int w, int h);
void host_steam_keyboard_close();

}  // namespace nxrt
