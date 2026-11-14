// SPDX-License-Identifier: GPL-3.0-or-later
// nsd, sfdnsres, ntc, ngct, ngc and notif: name resolution, network clock and text filter services, offline. See network.cpp.
#include "network_internal.h"

#include <atomic>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "../net/vnet.h"
#include "../net/vsock.h"
#include "coop.h"
#include "ldn.h"
#include "services_internal.h"
#include "network_cloud.h"

namespace nxrt::services::netsvc {

namespace {

// =============================================================================================================================================
// nsd: nsd:u, nsd:a (IManager). The environment is the retail one (lp1); Resolve returns the name as it was given.
// =============================================================================================================================================

}  // namespace

std::shared_ptr<ipc::Service> make_nsd() {
    return make_iface("nn::nsd::detail::IManager", [](ipc::Service& s) {
        cmd(s, "nsd", 11, "GetEnvironmentIdentifier", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            static const uint8_t kEnv[8] = {'l', 'p', '1', 0, 0, 0, 0, 0};
            buf_write(req.out_buf(0), kEnv, sizeof kEnv);
            return 0;
        });
        rows(s, "nsd", {
            {15, "SetChangeEnvironmentIdentifierDisabled", K::Ok},
            {100, "GetApplicationServerEnvironmentType", K::U8, 0},
        });
        // Resolve(fqdn_in) -> fqdn_out is the same bytes; ResolveEx answers the same with inner_result 0.
        auto copy_name = [](ipc::Request& req) { echo_in_to_out(req); };
        cmd(s, "nsd", 20, "Resolve", [copy_name](ipc::Request& req, ipc::Response&) -> uint32_t {
            copy_name(req);
            return 0;
        });
        cmd(s, "nsd", 21, "ResolveEx", [copy_name](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            copy_name(req);
            rsp.push(static_cast<uint32_t>(0));
            return 0;
        });
    });
}

namespace {

// =============================================================================================================================================
// sfdnsres: sfdnsres (IResolver). Every lookup fails: host not found (netdb 1, EAI_NONAME). Error text commands answer the glibc strings.
// =============================================================================================================================================

std::string netdb_text(int32_t e) {
    switch (e) {
        case 1: return "Unknown host";
        case 2: return "Temporary failure in name resolution";
        case 3: return "Non-recoverable failure in name resolution";
        case 4: return "No address associated with name";
        default: return "Unknown error";
    }
}

std::string gai_text(int32_t e) {
    switch (e) {
        case -1: return "Invalid value for ai_flags";
        case -2: return "Name or service not known";
        case -3: return "Temporary failure in name resolution";
        case -4: return "Non-recoverable failure in name resolution";
        case -5: return "No address associated with hostname";
        case -6: return "ai_family not supported";
        case -7: return "ai_socktype not supported";
        case -8: return "Servicename not supported for ai_socktype";
        case -9: return "Address family for hostname not supported";
        case -10: return "Memory allocation failure";
        case -11: return "System error";
        default: return "Unknown error";
    }
}

void write_text(const ipc::Buffer& out, const std::string& text) {
    std::vector<uint8_t> z(text.size() + 1, 0);
    std::memcpy(z.data(), text.data(), text.size());
    buf_write(out, z.data(), z.size());
}

}  // namespace

namespace {

// Cloud Island proof of concept: the NPLN names resolve to [net] cloud_server. Layouts after libnx nx/source/runtime/resolver.c (ISC).
void be32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x >> 24)); v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8)); v.push_back(static_cast<uint8_t>(x));
}

// The name in input buffer n when it is one of the cloud names, else "".
std::string cloud_name(const ipc::Request& req, size_t n) {
    if (!cloud::enabled()) return "";
    const std::string host = buf_text(req.in_buf(n));
    if (!cloud::is_cloud_name(host)) return "";
    NXLOG_D("net", "cloud: %s resolves to the cloud server", host.c_str());
    return host;
}

