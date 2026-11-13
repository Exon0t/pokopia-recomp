// SPDX-License-Identifier: GPL-3.0-or-later
// acc:u0 (nn::account::IAccountServiceForApplication) and acc:u1 (the same answers for the system side), with the interfaces they
// return: IProfile (nn::account::profile), IManagerForApplication (nn::account::baas) and IAsyncContext (nn::account::detail).
// The console has one user, the [account] user of the profile (services::profile()), which is open from the start. It has a network
// service account as far as the local calls go (CheckAvailability, GetAccountId, the id token cache succeed); everything that would
// reach Nintendo's servers still fails (nifm reports no internet, the licence context fails).
//
// The network service account id (GetAccountId): [account] network_account_id (16 hex digits, nonzero), else a hash of the Windows
// MachineGuid (HKLM\SOFTWARE\Microsoft\Cryptography; per prefix under Wine/Proton) and the uid, else (registry unreadable) the old
// uid[0] ^ uid[1]. It must differ between the players of a local session even when both keep the default uid: Pokopia's KTGL session
// layer builds its member id from it and the host refuses a joining member whose id it already has (error 9005,
// work/mp/join-reject-analysis.md).
// SDK 20.x (sdk image, read for this): nn::account::EnsureNetworkServiceAccountAvailable (sdk 0x189ca0) calls IManagerForApplication
// cmd 0 CheckAvailability and returns at once when it succeeds; only a 124-200..269 failure starts the player select applet in mode 2.
// nn::account::GetNetworkServiceAccountId (sdk 0x189de0) is cmd 1 GetAccountId (a tail call, no other command).
//
// The id token answers follow the yuzu family (Lichii / Eglantine, GPL-3.0, src/core/hle/service/acc/acc.cpp, where Pokopia's Link
// Play works): EnsureIdTokenCacheAsync succeeds with a context that is already done, LoadIdTokenCache answers a zero-filled token.
//
// Adapted from ScarletNX src/nx/hle/services/acc.c (GPL-3.0-or-later): the command table (acc.swipc), the one-local-user answers,
// the profile timestamp (1546300800, 2019-01-01 UTC), the IManagerForApplication GetAccountId formula (uid[0] ^ uid[1]) and the
// IAsyncContext that is done at once and fails with the nifm result (110-300) because there is no network.
// Adapted from Ryujinx src/Ryujinx.HLE/HOS/Services/Account/Acc (MIT): IsUserRegistrationRequestPermitted per service (false on
// acc:u0, true on acc:u1), the IProfile Get/GetBase layout, and the NullArgument check of user ids.
//
// Uid bytes (nn::account::Uid is 0x10 bytes, two u64; Switchbrew names no order): the first u64 in memory is user_uid_hi, the
// second is user_uid_lo, both little-endian. Evidence: ScarletNX writes uid[0] first and prints it as the first 16 hex digits;
// Ryujinx's UserId writes High first and prints High then Low; libnx's AccountUid is uid[2] with uid[0] first. The hex form of
// [account] uid is hi then lo (services/profile), so the bytes follow it. services_api.h's "low word first in memory" note is not
// what these sources say; it is left as it is (not this file).
//
// Result codes (module 124 = acc, Switchbrew "Error codes"): 124-20 ResultInvalidUserId (0x287C), 124-32 ResultInvalidArrayLength
// (0x407C), 124-100 ResultUserNotExist (0xC87C). Module 110 (nifm) 300 comes from ScarletNX's comment; Switchbrew's page does not
// list it (checked), so that code is unconfirmed.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <set>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/services.h"
#include "nxrt/services_api.h"
#include "services_internal.h"
#include "network_cloud.h"

