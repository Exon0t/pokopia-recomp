// SPDX-License-Identifier: GPL-3.0-or-later
// The machine the runtime runs on (include/nxrt/host_device.h). Based on the Steam Deck detection in tinfoil-chewer's Linux fork
// (tinfoil-chewer/pokopia-recomp-linux 8b150a4), widened to the Legion Go S and to Windows handhelds.
#include "nxrt/host_device.h"

#include "nxrt/config.h"
#include "nxrt/log.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace nxrt {

namespace {

std::string lower_trimmed(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    s.erase(0, i);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// An SMBIOS string: HKLM\HARDWARE\DESCRIPTION\System\BIOS (Windows; Wine's wineboot fills it from SMBIOS), else the Linux DMI file
// through Wine's Z: drive (on Windows that path does not exist).
std::string smbios(const char* value, const char* dmi_file) {
    char buf[128] = {};
    DWORD n = sizeof buf;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\BIOS", value, RRF_RT_REG_SZ, nullptr, buf, &n) == ERROR_SUCCESS)
        return lower_trimmed(buf);
    std::ifstream f(std::string("Z:\\sys\\class\\dmi\\id\\") + dmi_file);
    std::string s;
    if (f && std::getline(f, s)) return lower_trimmed(s);
    return {};
}

bool detect(std::string* why) {
    const std::string forced = lower_trimmed(config_get_str("host", "handheld", "auto"));
    if (forced == "1" || forced == "on" || forced == "true" || forced == "yes") return *why = "[host] handheld = on", true;
    if (forced == "0" || forced == "off" || forced == "false" || forced == "no") return *why = "[host] handheld = off", false;
    char env[16] = {};
    const DWORD n = GetEnvironmentVariableA("SteamDeck", env, sizeof env);
    if (n > 0 && n < sizeof env && lower_trimmed(std::string(env, n)) == "1") return *why = "Steam's SteamDeck=1", true;
    const std::string product = smbios("SystemProductName", "product_name"), family = smbios("SystemFamily", "product_family");
    // Steam Deck LCD / OLED. The Go S's product names are SKU codes (83N6, ...), its family names the model: "Legion Go S 8APU1".
    if (product == "jupiter" || product == "galileo" || family.rfind("legion go s", 0) == 0) return *why = "board " + product + " / " + family, true;
    *why = "not a known handheld (board '" + product + "' / '" + family + "')";
    return false;
}

}  // namespace

bool host_is_handheld() {
    static const bool handheld = [] {
        std::string why;
        const bool h = detect(&why);
        NXLOG_I("host", "handheld: %s (%s)", h ? "yes" : "no", why.c_str());
        return h;
    }();
    return handheld;
}

// ---- Steam's on-screen keyboard -------------------------------------------------------------------------------------------------------

namespace {

std::string env_lower(const char* name) {
    char v[64] = {};
    const DWORD n = GetEnvironmentVariableA(name, v, sizeof v);
    return n > 0 && n < sizeof v ? lower_trimmed(std::string(v, n)) : std::string();
}

bool under_wine() {
    static const bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
    return wine;
}

bool file_exists(const wchar_t* path) {
    const DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Runs `exe "url"` and waits for it (up to 5 s): the commands reach Steam in the order they were asked.
bool run_with_url(const wchar_t* exe, const std::string& url) {
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" \"" + std::wstring(url.begin(), url.end()) + L"\"";   // the URL is ASCII
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// Hands a steam:// URL to the Steam client. Under Proton, the prefix's steam.exe is Proton's helper, which forwards steam:// arguments to
// the native client (steam_helper: steam-runtime-steam-remote, else steam); on Windows it is Steam itself. Plain Wine without Proton:
// winebrowser (xdg-open, which reaches Steam through its x-scheme-handler/steam). Last: ShellExecute.
void send_to_steam(const std::string& url) {
    static const wchar_t* const kSteamExe[] = {L"C:\\Program Files (x86)\\Steam\\steam.exe", L"C:\\windows\\system32\\steam.exe"};
    for (const wchar_t* exe : kSteamExe) {
        if (file_exists(exe) && run_with_url(exe, url)) {
            NXLOG_I("host", "Steam keyboard: %s (through %ls)", url.c_str(), exe);
            return;
        }
    }
    if (under_wine() && run_with_url(L"C:\\windows\\system32\\winebrowser.exe", url)) {
        NXLOG_I("host", "Steam keyboard: %s (through winebrowser)", url.c_str());
        return;
    }
    const INT_PTR r = reinterpret_cast<INT_PTR>(ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (r > 32) NXLOG_I("host", "Steam keyboard: %s (ShellExecute)", url.c_str());
    else NXLOG_W("host", "Steam keyboard: %s could not be handed to Steam (no steam.exe, ShellExecute %lld)", url.c_str(), static_cast<long long>(r));
}

struct SteamQueue {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::string> urls;
    bool started = false;
};
SteamQueue& steam_queue() {
    static SteamQueue* q = new SteamQueue;   // never destroyed: the worker may still run at exit
    return *q;
}

// One worker, in order: an open and the close right after it must not reach Steam swapped.
void queue_url(std::string url) {
    SteamQueue& q = steam_queue();
    std::lock_guard<std::mutex> lk(q.mu);
    q.urls.push_back(std::move(url));
    if (!q.started) {
        q.started = true;
        std::thread([] {
            SteamQueue& w = steam_queue();
            for (;;) {
                std::string u;
                {
                    std::unique_lock<std::mutex> l(w.mu);
                    w.cv.wait(l, [&] { return !w.urls.empty(); });
                    u = std::move(w.urls.front());
                    w.urls.pop_front();
                }
                send_to_steam(u);
            }
        }).detach();
    }
    q.cv.notify_one();
}

}  // namespace

bool host_steam_keyboard_wanted() {
    static const bool want = [] {
        const std::string c = lower_trimmed(config_get_str("applet", "steam_keyboard", "auto"));
        bool w = false;
        const char* why = "";
        if (c == "1" || c == "on" || c == "true" || c == "yes") {
            w = true;
            why = "[applet] steam_keyboard = on";
        } else if (c == "0" || c == "off" || c == "false" || c == "no") {
            why = "[applet] steam_keyboard = off";
        } else if (host_is_handheld()) {
            w = true;
            why = "a handheld ([host] handheld)";
        } else if (env_lower("SteamGamepadUI") == "1") {
            w = true;
            why = "Steam's gamepad UI (SteamGamepadUI=1)";
        } else if (env_lower("SteamOS") == "1") {
            w = true;
            why = "SteamOS (SteamOS=1)";
        } else {
            why = "not a handheld, SteamOS or Steam's gamepad UI";
        }
        NXLOG_I("host", "Steam keyboard for text the game asks for: %s (%s)", w ? "yes" : "no", why);
        return w;
    }();
    return want;
}

void host_steam_keyboard_open(int x, int y, int w, int h) {
    // Mode 0 = single line: Steam's Enter sends Return and closes its keyboard (SDL's X11 backend asks the same way).
    char url[160];
    std::snprintf(url, sizeof url, "steam://open/keyboard?XPosition=%d&YPosition=%d&Width=%d&Height=%d&Mode=0", std::max(0, x), std::max(0, y),
                  std::max(0, w), std::max(0, h));
    queue_url(url);
}

void host_steam_keyboard_close() { queue_url("steam://close/keyboard"); }

}  // namespace nxrt
