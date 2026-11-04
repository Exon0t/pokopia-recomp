// SPDX-License-Identifier: GPL-3.0-or-later
// System tick and high-resolution sleep. Pattern from smo-recomp runtime/os/os_threads.c sleep_ns and SuperMarioWonderRecomp kernel.cpp system_tick.
#include <windows.h>

#include <string>

#include "nxrt/config.h"
#include "nxrt/kernel.h"
#include "nxrt/game.h"
#include "nxrt/log.h"
#include "nxrt/tt.h"
#include "kernel_internal.h"

namespace nxrt::kernel {

uint64_t tick_hz() {
    static const uint64_t hz = game::info().platform == game::Platform::Switch2 ? kTickHzSwitch2 : kTickHzSwitch1;
    return hz;
}

uint64_t system_tick() {
    if (uint64_t vt; tt::virtual_tick(&vt)) return vt;      // deterministic mode: the virtual clock (test tooling)
    static const uint64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<uint64_t>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const uint64_t v = static_cast<uint64_t>(c.QuadPart);
    const uint64_t hz = tick_hz();
    return (v / freq) * hz + (v % freq) * hz / freq;     // split so the multiplication cannot overflow
}

// SleepThread takes nanoseconds and the console timer is good to a few microseconds. Sleep() counts whole milliseconds on a
// 1 ms or 15.6 ms grid, so the wait runs on a high-resolution waitable timer (100 ns units); Sleep() is only the fallback.
// A zero or negative span yields, as on the console.
namespace {
// Short sleeps as real sleeps: under Wine, NtDelayExecution (not alertable) is a nanosleep on the host, good to tens of microseconds,
// so a 100 us poll costs no processor time, but wakes ~50 us late (Linux timer slack). On Windows it rounds up to the timer's grid
// (about half a millisecond or more). [kernel] short_sleep = auto (yield) | yield | sleep (only under Wine; Windows always yields).
// Measured in game on the PC, uncapped: yielding 231 fps on 4.1 cores, sleeping 198 fps on 3.8 cores. Where the frame waits on the
// GPU anyway (a Steam Deck) the lateness costs little and the processor time saved may be worth more: to be measured there.
// Pokopia polls its GPU fences with ~80 us SleepThread calls on two threads, thousands a second: yielded through, each keeps a
// processor core busy for as long as the GPU takes, which on a Steam Deck also takes power from the GPU (one power budget).
using NtDelayExecutionFn = LONG(WINAPI*)(BOOLEAN, PLARGE_INTEGER);
NtDelayExecutionFn short_sleep_fn() {
    static const NtDelayExecutionFn fn = [] () -> NtDelayExecutionFn {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        const bool wine = ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
        const std::string m = config_get_str("kernel", "short_sleep", "auto");
        const bool sleep = m == "sleep" && wine;
        auto f = sleep && ntdll ? reinterpret_cast<NtDelayExecutionFn>(GetProcAddress(ntdll, "NtDelayExecution")) : nullptr;
        NXLOG_I("kernel", "short sleeps (up to 200 us): %s ([kernel] short_sleep = %s%s)", f ? "slept (NtDelayExecution)" : "yielded through",
                m.c_str(), wine ? "" : ", Windows");
        return f;
    }();
    return fn;
}

void sleep_once(int64_t ns) {
    static thread_local HANDLE timer = nullptr;
    static thread_local bool no_timer = false;
    if (ns <= 0) {
        yield_host();
        return;
    }
    // Short sleeps (polls such as Pokopia's GPU-fence wait, 100 us a round) overshoot to about half a millisecond on the timer: yield until the
    // deadline instead. Longer ones keep the timer (no busy waiting while a thread idles).
    if (ns <= 200000) {
        if (NtDelayExecutionFn delay = short_sleep_fn()) {
            LARGE_INTEGER span;
            span.QuadPart = -((ns + 99) / 100);             // relative, in 100 ns units
            delay(FALSE, &span);
            return;
        }
        LARGE_INTEGER f, s, n;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&s);
        const int64_t ticks = ns * f.QuadPart / 1000000000;
        do {
            yield_host();
            QueryPerformanceCounter(&n);
        } while (n.QuadPart - s.QuadPart < ticks);
        return;
    }
    if (!timer && !no_timer) {
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) no_timer = true;
    }
    LARGE_INTEGER due;
    due.QuadPart = -((ns + 99) / 100);
    if (!timer || !SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
        Sleep(static_cast<DWORD>((ns + 999999) / 1000000));
    else
        WaitForSingleObject(timer, INFINITE);
}
}  // namespace

// On the thread set_blocked_hook names, a sleep longer than 10 ms is cut into 10 ms slices with the hook run between them, as in the kernel
// waits: Pokopia's main thread sleeps in SleepThread while the software keyboard applet is open (the freeze report's main stack).
void sleep_ns(int64_t ns) {
    if (tt::virtual_sleep(ns)) return;                      // deterministic mode: time advances, the thread only yields
    constexpr int64_t kSlice = 10000000;
    void (*fn)() = ns > kSlice ? detail::blocked_hook_here() : nullptr;
    if (!fn) return sleep_once(ns);
    const auto qpc_ns = [] {
        LARGE_INTEGER f, c;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&c);
        return static_cast<int64_t>(c.QuadPart / f.QuadPart * 1000000000 + c.QuadPart % f.QuadPart * 1000000000 / f.QuadPart);
    };
    const int64_t until = qpc_ns() + ns;
    for (int64_t rem = ns; rem > 0; rem = until - qpc_ns()) {
        if (rem <= kSlice) return sleep_once(rem);
        sleep_once(kSlice);
        fn();
    }
}

}  // namespace nxrt::kernel