namespace nxrt::services {

namespace {

constexpr uint64_t kProfileLastEdit = 1546300800ull;   // ProfileBase.last_edit_timestamp: 2019-01-01 00:00 UTC (ScarletNX)
constexpr uint64_t kThumbnailSize = 0x24000;           // StoreSaveDataThumbnail's image: 256x144 RGBA (ScarletNX ACC_THUMBNAIL_SIZE)
constexpr uint64_t kUserListBytes = 0xA0;              // the user array of the list commands: array<Uid, 0xa> (Switchbrew)
constexpr uint64_t kIdTokenBytes = 0x100;              // LoadIdTokenCache's token: zero-filled, this long (yuzu family)

struct Uid {
    uint64_t hi = 0, lo = 0;
    bool null() const { return (hi | lo) == 0; }
    bool operator==(const Uid& o) const { return hi == o.hi && lo == o.lo; }
};

Uid local_uid() {
    const TitleProfile& p = profile();
    return Uid{p.user_uid_hi, p.user_uid_lo};
}

// The users this process has open. The console opens its user at boot (Ryujinx opens the default user in its AccountManager
// constructor). No command of the acc:u0 / acc:u1 table below changes the set, so it stays at that one user until acc_reset().
struct AccState {
    std::mutex mu;
    std::vector<Uid> opened;
    bool nsa_ready = false;   // nsa_id is computed at the first GetAccountId after acc_reset()
    uint64_t nsa_id = 0;
};
AccState& state() {
    static AccState s;
    return s;
}

// First use of a command, for the Info log lines below (one line per key for the life of the process; acc_reset() clears it). The keys
// are the command id for acc:u0 / acc:u1 and 0x10000 + id for IManagerForApplication. The game's nn::account calls are lifted SDK code,
// so these IPC lines are the only trace of which account answers (the network service account id) the game was given; NXRT_HLE_TRACE
// does not see them. Commands nobody answers are logged by the IPC layer (Warn "<service> cmd N not implemented", once per command).
constexpr uint32_t kMgrKey = 0x10000;
std::mutex g_seen_mu;
std::set<uint32_t> g_seen;
bool first_use(uint32_t key) {
    std::lock_guard<std::mutex> lk(g_seen_mu);
    return g_seen.insert(key).second;
}

// HKLM\SOFTWARE\Microsoft\Cryptography MachineGuid (the 64-bit view), empty when it cannot be read.
std::string machine_guid() {
#ifdef _WIN32
    char buf[128] = {};
    DWORD n = sizeof buf;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", "MachineGuid", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr,
                     buf, &n) == ERROR_SUCCESS)
        return std::string(buf, strnlen(buf, sizeof buf));
#endif
    return {};
}

// Exactly 16 hex digits (surrounding blanks allowed), else false.
bool parse_id(const std::string& s, uint64_t* out) {
    size_t b = s.find_first_not_of(" \t"), e = s.find_last_not_of(" \t");
    if (b == std::string::npos || e - b + 1 != 16) return false;
    uint64_t v = 0;
    for (size_t i = b; i <= e; ++i) {
        const char c = s[i];
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return false;
        v = v << 4 | static_cast<uint64_t>(d);
    }
    *out = v;
    return true;
}

// FNV-1a 64 over the MachineGuid text and the uid's 16 bytes, then the splitmix64 finalizer; never 0.
uint64_t hash_id(const std::string& guid, const Uid& u) {
    uint64_t h = 0xcbf29ce484222325ull;
    auto feed = [&h](const void* p, size_t n) {
        for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const uint8_t*>(p)[i]) * 0x100000001b3ull;
    };
    feed(guid.data(), guid.size());
    feed(&u.hi, 8);
    feed(&u.lo, 8);
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ull;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebull;
    h ^= h >> 31;
    return h ? h : 1;
}

// The network service account id GetAccountId answers (see the top of the file). Logged at Info when it is worked out.
uint64_t network_account_id(const Uid& u) {
    std::lock_guard<std::mutex> lk(state().mu);
    AccState& st = state();
    if (st.nsa_ready) return st.nsa_id;
    const char* from = nullptr;
    const std::string cfg = config_get_str("account", "network_account_id", "");
    uint64_t id = 0;
    if (!cfg.empty() && parse_id(cfg, &id) && id != 0) {
        from = config_source("account", "network_account_id");
    } else {
        if (!cfg.empty())
            NXLOG_W("acc", "[account] network_account_id = \"%s\" is not 16 hex digits (nonzero): using the default", cfg.c_str());
        const std::string guid = machine_guid();
        if (!guid.empty()) {
            id = hash_id(guid, u);
            from = "hash of MachineGuid and uid";
        } else {
            id = u.hi ^ u.lo;
            from = "uid hi ^ lo (MachineGuid not readable)";
        }
    }
    st.nsa_id = id;
    st.nsa_ready = true;
    NXLOG_I("acc", "network service account id 0x%016llx (%s)", static_cast<unsigned long long>(id), from);
    return id;
}