// One serialized addrinfo (AF_INET, SOCK_STREAM, TCP) for the cloud server, then the 0 sentinel. Every field is byte-swapped, the
// sockaddr's port and address a second time (so they sit little-endian), as the console's resolver does.
std::vector<uint8_t> cloud_addrinfo(uint16_t port) {
    std::vector<uint8_t> v;
    be32(v, 0xBEEFCAFE); be32(v, 0); be32(v, 2); be32(v, 1); be32(v, 6); be32(v, 16);
    const uint32_t ip = cloud::server_ip();
    const uint8_t sa[16] = {16, 2, static_cast<uint8_t>(port), static_cast<uint8_t>(port >> 8), static_cast<uint8_t>(ip),
                            static_cast<uint8_t>(ip >> 8), static_cast<uint8_t>(ip >> 16), static_cast<uint8_t>(ip >> 24)};
    v.insert(v.end(), sa, sa + 16);
    v.push_back(0);        // no canonical name
    be32(v, 0);            // sentinel
    return v;
}

// A serialized hostent: name, 0 aliases, AF_INET, 4-byte addresses, one address in network order.
std::vector<uint8_t> cloud_hostent(const std::string& host) {
    std::vector<uint8_t> v(host.begin(), host.end());
    v.push_back(0);
    be32(v, 0);
    v.push_back(0); v.push_back(2); v.push_back(0); v.push_back(4);
    be32(v, 1);
    be32(v, cloud::server_ip());
    return v;
}

// A dotted IPv4 literal ("127.0.0.1": the game resolves the STUN server the cloud server hands out this way) as a host-order address.
bool numeric_host(const ipc::Request& req, uint32_t* ip) {
    if (!cloud::enabled()) return false;
    const std::string h = buf_text(req.in_buf(0));
    uint32_t v = 0;
    int parts = 0, cur = -1;
    for (char c : h) {
        if (c >= '0' && c <= '9') {
            cur = (cur < 0 ? 0 : cur) * 10 + (c - '0');
            if (cur > 255) return false;
        } else if (c == '.' && cur >= 0 && parts < 3) {
            v = (v << 8) | static_cast<uint32_t>(cur);
            cur = -1;
            ++parts;
        } else {
            return false;
        }
    }
    if (parts != 3 || cur < 0) return false;
    *ip = (v << 8) | static_cast<uint32_t>(cur);
    return true;
}

// The addrinfo list of a numeric host: one datagram (UDP) entry and one stream (TCP) entry, then the 0 sentinel.
std::vector<uint8_t> numeric_addrinfo(uint32_t ip, uint16_t port) {
    std::vector<uint8_t> v;
    for (int k = 0; k < 2; ++k) {
        be32(v, 0xBEEFCAFE); be32(v, 0); be32(v, 2); be32(v, k == 0 ? 2 : 1); be32(v, k == 0 ? 17 : 6); be32(v, 16);
        const uint8_t sa[16] = {16, 2, static_cast<uint8_t>(port), static_cast<uint8_t>(port >> 8), static_cast<uint8_t>(ip),
                                static_cast<uint8_t>(ip >> 8), static_cast<uint8_t>(ip >> 16), static_cast<uint8_t>(ip >> 24)};
        v.insert(v.end(), sa, sa + 16);
        v.push_back(0);
    }
    be32(v, 0);
    return v;
}

uint16_t service_port(const ipc::Request& req) {
    const std::string s = buf_text(req.in_buf(1));
    int p = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return 0;
        p = p * 10 + (c - '0');
    }
    return static_cast<uint16_t>(p);
}

}  // namespace