// Only one user exists: the local one.
bool user_exists(const Uid& u) { return !u.null() && u == local_uid(); }

// Writes a Uid as 16 bytes: hi then lo, each little-endian (the host and the guest are little-endian x64).
void put_uid(uint8_t* p, const Uid& u) {
    std::memcpy(p, &u.hi, 8);
    std::memcpy(p + 8, &u.lo, 8);
}
void push_uid(ipc::Response& rsp, const Uid& u) {
    rsp.push<uint64_t>(u.hi);
    rsp.push<uint64_t>(u.lo);
}
// A Uid argument at `off` of the raw request data.
Uid arg_uid(const ipc::Request& req, size_t off) { return Uid{req.arg<uint64_t>(off), req.arg<uint64_t>(off + 8)}; }

// Fills the user array the guest gave (zeros after the users) with `users`, up to what the buffer holds.
void write_user_list(const ipc::Request& req, const std::vector<Uid>& users) {
    const ipc::Buffer out = req.out_buf(0);
    if (!out.addr) return;
    const uint64_t n = std::min<uint64_t>(out.size, kUserListBytes);
    std::vector<uint8_t> bytes(static_cast<size_t>(n), 0);
    for (size_t i = 0; i < users.size() && (i + 1) * 16 <= bytes.size(); ++i) put_uid(bytes.data() + i * 16, users[i]);
    buf_write(out, bytes.data(), bytes.size());
}

// nn::account::profile::ProfileBase (0x38 bytes): uid (0x10), last edit timestamp (u64 at 0x10), nickname (0x20 at 0x18, NUL padded).
void push_profile_base(ipc::Response& rsp, const Uid& u) {
    uint8_t b[0x38] = {};
    put_uid(b, u);
    std::memcpy(b + 0x10, &kProfileLastEdit, 8);
    const std::string& nick = profile().user_nickname;
    std::memcpy(b + 0x18, nick.data(), std::min<size_t>(nick.size(), 0x20));
    rsp.out.insert(rsp.out.end(), b, b + sizeof b);
}

// IProfile: Get (0), GetBase (1), GetImageSize (10), LoadImage (11). The picture is empty (size 0): Ryujinx embeds a stock JPEG and
// ScarletNX draws one, this file has neither, so a game that reserves GetImageSize bytes gets none.
std::shared_ptr<ipc::Service> make_profile(const Uid& u) {
    return make_iface("IProfile", [u](ipc::Service& s) {
        s.add_cmd(0, "Get", [u](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            // UserData (0x80 bytes) into the guest's buffer: all zeros, which is the default icon with no Mii.
            const std::vector<uint8_t> user_data(0x80, 0);
            buf_write(req.out_buf(0), user_data.data(), user_data.size());
            push_profile_base(rsp, u);
            return 0;
        });
        s.add_cmd(1, "GetBase", [u](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            push_profile_base(rsp, u);
            return 0;
        });
        s.add_cmd(10, "GetImageSize", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(0);
            return 0;
        });
        s.add_cmd(11, "LoadImage", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(0);
            return 0;
        });
    });
}

// IAsyncContext: an operation that is done at once (HasDone 1, the event signalled) with GetResult `result`: 0 for the id token cache
// (yuzu family), nifm's 110-300 for what needs the network (ScarletNX).
std::shared_ptr<ipc::Service> make_async_context(uint32_t result) {
    auto done = std::make_shared<ServiceEvent>(true);
    return make_iface("IAsyncContext", [done, result](ipc::Service& s) {
        s.add_cmd(0, "GetSystemEvent", [done](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(done->new_handle());
            return 0;
        });
        s.add_cmd(1, "Cancel", [](ipc::Request&, ipc::Response&) -> uint32_t { return 0; });
        s.add_cmd(2, "HasDone", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint8_t>(1);
            return 0;
        });
        s.add_cmd(3, "GetResult", [result](ipc::Request&, ipc::Response&) -> uint32_t { return result; });
    });
}

// IAsyncNetworkServiceLicenseKindContext (IAsyncContextForLoginForOnlinePlay): the IAsyncContext above plus cmd 100, the licence kind.
// nn::account::AsyncNetworkServiceLicenseInfoContext::GetNetworkServiceLicenseKind aborts when cmd 100 fails (sdk 0x750018c634), so it
// answers 0 (no licence) even though GetResult has already failed.
std::shared_ptr<ipc::Service> make_license_async_context() {
    // Cloud Island proof of concept ([net] cloud_server): the licence check succeeds with kind 1 (a subscriber).
    const bool cloud_on = netsvc::cloud::enabled();
    std::shared_ptr<ipc::Service> s = make_async_context(cloud_on ? 0 : res(110, 300));
    s->add_cmd(100, "GetNetworkServiceLicenseKind", [cloud_on](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint32_t>(cloud_on ? 1 : 0);
        return 0;
    });
    return s;
}

// nn::account::baas::IManagerForApplication: CheckAvailability (0), GetAccountId (1), EnsureIdTokenCacheAsync (2), LoadIdTokenCache (3, 4),
// LoadNetworkServiceLicenseKindAsync (170). GetAccountId answers network_account_id(); the id token cache succeeds (yuzu family); the
// licence context fails (no network). Every command logs its first use at Info with what it answered (the account id in hex): a game's
// online/local session layer may build its player id from it (Pokopia's KTGL member id, see work/mp/join-reject-analysis.md), so two PCs
// must be seen to get different ids.
std::shared_ptr<ipc::Service> make_manager(const Uid& u) {
    return make_iface("IManagerForApplication", [u](ipc::Service& s) {
        s.add_cmd(0, "CheckAvailability", [u](ipc::Request&, ipc::Response&) -> uint32_t {
            if (first_use(kMgrKey + 0))
                NXLOG_I("acc", "IManagerForApplication cmd 0 CheckAvailability: user %016llx_%016llx -> 0 (available) (first use)",
                        static_cast<unsigned long long>(u.hi), static_cast<unsigned long long>(u.lo));
            return 0;
        });
        s.add_cmd(1, "GetAccountId", [u](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            const uint64_t id = network_account_id(u);
            if (first_use(kMgrKey + 1))
                NXLOG_I("acc", "IManagerForApplication cmd 1 GetAccountId: user %016llx_%016llx -> account id 0x%016llx (first use)",
                        static_cast<unsigned long long>(u.hi), static_cast<unsigned long long>(u.lo), static_cast<unsigned long long>(id));
            rsp.push<uint64_t>(id);
            return 0;
        });
        s.add_cmd(2, "EnsureIdTokenCacheAsync", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            if (first_use(kMgrKey + 2))
                NXLOG_I("acc", "IManagerForApplication cmd 2 EnsureIdTokenCacheAsync -> IAsyncContext (done, GetResult 0) (first use)");
            rsp.push_object(make_async_context(0));
            return 0;
        });
        // LoadIdTokenCache (3: the deprecated one on 19.0.0+, 4: the current one; nn::account::LoadNetworkServiceAccountIdTokenCache uses
        // vtable slot 0x40 = cmd 4): the token into the output buffer, u32 its size. A zero-filled 0x100-byte token (yuzu family), cut to
        // the buffer. It used to be 124-200 NetworkServiceAccountRegistrationRequired.
        for (uint32_t id : {3u, 4u}) {
            const char* name = id == 3 ? "LoadIdTokenCacheDeprecated" : "LoadIdTokenCache";
            s.add_cmd(id, name, [id, name, u](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
                const ipc::Buffer out = req.out_buf(0);
                if (netsvc::cloud::enabled()) {
                    // an unsigned token (alg none) whose subject is this PC's network service account id: a private server that
                    // accepts emulator identities takes the account from it. Not a Nintendo credential.
                    static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
                    auto enc = [](const std::string& in) {
                        std::string o;
                        for (size_t i = 0; i < in.size(); i += 3) {
                            const uint32_t v = (static_cast<uint8_t>(in[i]) << 16) | (i + 1 < in.size() ? static_cast<uint8_t>(in[i + 1]) << 8 : 0) |
                                               (i + 2 < in.size() ? static_cast<uint8_t>(in[i + 2]) : 0);
                            o.push_back(b64[v >> 18]);
                            o.push_back(b64[(v >> 12) & 63]);
                            if (i + 1 < in.size()) o.push_back(b64[(v >> 6) & 63]);
                            if (i + 2 < in.size()) o.push_back(b64[v & 63]);
                        }
                        return o;
                    };
                    char sub[32];
                    std::snprintf(sub, sizeof sub, "%016llx", static_cast<unsigned long long>(network_account_id(u)));
                    const std::string tok = enc("{\"alg\":\"none\",\"typ\":\"JWT\"}") + "." + enc(std::string("{\"sub\":\"") + sub + "\"}") + ".";
                    const uint64_t tn = out.addr ? std::min<uint64_t>(out.size, tok.size()) : 0;
                    if (tn) buf_write(out, tok.data(), tn);
                    if (first_use(kMgrKey + id + 0x100))
                        NXLOG_I("acc", "IManagerForApplication cmd %u %s -> unsigned token for account %s (cloud, first use)", id, name, sub);
                    rsp.push<uint32_t>(static_cast<uint32_t>(tn));
                    return 0;
                }
                const uint64_t n = out.addr ? std::min<uint64_t>(out.size, kIdTokenBytes) : 0;
                if (n) {
                    const std::vector<uint8_t> zeros(static_cast<size_t>(n), 0);
                    buf_write(out, zeros.data(), zeros.size());
                }
                if (first_use(kMgrKey + id))
                    NXLOG_I("acc", "IManagerForApplication cmd %u %s -> 0, zero token of %llu bytes (first use)", id, name,
                            static_cast<unsigned long long>(n));
                rsp.push<uint32_t>(static_cast<uint32_t>(n));
                return 0;
            });
        }
        // 170 LoadNetworkServiceLicenseKindAsync / EnsureIdTokenCacheForOnlinePlayAsync (nn::account::LoadNetworkServiceLicenseInfoAsync,
        // slot 0x60): an async context that is done and failed like the others, so the game reads "no Switch Online licence".
        s.add_cmd(170, "LoadNetworkServiceLicenseKindAsync", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            if (first_use(kMgrKey + 170))
                NXLOG_I("acc", "IManagerForApplication cmd 170 LoadNetworkServiceLicenseKindAsync -> IAsyncContext (done, GetResult 110-300, "
                        "licence kind 0) (first use)");
            rsp.push_object(make_license_async_context());
            return 0;
        });
    });
}