std::shared_ptr<ipc::Service> make_resolver() {
    return make_iface("nn::socket::resolver::IResolver", [](ipc::Service& s) {
        // GetHostByNameRequest / GetHostByAddrRequest: (netdb_error, bsd_errno, size)
        cmd(s, "sfdnsres", 2, "GetHostByNameRequest", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            if (const std::string h = cloud_name(req, 0); !h.empty()) {
                const std::vector<uint8_t> v = cloud_hostent(h);
                rsp.push(static_cast<uint32_t>(0));
                rsp.push(static_cast<uint32_t>(0));
                rsp.push(static_cast<uint32_t>(buf_write(req.out_buf(0), v.data(), v.size())));
                return 0;
            }
            rsp.push(static_cast<uint32_t>(kNetdbHostNotFound));
            rsp.push(static_cast<uint32_t>(kBsdNetDown));
            rsp.push(static_cast<uint32_t>(0));
            return 0;
        });
        cmd(s, "sfdnsres", 3, "GetHostByAddrRequest", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push(static_cast<uint32_t>(kNetdbHostNotFound));
            rsp.push(static_cast<uint32_t>(kBsdNetDown));
            rsp.push(static_cast<uint32_t>(0));
            return 0;
        });
        cmd(s, "sfdnsres", 4, "GetHostStringErrorRequest", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            int32_t e = req.arg<int32_t>(0);
            write_text(req.out_buf(0), netdb_text(e));
            return 0;
        });
        cmd(s, "sfdnsres", 5, "GetGaiStringErrorRequest", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            int32_t e = req.arg<int32_t>(0);
            write_text(req.out_buf(0), gai_text(e));
            return 0;
        });
        // GetAddrInfoRequest: (bsd_errno, gai_error, size)
        cmd(s, "sfdnsres", 6, "GetAddrInfoRequest", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            ask_dns(req);
            if (!cloud_name(req, 0).empty()) {
                const std::vector<uint8_t> v = cloud_addrinfo(service_port(req));
                rsp.push(static_cast<uint32_t>(0));
                rsp.push(static_cast<int32_t>(0));
                rsp.push(static_cast<uint32_t>(buf_write(req.out_buf(0), v.data(), v.size())));
                return 0;
            }
            if (uint32_t nip = 0; numeric_host(req, &nip)) {
                const std::vector<uint8_t> v = numeric_addrinfo(nip, service_port(req));
                rsp.push(static_cast<uint32_t>(0));
                rsp.push(static_cast<int32_t>(0));
                rsp.push(static_cast<uint32_t>(buf_write(req.out_buf(0), v.data(), v.size())));
                return 0;
            }
            rsp.push(static_cast<uint32_t>(0));
            rsp.push(static_cast<int32_t>(kEaiNoName));
            rsp.push(static_cast<uint32_t>(0));
            return 0;
        });
        cmd(s, "sfdnsres", 8, "GetCancelHandleRequest", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push(alloc_cancel_handle());
            return 0;
        });
        rows(s, "sfdnsres", {
            {9, "CancelRequest", K::Ok},
        });
        // GetHostByNameRequestWithOptions / GetHostByAddrRequestWithOptions: (size, netdb_error, bsd_errno)
        cmd(s, "sfdnsres", 10, "GetHostByNameRequestWithOptions", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            if (const std::string h = cloud_name(req, 0); !h.empty()) {
                const std::vector<uint8_t> v = cloud_hostent(h);
                rsp.push(static_cast<uint32_t>(buf_write(req.out_buf(0), v.data(), v.size())));
                rsp.push(static_cast<uint32_t>(0));
                rsp.push(static_cast<uint32_t>(0));
                return 0;
            }
            rsp.push(static_cast<uint32_t>(0));
            rsp.push(static_cast<uint32_t>(kNetdbHostNotFound));
            rsp.push(static_cast<uint32_t>(kBsdNetDown));
            return 0;
        });
        cmd(s, "sfdnsres", 11, "GetHostByAddrRequestWithOptions", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push(static_cast<uint32_t>(0));
            rsp.push(static_cast<uint32_t>(kNetdbHostNotFound));
            rsp.push(static_cast<uint32_t>(kBsdNetDown));
            return 0;
        });
        // GetAddrInfoRequestWithOptions: (size, gai_error, netdb_error, bsd_errno)
        cmd(s, "sfdnsres", 12, "GetAddrInfoRequestWithOptions", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            ask_dns(req);
            if (!cloud_name(req, 0).empty()) {
                const std::vector<uint8_t> v = cloud_addrinfo(service_port(req));
                rsp.push(static_cast<uint32_t>(buf_write(req.out_buf(0), v.data(), v.size())));
                rsp.push(static_cast<int32_t>(0));
                rsp.push(static_cast<uint32_t>(0));
                rsp.push(static_cast<uint32_t>(0));
                return 0;
            }
            if (uint32_t nip = 0; numeric_host(req, &nip)) {
                const std::vector<uint8_t> v = numeric_addrinfo(nip, service_port(req));
                rsp.push(static_cast<uint32_t>(buf_write(req.out_buf(0), v.data(), v.size())));
                rsp.push(static_cast<int32_t>(0));
                rsp.push(static_cast<uint32_t>(0));
                rsp.push(static_cast<uint32_t>(0));
                return 0;
            }
            rsp.push(static_cast<uint32_t>(0));
            rsp.push(static_cast<int32_t>(kEaiNoName));
            rsp.push(static_cast<uint32_t>(0));
            rsp.push(static_cast<uint32_t>(0));
            return 0;
        });
        // ResolverSetOptionRequest: (i32 ret, u32 bsd_errno), accepted
        cmd(s, "sfdnsres", 14, "ResolverSetOptionRequest", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push(static_cast<int32_t>(0));
            rsp.push(static_cast<uint32_t>(0));
            return 0;
        });
    });
}