// Commands both acc:u0 and acc:u1 answer with the same ids and meaning in Switchbrew's tables.
void add_shared(ipc::Service& s, bool system_side) {
    s.add_cmd(0, "GetUserCount", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<int32_t>(1);
        return 0;
    });
    s.add_cmd(1, "GetUserExistence", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint8_t>(user_exists(arg_uid(req, 0)) ? 1 : 0);
        return 0;
    });
    s.add_cmd(2, "ListAllUsers", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        write_user_list(req, {local_uid()});
        return 0;
    });
    s.add_cmd(3, "ListOpenUsers", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        std::vector<Uid> open;
        {
            std::lock_guard<std::mutex> lk(state().mu);
            open = state().opened;
        }
        write_user_list(req, open);
        return 0;
    });
    s.add_cmd(4, "GetLastOpenedUser", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        push_uid(rsp, local_uid());
        return 0;
    });
    s.add_cmd(5, "GetProfile", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        const Uid u = arg_uid(req, 0);
        if (u.null()) return res(124, 20);
        if (!user_exists(u)) return res(124, 100);
        rsp.push_object(make_profile(u));
        return 0;
    });
    s.add_cmd(50, "IsUserRegistrationRequestPermitted", [system_side](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint8_t>(system_side ? 1 : 0);   // the application may not register users; the system service may (Ryujinx)
        return 0;
    });
    // TrySelectUserWithoutInteraction: with one user, that user. The network-account flag is only logged (ScarletNX and Ryujinx).
    auto try_select = [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        if (req.arg<uint8_t>(0)) NXLOG_D("acc", "TrySelectUserWithoutInteraction: network service account required (not checked)");
        push_uid(rsp, local_uid());
        return 0;
    };
    s.add_cmd(51, "TrySelectUserWithoutInteractionDeprecated", try_select);
    s.add_cmd(52, "TrySelectUserWithoutInteraction", try_select);
    s.add_cmd(110, "StoreSaveDataThumbnail", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        if (arg_uid(req, 0).null()) return res(124, 20);
        if (req.in_buf(0).size != kThumbnailSize) return res(124, 32);
        return 0;   // the thumbnail is not kept (ScarletNX)
    });
    s.add_cmd(111, "ClearSaveDataThumbnail", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        if (arg_uid(req, 0).null()) return res(124, 20);
        return 0;
    });
}