namespace {

// =============================================================================================================================================
// ntc: ntc (IStaticService) and IEnsureNetworkClockAvailabilityService. The network clock task fails at once with "network not available"
// (time 1000), or succeeds at once with [net] online_test = on; Cancel makes it "task canceled" (time 1003). The finish event is signalled when the task ends.
// =============================================================================================================================================

struct NtcTask {
    ServiceEvent finish;
    uint32_t result = 0;
};

std::shared_ptr<ipc::Service> make_ntc_task() {
    auto t = std::make_shared<NtcTask>();
    return make_iface("nn::ntc::detail::IEnsureNetworkClockAvailabilityService", [t](ipc::Service& s) {
        cmd(s, "ntc", 0, "StartTask", [t](ipc::Request&, ipc::Response&) -> uint32_t {
            t->result = online_test() ? 0 : kTimeNetworkNotAvailable;
            t->finish.signal();
            return t->result;
        });
        cmd(s, "ntc", 1, "GetFinishNotificationEvent", [t](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(t->finish.new_handle());
            return 0;
        });
        cmd(s, "ntc", 2, "GetResult", [t](ipc::Request&, ipc::Response&) -> uint32_t { return t->result; });
        cmd(s, "ntc", 3, "Cancel", [t](ipc::Request&, ipc::Response&) -> uint32_t {
            t->result = kTimeTaskCanceled;
            t->finish.signal();
            return 0;
        });
    });
}

}  // namespace

std::shared_ptr<ipc::Service> make_ntc_static() {
    return make_iface("nn::ntc::detail::IStaticService", [](ipc::Service& s) {
        cmd(s, "ntc", 0, "CreateEnsureNetworkClockAvailabilityService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_ntc_task());
            return 0;
        });
    });
}

namespace {

// =============================================================================================================================================
// ngct: ngct:u (IService). No word list: Match finds nothing, Filter returns the text as it was sent.
// =============================================================================================================================================

}  // namespace

std::shared_ptr<ipc::Service> make_ngct() {
    return make_iface("nn::ngct::detail::IService", [](ipc::Service& s) {
        rows(s, "ngct", {
            {0, "Match", K::U8, 0},               // b8 matched: false
        });
        cmd(s, "ngct", 1, "Filter", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            echo_in_to_out(req);
            return 0;
        });
    });
}

namespace {

// =============================================================================================================================================
// ngc: ngc:u (nn::ngc::detail::IService), the profanity filter behind nn::ngc::ProfanityFilter. No word list: Check finds nothing and
// Mask returns the text as it was sent, with no word masked. Pokopia's name check (the keyboard's text check) calls Mask (cmd 2, text in
// buffer A, masked text in buffer B) and refuses the name when the call fails: with the generic stub's error every name was "a word or
// character that isn't allowed" and the keyboard reopened forever (fork issue #6).
// =============================================================================================================================================

}  // namespace

std::shared_ptr<ipc::Service> make_ngc() {
    return make_iface("nn::ngc::detail::IService", [](ipc::Service& s) {
        rows(s, "ngc", {
            {0, "GetContentVersion", K::U32, 1},
            {1, "Check", K::U32, 0},              // u32 mask of the checks that matched: none
            {3, "Reload", K::Ok},
        });
        cmd(s, "ngc", 2, "Mask", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            echo_in_to_out(req);
            rsp.push(static_cast<uint32_t>(0));   // words masked
            return 0;
        });
    });
}

namespace {

// =============================================================================================================================================
// notif: notif:a (INotificationServicesForApplication). No alarms; Initialize accepted.
// =============================================================================================================================================

}  // namespace

std::shared_ptr<ipc::Service> make_notif() {
    return make_iface("nn::notification::INotificationServicesForApplication", [](ipc::Service& s) {
        rows(s, "notif", {
            {520, "ListAlarmSettings", K::S32, 0},
            {1000, "Initialize", K::Ok},
        });
    });
}

}  // namespace nxrt::services::netsvc