// GetBaasAccountManagerForApplication (101) and LoadOpenContext (130): an IManagerForApplication for a known user. The first use of
// each logs the user asked for and the answer.
uint32_t get_baas_manager(ipc::Request& req, ipc::Response& rsp) {
    const Uid u = arg_uid(req, 0);
    const uint32_t r = u.null() ? res(124, 20) : !user_exists(u) ? res(124, 100) : 0;
    if (first_use(req.cmd))
        NXLOG_I("acc", "cmd %u %s: user %016llx_%016llx -> 0x%x (first use)", req.cmd,
                req.cmd == 101 ? "GetBaasAccountManagerForApplication" : "LoadOpenContext", static_cast<unsigned long long>(u.hi),
                static_cast<unsigned long long>(u.lo), r);
    if (r) return r;
    rsp.push_object(make_manager(u));
    return 0;
}

void setup_app(ipc::Service& s) {
    add_shared(s, false);
    // Accepted and ignored: the application's init call (InitializeApplicationInfo / V0 / V2 take the pid, Switchbrew).
    auto accept = [](ipc::Request&, ipc::Response&) -> uint32_t {
        NXLOG_D("acc", "InitializeApplicationInfo: accepted, no state kept");
        return 0;
    };
    s.add_cmd(100, "InitializeApplicationInfoV0", accept);
    s.add_cmd(140, "InitializeApplicationInfo", accept);
    s.add_cmd(160, "InitializeApplicationInfoV2", accept);   // newer SDKs' init call (Switchbrew: same pid input)
    s.add_cmd(101, "GetBaasAccountManagerForApplication", get_baas_manager);
    s.add_cmd(130, "LoadOpenContext", get_baas_manager);
    s.add_cmd(131, "ListOpenContextStoredUsers", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        std::vector<Uid> open;
        {
            std::lock_guard<std::mutex> lk(state().mu);
            open = state().opened;
        }
        write_user_list(req, open);
        return 0;
    });
    s.add_cmd(141, "ListQualifiedUsers", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        write_user_list(req, {local_uid()});
        return 0;
    });
    s.add_cmd(150, "IsUserAccountSwitchLocked", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint8_t>(0);   // the NACP's switch lock is not read here (Ryujinx reads it from the title's NACP)
        return 0;
    });
}

void setup_sys(ipc::Service& s) {
    add_shared(s, true);
    // acc:u1 command 140 is ListQualifiedUsers (Switchbrew's acc:u1 table), not the application's InitializeApplicationInfo.
    s.add_cmd(140, "ListQualifiedUsers", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        write_user_list(req, {local_uid()});
        return 0;
    });
}

}  // namespace

void register_acc() {
    add("acc:u0", [] { return make_iface("acc:u0", setup_app); });
    add("acc:u1", [] { return make_iface("acc:u1", setup_sys); });
}

void acc_reset() {
    {
        std::lock_guard<std::mutex> lk(g_seen_mu);
        g_seen.clear();
    }
    std::lock_guard<std::mutex> lk(state().mu);
    state().opened.assign(1, local_uid());
    state().nsa_ready = false;   // the uid or [account] network_account_id may have changed
}

namespace accapi {
void user_uid(uint64_t* hi, uint64_t* lo) {
    const TitleProfile& p = profile();
    if (hi) *hi = p.user_uid_hi;
    if (lo) *lo = p.user_uid_lo;
}
}  // namespace accapi

}  // namespace nxrt::services
