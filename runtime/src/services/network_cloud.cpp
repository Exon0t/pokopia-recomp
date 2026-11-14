// SPDX-License-Identifier: GPL-3.0-or-later
// Cloud Island proof of concept: the [net] cloud_* configuration and the host TCP socket table behind bsd and ssl. See network_cloud.h.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "network_cloud.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/dispatch.h"
#include "nxrt/module.h"
#include <atomic>
#include "../net/win_sock.h"

#pragma comment(lib, "ws2_32.lib")

namespace nxrt::services::netsvc::cloud {

namespace {

struct Cfg {
    bool enabled = false;
    uint32_t ip = 0;
    uint16_t gateway = 0;
};

const Cfg& cfg() {
    static const Cfg c = [] {
        Cfg r;
        const std::string s = config_get_str("net", "cloud_server", "");
        if (s.empty()) return r;
        in_addr a{};
        if (vnet::ensure_winsock() && inet_pton(AF_INET, s.c_str(), &a) == 1) {
            r.ip = ntohl(a.s_addr);
            r.enabled = true;
            r.gateway = static_cast<uint16_t>(config_get_int("net", "cloud_gateway_port", 18443));
            NXLOG_I("net", "[net] cloud_server = %s: the NPLN names resolve there, SOCK_STREAM sockets are host TCP, ssl passes bytes through"
                           " (guest port 443 -> 127.0.0.1:%u)", s.c_str(), r.gateway);
        } else {
            NXLOG_W("net", "[net] cloud_server = \"%s\" is not an IPv4 address: cloud islands stay off", s.c_str());
        }
        return r;
    }();
    return c;
}

struct Sock {
    SOCKET s = INVALID_SOCKET;        // the socket (an event descriptor: the read end of its loopback pair)
    SOCKET wake = INVALID_SOCKET;     // event descriptor only: the write end; one byte sits in the pair while the counter is not 0
    bool nonblock = false;
    bool event = false;
    bool semaphore = false;
    uint64_t counter = 0;
    std::mutex mu;                    // the counter and the wake byte
    int type = vsock::kSockStream;
    bool as_local = false;            // a datagram socket the game bound to this PC's address: bound to any address, named as asked
    std::map<uint64_t, int32_t> opts;   // (level << 32 | option) -> the last int set: read back by getsockopt (gRPC verifies what it set)
    ~Sock() {
        if (s != INVALID_SOCKET) closesocket(s);
        if (wake != INVALID_SOCKET) closesocket(wake);
    }
};

// A connected loopback TCP pair (Winsock has no socketpair).
bool loop_pair(SOCKET* rd, SOCKET* wr) {
    const SOCKET l = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (l == INVALID_SOCKET) return false;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = sizeof sa;
    bool ok = ::bind(l, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0 && ::listen(l, 1) == 0 &&
              ::getsockname(l, reinterpret_cast<sockaddr*>(&sa), &len) == 0;
    SOCKET c = INVALID_SOCKET, a = INVALID_SOCKET;
    if (ok) {
        c = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ok = c != INVALID_SOCKET && ::connect(c, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0;
    }
    if (ok) {
        a = ::accept(l, nullptr, nullptr);
        ok = a != INVALID_SOCKET;
    }
    closesocket(l);
    if (!ok) {
        if (c != INVALID_SOCKET) closesocket(c);
        return false;
    }
    *rd = a;
    *wr = c;
    return true;
}

struct Table {
    std::mutex mu;
    std::map<int, std::shared_ptr<Sock>> socks;
    int next = kFdBase;
};

Table& tbl() {
    static Table t;
    return t;
}

std::shared_ptr<Sock> get(int fd) {
    Table& t = tbl();
    std::lock_guard<std::mutex> lk(t.mu);
    auto it = t.socks.find(fd);
    return it == t.socks.end() ? nullptr : it->second;
}

// Winsock error -> Linux errno number (what bsd answers).
uint32_t lin(int wsa) {
    switch (wsa) {
        case 0: return 0;
        case WSAEWOULDBLOCK: return vsock::kEagain;
        case WSAEINPROGRESS: case WSAEALREADY: return 114;   // EALREADY
        case WSAECONNREFUSED: return 111;                     // ECONNREFUSED
        case WSAETIMEDOUT: return 110;                        // ETIMEDOUT
        case WSAECONNRESET: return 104;                       // ECONNRESET
        case WSAECONNABORTED: return 103;                     // ECONNABORTED
        case WSAEISCONN: return 106;                          // EISCONN
        case WSAENOTCONN: return vsock::kEnotconn;
        case WSAEADDRINUSE: return vsock::kEaddrinuse;
        case WSAEADDRNOTAVAIL: return vsock::kEaddrnotavail;
        case WSAENETUNREACH: return vsock::kEnetunreach;
        case WSAEHOSTUNREACH: return 113;                     // EHOSTUNREACH
        case WSAENETDOWN: return vsock::kEnetdown;
        case WSAEMSGSIZE: return vsock::kEmsgsize;
        case WSAEINVAL: return vsock::kEinval;
        case WSAEBADF: case WSAENOTSOCK: return vsock::kEbadf;
        case WSAEMFILE: return vsock::kEmfile;
        case WSAEACCES: return vsock::kEacces;
        case WSAEOPNOTSUPP: return vsock::kEopnotsupp;
        case WSAEAFNOSUPPORT: return vsock::kEafnosupport;
        case WSAEPROTONOSUPPORT: return vsock::kEprotonosupport;
        case WSAESHUTDOWN: return vsock::kEpipe;
        default: return vsock::kEinval;
    }
}

vsock::Ret fail(uint32_t e) { return {-1, e}; }
vsock::Ret wsafail() { return fail(lin(WSAGetLastError())); }

sockaddr_in to_sa(vsock::Addr a) {
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(a.port);
    sa.sin_addr.s_addr = htonl(a.ip);
    return sa;
}

vsock::Addr from_sa(const sockaddr_in& sa) { return {ntohl(sa.sin_addr.s_addr), ntohs(sa.sin_port)}; }

// FreeBSD poll bits (the Switch's) <-> Winsock's.
constexpr int16_t kPollIn = 1, kPollPri = 2, kPollOut = 4, kPollErr = 8, kPollHup = 0x10, kPollNval = 0x20;

SHORT to_wsa_events(int16_t e) {
    SHORT w = 0;
    if (e & (kPollIn | kPollPri)) w |= POLLRDNORM;
    if (e & kPollOut) w |= POLLWRNORM;
    return w;
}

int16_t from_wsa_events(SHORT w) {
    int16_t e = 0;
    if (w & (POLLRDNORM | POLLRDBAND)) e |= kPollIn;
    if (w & POLLPRI) e |= kPollPri;
    if (w & POLLWRNORM) e |= kPollOut;
    if (w & POLLERR) e |= kPollErr;
    if (w & POLLHUP) e |= kPollHup;
    if (w & POLLNVAL) e |= kPollNval;
    return e;
}

void set_nonblock(SOCKET s, bool on) {
    u_long v = on ? 1 : 0;
    ioctlsocket(s, FIONBIO, &v);
}

}  // namespace

namespace {
vsock::Ret add_fd(const std::shared_ptr<Sock>& sk);
}  // namespace

bool enabled() { return cfg().enabled; }
uint32_t server_ip() { return cfg().ip; }
uint16_t gateway_port() { return cfg().gateway; }

const std::vector<uint8_t>& ca_der() {
    static const std::vector<uint8_t> der = [] {
        std::vector<uint8_t> v;
        const std::string path = config_get_str("net", "cloud_ca", "");
        if (path.empty()) return v;
        if (FILE* f = std::fopen(path.c_str(), "rb")) {
            uint8_t buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) v.insert(v.end(), buf, buf + n);
            std::fclose(f);
        }
        NXLOG_I("net", "[net] cloud_ca = %s: %zu bytes", path.c_str(), v.size());
        return v;
    }();
    return der;
}

bool is_cloud_name(const std::string& host) {
    auto ends = [&](const char* suf) {
        const size_t n = std::strlen(suf);
        return host.size() >= n && host.compare(host.size() - n, n, suf) == 0;
    };
    return ends(".npln.nintendo.net") || host == "npln.nintendo.net" || ends(".npln.srv.nintendo.net") || host == "npln.srv.nintendo.net";
}

bool is_host_fd(int fd) { return fd >= kFdBase && get(fd) != nullptr; }

bool any_host_fd(const PollFd* fds, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (fds[i].fd >= kFdBase && is_host_fd(fds[i].fd)) return true;
    return false;
}

vsock::Ret socket(int domain, int type, int protocol) {
    (void)protocol;
    if (!enabled() || !vnet::ensure_winsock()) return fail(vsock::kEnetdown);
    if (domain != vsock::kAfInet) return fail(vsock::kEafnosupport);
    const int base = type & 0xFFFF;
    if (base != vsock::kSockStream && base != vsock::kSockDgram) return fail(vsock::kEprotonosupport);
    const bool dgram = base == vsock::kSockDgram;
    const SOCKET s = dgram ? ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP) : ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return wsafail();
    if (dgram) {   // Winsock reports an ICMP port-unreachable as WSAECONNRESET on the next receive: off, as on every other system
        DWORD off = 0, got = 0;
        WSAIoctl(s, 0x9800000C /* SIO_UDP_CONNRESET */, &off, sizeof off, nullptr, 0, &got, nullptr, nullptr);
    }
    auto sk = std::make_shared<Sock>();
    sk->s = s;
    sk->type = base;
    if (type & vsock::kSockNonblock) {
        set_nonblock(s, true);
        sk->nonblock = true;
    }
    const vsock::Ret r = add_fd(sk);
    if (!r.err) NXLOG_I("net", "cloud: host %s socket fd %d%s", dgram ? "UDP" : "TCP", r.ret, sk->nonblock ? " (non-blocking)" : "");
    return r;
}

uint32_t local_ip() {
    static const uint32_t ip = [] {
        uint32_t out = 0x7F000001u;
        if (!vnet::ensure_winsock()) return out;
        const SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) return out;
        sockaddr_in to = to_sa({0xC0000201u, 9});   // 192.0.2.1 (TEST-NET-1): connecting a UDP socket sends nothing, it only picks the route
        sockaddr_in me{};
        int len = sizeof me;
        if (::connect(s, reinterpret_cast<sockaddr*>(&to), sizeof to) == 0 && ::getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0 &&
            me.sin_addr.s_addr != 0)
            out = ntohl(me.sin_addr.s_addr);
        closesocket(s);
        NXLOG_I("net", "cloud: this PC's address for peers is %u.%u.%u.%u", out >> 24, (out >> 16) & 255, (out >> 8) & 255, out & 255);
        return out;
    }();
    return ip;
}

namespace {
// The first bytes of a datagram as hex, for the log.
std::string hex_head(const void* p, size_t n) {
    static const char* d = "0123456789abcdef";
    const uint8_t* b = static_cast<const uint8_t*>(p);
    std::string o;
    for (size_t i = 0; i < n && i < 160; ++i) {
        o.push_back(d[b[i] >> 4]);
        o.push_back(d[b[i] & 15]);
    }
    return o;
}
}  // namespace

bool is_dgram(int fd) {
    auto sk = get(fd);
    return sk && !sk->event && sk->type == vsock::kSockDgram;
}

vsock::Ret sendto(int fd, const void* p, size_t n, int flags, const vsock::Addr* to) {
    auto sk = get(fd);
    if (!sk || sk->event) return fail(vsock::kEbadf);
    const bool dontwait = (flags & vsock::kMsgDontwait) && !sk->nonblock;
    if (dontwait) set_nonblock(sk->s, true);
    int r;
    if (to) {
        sockaddr_in sa = to_sa(*to);
        r = ::sendto(sk->s, static_cast<const char*>(p), static_cast<int>(n), 0, reinterpret_cast<sockaddr*>(&sa), sizeof sa);
    } else {
        r = ::send(sk->s, static_cast<const char*>(p), static_cast<int>(n), 0);
    }
    const int e = r < 0 ? WSAGetLastError() : 0;
    if (dontwait) set_nonblock(sk->s, false);
    static int logged = 0;
    if (logged < 400) {
        ++logged;
        if (to) NXLOG_I("net", "cloud: udp fd %d sendto %u.%u.%u.%u:%u %zu bytes -> %d (Winsock %d) %s", fd, to->ip >> 24, (to->ip >> 16) & 255,
                        (to->ip >> 8) & 255, to->ip & 255, to->port, n, r, e, hex_head(p, n).c_str());
        else NXLOG_I("net", "cloud: udp fd %d send %zu bytes -> %d (Winsock %d) %s", fd, n, r, e, hex_head(p, n).c_str());
    }
    if (r < 0) return fail(lin(e));
    return {r, 0};
}

vsock::Ret recvfrom(int fd, void* p, size_t n, int flags, vsock::Addr* from) {
    auto sk = get(fd);
    if (!sk || sk->event) return fail(vsock::kEbadf);
    const bool dontwait = (flags & vsock::kMsgDontwait) && !sk->nonblock;
    if (dontwait) set_nonblock(sk->s, true);
    sockaddr_in sa{};
    int sl = sizeof sa;
    int r = ::recvfrom(sk->s, static_cast<char*>(p), static_cast<int>(n > 0x7fffffff ? 0x7fffffff : n), (flags & vsock::kMsgPeek) ? MSG_PEEK : 0,
                       reinterpret_cast<sockaddr*>(&sa), &sl);
    int e = r < 0 ? WSAGetLastError() : 0;
    if (r < 0 && e == WSAEMSGSIZE) {   // a datagram longer than the buffer: the buffer is full, the rest is dropped
        r = static_cast<int>(n);
        e = 0;
    }
    if (dontwait) set_nonblock(sk->s, false);
    if (r < 0) return fail(lin(e));
    if (from) *from = from_sa(sa);
    static int logged = 0;
    if (logged < 400) {
        ++logged;
        const vsock::Addr a = from_sa(sa);
        NXLOG_I("net", "cloud: udp fd %d recvfrom %u.%u.%u.%u:%u %d bytes %s", fd, a.ip >> 24, (a.ip >> 16) & 255, (a.ip >> 8) & 255, a.ip & 255,
                a.port, r, hex_head(p, static_cast<size_t>(r)).c_str());
    }
    return {r, 0};
}

namespace {
vsock::Ret add_fd(const std::shared_ptr<Sock>& sk) {
    Table& t = tbl();
    std::lock_guard<std::mutex> lk(t.mu);
    if (t.socks.size() >= 200) return fail(vsock::kEmfile);
    int fd = t.next;
    while (t.socks.count(fd)) fd = fd + 1 >= kFdBase + 0x100 ? kFdBase : fd + 1;
    t.next = fd + 1 >= kFdBase + 0x100 ? kFdBase : fd + 1;
    t.socks[fd] = sk;
    return {fd, 0};
}
}  // namespace

void install_fibers();

void patch_fiber_dispatch();

vsock::Ret eventfd(uint64_t init, int flags) {
    patch_fiber_dispatch();   // the SDK asks for its event descriptor right before it starts its fibers
    if (!enabled() || !vnet::ensure_winsock()) return fail(vsock::kEnetdown);
    auto sk = std::make_shared<Sock>();
    if (!loop_pair(&sk->s, &sk->wake)) return wsafail();
    sk->event = true;
    sk->semaphore = (flags & 1) != 0;
    sk->nonblock = (flags & 4) != 0 || (flags & vsock::kONonblock) != 0;
    sk->counter = init;
    if (init) ::send(sk->wake, "x", 1, 0);
    const vsock::Ret r = add_fd(sk);
    if (!r.err) NXLOG_I("net", "cloud: event descriptor fd %d (initial %llu, flags 0x%x)", r.ret, static_cast<unsigned long long>(init), flags);
    return r;
}

// The console's TLS service is about to run TLS on this socket, and the runtime has no TLS: swap the connection for one to the
// local gateway (127.0.0.1:[net] cloud_gateway_port), which does the TLS to the server. Nothing has been sent on it yet.
bool to_gateway(int fd) {
    auto sk = get(fd);
    if (!sk || sk->event || !gateway_port()) return false;
    const SOCKET n = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (n == INVALID_SOCKET) return false;
    sockaddr_in sa = to_sa({0x7F000001u, gateway_port()});
    if (::connect(n, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) {
        NXLOG_W("net", "cloud: no gateway on 127.0.0.1:%u (Winsock %d): the upload connection stays as it is", gateway_port(), WSAGetLastError());
        closesocket(n);
        return false;
    }
    const BOOL on = TRUE;
    ::setsockopt(n, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof on);
    if (sk->nonblock) set_nonblock(n, true);
    const SOCKET old = sk->s;
    sk->s = n;
    closesocket(old);
    NXLOG_I("net", "cloud: fd %d now goes through the local TLS gateway (port %u)", fd, gateway_port());
    return true;
}

int dup(int fd) {
    auto sk = get(fd);
    if (!sk) return -1;
    const vsock::Ret r = add_fd(sk);
    return r.err ? -1 : r.ret;
}

vsock::Ret connect(int fd, vsock::Addr a) {
    auto sk = get(fd);
    if (!sk) return fail(vsock::kEbadf);
    vsock::Addr target = a;
    const bool redirected = target.ip != a.ip || target.port != a.port;
    NXLOG_I("net", "cloud: connect fd %d -> %u.%u.%u.%u:%u%s", fd, a.ip >> 24, (a.ip >> 16) & 255, (a.ip >> 8) & 255, a.ip & 255, a.port,
            redirected ? " (through the local gateway)" : "");
    sockaddr_in sa = to_sa(target);
    if (::connect(sk->s, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0) return {0, 0};
    const int e = WSAGetLastError();
    if (e == WSAEWOULDBLOCK) return fail(115);   // EINPROGRESS: the game polls for writability, then reads SO_ERROR
    NXLOG_W("net", "cloud: connect fd %d failed (Winsock %d)", fd, e);
    return fail(lin(e));
}

vsock::Ret bind(int fd, vsock::Addr a) {
    auto sk = get(fd);
    if (!sk) return fail(vsock::kEbadf);
    NXLOG_I("net", "cloud: bind fd %d to %u.%u.%u.%u:%u", fd, a.ip >> 24, (a.ip >> 16) & 255, (a.ip >> 8) & 255, a.ip & 255, a.port);
    if (sk->type == vsock::kSockDgram && a.ip == local_ip() && a.ip != 0x7F000001u) {
        // Winsock cannot send from a socket bound to the LAN address to 127.0.0.1 (a STUN server on this PC): bind to any address.
        sk->as_local = true;
        a.ip = 0;
    }
    sockaddr_in sa = to_sa(a);
    if (::bind(sk->s, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) return wsafail();
    return {0, 0};
}

vsock::Ret send(int fd, const void* p, size_t n, int flags) {
    auto sk = get(fd);
    if (!sk) return fail(vsock::kEbadf);
    if (sk->event) {
        if (n < 8) return fail(vsock::kEinval);
        uint64_t add = 0;
        std::memcpy(&add, p, 8);
        std::lock_guard<std::mutex> lk(sk->mu);
        if (sk->counter == 0 && add) ::send(sk->wake, "x", 1, 0);
        sk->counter += add;
        return {8, 0};
    }
    const bool dontwait = (flags & vsock::kMsgDontwait) && !sk->nonblock;
    if (dontwait) set_nonblock(sk->s, true);
    const int r = ::send(sk->s, static_cast<const char*>(p), static_cast<int>(n > 0x7fffffff ? 0x7fffffff : n), 0);
    const int e = r < 0 ? WSAGetLastError() : 0;
    if (dontwait) set_nonblock(sk->s, false);
    if (r < 0) return fail(lin(e));
    return {r, 0};
}

vsock::Ret recv(int fd, void* p, size_t n, int flags) {
    auto sk = get(fd);
    if (!sk) return fail(vsock::kEbadf);
    if (sk->event) {
        if (n < 8) return fail(vsock::kEinval);
        for (;;) {
            {
                std::lock_guard<std::mutex> lk(sk->mu);
                if (sk->counter) {
                    const uint64_t v = sk->semaphore ? 1 : sk->counter;
                    sk->counter -= v;
                    if (sk->counter == 0) {
                        char c;
                        ::recv(sk->s, &c, 1, 0);
                    }
                    std::memcpy(p, &v, 8);
                    return {8, 0};
                }
            }
            if (sk->nonblock || (flags & vsock::kMsgDontwait)) return fail(vsock::kEagain);
            WSAPOLLFD pf{};
            pf.fd = sk->s;
            pf.events = POLLRDNORM;
            if (WSAPoll(&pf, 1, 1000) == SOCKET_ERROR) return wsafail();
        }
    }
    const bool dontwait = (flags & vsock::kMsgDontwait) && !sk->nonblock;
    if (dontwait) set_nonblock(sk->s, true);
    const int r = ::recv(sk->s, static_cast<char*>(p), static_cast<int>(n > 0x7fffffff ? 0x7fffffff : n), (flags & vsock::kMsgPeek) ? MSG_PEEK : 0);
    const int e = r < 0 ? WSAGetLastError() : 0;
    if (dontwait) set_nonblock(sk->s, false);
    if (r < 0) return fail(lin(e));
    return {r, 0};
}

vsock::Ret shutdown(int fd, int how) {
    auto sk = get(fd);
    if (!sk) return fail(vsock::kEbadf);
    const int h = how == vsock::kShutRd ? SD_RECEIVE : how == vsock::kShutWr ? SD_SEND : SD_BOTH;
    if (::shutdown(sk->s, h) != 0) return wsafail();
    return {0, 0};
}

vsock::Ret close(int fd) {
    Table& t = tbl();
    std::shared_ptr<Sock> sk;
    {
        std::lock_guard<std::mutex> lk(t.mu);
        auto it = t.socks.find(fd);
        if (it == t.socks.end()) return fail(vsock::kEbadf);
        sk = it->second;
        t.socks.erase(it);
    }
    NXLOG_I("net", "cloud: close fd %d", fd);   // the socket closes with its last descriptor (ssl keeps its own)
    return {0, 0};
}

vsock::Ret fcntl(int fd, int cmd, int arg) {
    auto sk = get(fd);
    if (!sk) return fail(vsock::kEbadf);
    NXLOG_I("net", "cloud: fcntl fd %d cmd %d arg 0x%x", fd, cmd, arg);
    if (cmd == vsock::kFGetFl) return {sk->nonblock ? (vsock::kONonblock | 4) : 0, 0};   // Linux's and FreeBSD's O_NONBLOCK bits
    if (cmd == vsock::kFSetFl) {
        const bool nb = (arg & vsock::kONonblock) || (arg & 4);
        if (!sk->event) set_nonblock(sk->s, nb);
        sk->nonblock = nb;
        return {0, 0};
    }
    return fail(vsock::kEinval);
}

vsock::Ret setsockopt(int fd, int level, int opt, const void* v, size_t n) {
    auto sk = get(fd);
    if (!sk) return fail(vsock::kEbadf);
    const uint8_t* b = static_cast<const uint8_t*>(v);
    {
        int32_t first = 0;
        if (n >= 4) std::memcpy(&first, b, 4);
        NXLOG_I("net", "cloud: setsockopt fd %d level 0x%x opt 0x%x len %zu value 0x%x", fd, level, opt, n, first);
        std::lock_guard<std::mutex> lk(sk->mu);
        sk->opts[(static_cast<uint64_t>(static_cast<uint32_t>(level)) << 32) | static_cast<uint32_t>(opt)] = first;
    }
    if (level == vsock::kSolSocket) {
        switch (opt) {
            case vsock::kSoSndTimeo:
            case vsock::kSoRcvTimeo: {
                // FreeBSD timeval {i64 sec, i64 usec}; Winsock wants milliseconds
                int64_t sec = 0, usec = 0;
                if (n >= 16) {
                    std::memcpy(&sec, b, 8);
                    std::memcpy(&usec, b + 8, 8);
                } else if (n >= 8) {
                    int32_t s32 = 0, u32v = 0;
                    std::memcpy(&s32, b, 4);
                    std::memcpy(&u32v, b + 4, 4);
                    sec = s32;
                    usec = u32v;
                }
                const DWORD ms = static_cast<DWORD>(sec * 1000 + usec / 1000);
                ::setsockopt(sk->s, SOL_SOCKET, opt == vsock::kSoSndTimeo ? SO_SNDTIMEO : SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
                return {0, 0};
            }
            case 0x80: {   // SO_LINGER {i32 onoff, i32 linger}
                linger lg{};
                if (n >= 8) {
                    int32_t on = 0, t = 0;
                    std::memcpy(&on, b, 4);
                    std::memcpy(&t, b + 4, 4);
                    lg.l_onoff = static_cast<u_short>(on != 0);
                    lg.l_linger = static_cast<u_short>(t);
                }
                ::setsockopt(sk->s, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&lg), sizeof lg);
                return {0, 0};
            }
            case vsock::kSoReuseAddr:
            case vsock::kSoSndBuf:
            case vsock::kSoRcvBuf:
            case 8: {   // SO_KEEPALIVE; the option numbers are the same in FreeBSD and Winsock
                int val = 0;
                if (n >= 4) std::memcpy(&val, b, 4);
                ::setsockopt(sk->s, SOL_SOCKET, opt, reinterpret_cast<const char*>(&val), sizeof val);
                return {0, 0};
            }
            default:
                return {0, 0};   // SO_NOSIGPIPE and the rest: accepted
        }
    }
    if (level == 6 && opt == 1) {   // IPPROTO_TCP TCP_NODELAY
        int val = 0;
        if (n >= 4) std::memcpy(&val, b, 4);
        const BOOL on = val != 0;
        ::setsockopt(sk->s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof on);
        return {0, 0};
    }
    return {0, 0};   // IP_TOS, TCP keepalive tuning and the like: accepted
}

vsock::Ret getsockopt(int fd, int level, int opt, void* v, size_t* n) {
    auto sk = get(fd);
    if (!sk) return fail(vsock::kEbadf);
    NXLOG_I("net", "cloud: getsockopt fd %d level 0x%x opt 0x%x len %zu", fd, level, opt, *n);
    int32_t val = 0;
    if (level == vsock::kSolSocket && opt == vsock::kSoError) {
        int so = 0, len = sizeof so;
        if (::getsockopt(sk->s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so), &len) != 0) return wsafail();
        val = static_cast<int32_t>(lin(so));
    } else if (level == vsock::kSolSocket && opt == vsock::kSoType) {
        val = sk->type;
    } else {
        std::lock_guard<std::mutex> lk(sk->mu);
        auto it = sk->opts.find((static_cast<uint64_t>(static_cast<uint32_t>(level)) << 32) | static_cast<uint32_t>(opt));
        if (it != sk->opts.end()) val = it->second;
    }
    if (*n >= 4) std::memcpy(v, &val, 4);
    *n = *n >= 4 ? 4 : 0;
    return {0, 0};
}

vsock::Ret getsockname(int fd, vsock::Addr* a) {
    auto sk = get(fd);
    if (!sk) return fail(vsock::kEbadf);
    sockaddr_in sa{};
    int len = sizeof sa;
    if (::getsockname(sk->s, reinterpret_cast<sockaddr*>(&sa), &len) != 0) return wsafail();
    *a = from_sa(sa);
    if (sk->as_local && a->ip == 0) a->ip = local_ip();
    if (sk->type == vsock::kSockDgram)
        NXLOG_I("net", "cloud: udp fd %d is %u.%u.%u.%u:%u", fd, a->ip >> 24, (a->ip >> 16) & 255, (a->ip >> 8) & 255, a->ip & 255, a->port);
    return {0, 0};
}

vsock::Ret getpeername(int fd, vsock::Addr* a) {
    auto sk = get(fd);
    if (!sk) return fail(vsock::kEbadf);
    sockaddr_in sa{};
    int len = sizeof sa;
    if (::getpeername(sk->s, reinterpret_cast<sockaddr*>(&sa), &len) != 0) return wsafail();
    *a = from_sa(sa);
    return {0, 0};
}

vsock::Ret poll(PollFd* fds, size_t n, int timeout_ms) {
    std::vector<WSAPOLLFD> w;
    std::vector<size_t> idx;
    for (size_t i = 0; i < n; ++i) {
        fds[i].revents = 0;
        if (fds[i].fd < 0) continue;
        auto sk = fds[i].fd >= kFdBase ? get(fds[i].fd) : nullptr;
        if (!sk) {
            if (fds[i].fd >= kFdBase) fds[i].revents = kPollNval;
            continue;
        }
        WSAPOLLFD p{};
        p.fd = sk->s;
        p.events = to_wsa_events(fds[i].events);
        w.push_back(p);
        idx.push_back(i);
    }
    if (w.empty()) {
        if (timeout_ms > 0) Sleep(static_cast<DWORD>(timeout_ms));
        return {0, 0};
    }
    const int r = WSAPoll(w.data(), static_cast<ULONG>(w.size()), timeout_ms < 0 ? -1 : timeout_ms);
    if (r == SOCKET_ERROR) return wsafail();
    int count = 0;
    for (size_t k = 0; k < w.size(); ++k) {
        fds[idx[k]].revents = from_wsa_events(w[k].revents);
        if (fds[idx[k]].revents) ++count;
    }
    return {count, 0};
}

vsock::Ret select(int nfds, uint64_t* rs, uint64_t* ws, uint64_t* xs, int timeout_ms) {
    fd_set r, w, x;
    FD_ZERO(&r);
    FD_ZERO(&w);
    FD_ZERO(&x);
    struct M {
        int fd;
        SOCKET s;
    };
    std::vector<M> m;
    auto scan = [&](const uint64_t* set, fd_set* out) {
        if (!set) return;
        for (int fd = kFdBase; fd < nfds && fd < 1024; ++fd) {
            if (!(set[fd >> 6] >> (fd & 63) & 1)) continue;
            auto sk = get(fd);
            if (!sk) continue;
            FD_SET(sk->s, out);
            m.push_back({fd, sk->s});
        }
    };
    scan(rs, &r);
    scan(ws, &w);
    scan(xs, &x);
    auto clear = [](uint64_t* set) {
        if (set) std::memset(set, 0, 16 * sizeof(uint64_t));
    };
    if (m.empty()) {
        if (timeout_ms > 0) Sleep(static_cast<DWORD>(timeout_ms));
        clear(rs);
        clear(ws);
        clear(xs);
        return {0, 0};
    }
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    const int n = ::select(0, rs ? &r : nullptr, ws ? &w : nullptr, xs ? &x : nullptr, timeout_ms < 0 ? nullptr : &tv);
    if (n == SOCKET_ERROR) return wsafail();
    int count = 0;
    auto rebuild = [&](uint64_t* set, fd_set* in) {
        if (!set) return;
        uint64_t out[16] = {};
        for (const M& e : m)
            if ((set[e.fd >> 6] >> (e.fd & 63) & 1) && FD_ISSET(e.s, in) && !(out[e.fd >> 6] >> (e.fd & 63) & 1)) {
                out[e.fd >> 6] |= uint64_t{1} << (e.fd & 63);
                ++count;
            }
        std::memcpy(set, out, sizeof out);
    };
    rebuild(rs, &r);
    rebuild(ws, &w);
    rebuild(xs, &x);
    return {count, 0};
}

int pending(int fd) {
    auto sk = get(fd);
    if (!sk) return -1;
    u_long n = 0;
    if (ioctlsocket(sk->s, FIONREAD, &n) != 0) return -1;
    return static_cast<int>(n);
}

int wait_one(int fd, uint32_t events, int timeout_ms) {
    auto sk = get(fd);
    if (!sk) return -1;
    WSAPOLLFD p{};
    p.fd = sk->s;
    if (events & 1) p.events |= POLLRDNORM;
    if (events & 2) p.events |= POLLWRNORM;
    const int r = WSAPoll(&p, 1, timeout_ms < 0 ? -1 : timeout_ms);
    if (r == SOCKET_ERROR) return -1;
    if (r == 0) return 0;
    int out = 0;
    if (p.revents & (POLLRDNORM | POLLRDBAND | POLLHUP)) out |= 1;
    if (p.revents & POLLWRNORM) out |= 2;
    if (p.revents & (POLLERR | POLLNVAL)) out |= 4;
    return out;
}

int so_error(int fd) {
    auto sk = get(fd);
    if (!sk) return -1;
    int so = 0, len = sizeof so;
    if (::getsockopt(sk->s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so), &len) != 0) return -1;
    return static_cast<int>(lin(so));
}



// =============================================================================================================================================
// Fibers (proof of concept). The SDK's NPLN client (gRPC) runs on nn::os fibers. A fiber switch is one SDK function,
// nn::os::detail::DispatchFiberContext(FiberContext** save, FiberContext** load): it pushes x19..x30 and q8..q15 in a 0xF0-byte frame on
// the guest stack, stores sp in *save, takes sp from *load, pops the same frame and returns there (a new fiber's frame holds its
// FiberType in the x0 slot and "entry | 1" in the x30 slot). Lifted code cannot return into another function's host frame, so each
// guest fiber gets a host fiber (Windows fibers): the replacement below keeps the guest-visible frames exactly as the SDK writes them
// and switches the host stack with them. Addresses are for the 2.0.1 sdk image at 0x7500000000 (build 2.0.1a); other builds: off.
// Limit: a fiber must be resumed on the guest thread that last ran it (the lifted code holds that thread's Ctx).
// =============================================================================================================================================
namespace {

constexpr uint64_t kSdkBase = 0x7500000000ull;
constexpr uint64_t kMainBase = 0x7100000000ull;
constexpr uint64_t kDispatchFiber = kSdkBase + 0x14f648;   // nn::os::detail::DispatchFiberContext
constexpr uint64_t kFiberEntry = kSdkBase + 0x14ba10;      // where a new fiber starts (x0 = its FiberType)

// Every guest fiber has its own host fiber AND its own Ctx: the lifted frames on a host fiber hold the Ctx pointer they were entered
// with, and the SDK resumes a fiber on whichever pool thread is free, so the registers cannot live in a thread's Ctx. The thread
// pointers (tpidr, tpidrro) follow the thread that runs the fiber: they are copied in at every switch.
struct FiberRec {
    void* host = nullptr;
    ::Ctx* ctx = nullptr;
};
std::mutex g_fiber_mu;
std::map<uint64_t, FiberRec> g_fibers;   // guest context slot -> what is suspended in it

void CALLBACK fiber_main(void* p) {
    ::Ctx* c = static_cast<::Ctx*>(p);   // loaded with the new fiber's registers by the switch that started us
    const GuestFn entry = fn_lookup(kFiberEntry);
    if (!entry) NXLOG_E("net", "cloud: no lifted function at the fiber entry 0x%llx", static_cast<unsigned long long>(kFiberEntry));
    if (entry && c) entry(c);
    NXLOG_E("net", "cloud: a fiber's entry function returned; parking its host fiber");
    for (;;) Sleep(1000);
}

void dispatch_fiber(::Ctx* c) {
    const uint64_t save = c->x[0], load = c->x[1];
    const uint64_t s = c->x[31] - 0xf0;
    uint64_t* f = reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(s));
    f[0] = 0;
    for (int i = 0; i < 11; ++i) f[1 + i] = c->x[19 + i];   // x19..x29
    f[12] = c->x[30];
    f[13] = 0;
    for (int i = 0; i < 8; ++i) std::memcpy(reinterpret_cast<uint8_t*>(f) + 0x70 + i * 16, &c->v[8 + i], 16);
    *reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(save)) = s;

    void* self = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiber(nullptr);
    const uint64_t t = *reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(load));
    const uint64_t* g = reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(t));
    const bool fresh = g[0] != 0;   // a new fiber: InitializeImpl put its FiberType in the x0 slot (a suspended frame has 0 there)
    FiberRec to;
    {
        std::lock_guard<std::mutex> lk(g_fiber_mu);
        g_fibers[save] = FiberRec{self, c};
        if (!fresh) {
            auto it = g_fibers.find(load);
            if (it != g_fibers.end()) to = it->second;
        }
    }
    if (fresh) {
        to.ctx = static_cast<::Ctx*>(_aligned_malloc(sizeof(::Ctx), 64));
        std::memcpy(to.ctx, c, sizeof(::Ctx));
        to.host = CreateFiberEx(0x10000, 0x400000, 0, fiber_main, to.ctx);
        std::lock_guard<std::mutex> lk(g_fiber_mu);
        g_fibers[load] = to;
        static std::atomic<int> made{0};
        const int n = ++made;
        if (n <= 4 || n % 256 == 0) NXLOG_I("net", "cloud: fiber %d started (context slot 0x%llx)", n, static_cast<unsigned long long>(load));
    }
    if (!to.host || !to.ctx) {
        NXLOG_E("net", "cloud: fiber switch to an unknown context slot 0x%llx (sp 0x%llx)", static_cast<unsigned long long>(load),
                static_cast<unsigned long long>(t));
        return;
    }
    ::Ctx* d = to.ctx;
    d->x[0] = g[0];
    for (int i = 0; i < 11; ++i) d->x[19 + i] = g[1 + i];
    d->x[30] = g[12];
    d->x[1] = g[13];
    for (int i = 0; i < 8; ++i) std::memcpy(&d->v[8 + i], reinterpret_cast<const uint8_t*>(g) + 0x70 + i * 16, 16);
    d->x[31] = t + 0xf0;
    d->tpidr = c->tpidr;         // the fiber now runs on this thread
    d->tpidrro = c->tpidrro;
    SwitchToFiber(to.host);
    // resumed, maybe on another thread: whoever switched back loaded this fiber's Ctx (the c of this call) from the frame pushed above
}

}  // namespace


void install_fibers() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (!enabled()) return;
        static const char* kName = "_ZN2nn2os6detail20DispatchFiberContextEPPNS1_22FiberContextForHorizonES4_";
        hle_register(kName, dispatch_fiber);
        module_override_symbol(kName);
        NXLOG_I("net", "cloud: nn::os fibers run on host fibers (DispatchFiberContext answered on the host)");
    });
}

// The lifted sdk calls DispatchFiberContext directly (no import slot), so the host code of the lifted function is redirected:
// its first 12 bytes become "mov rax, dispatch_fiber; jmp rax". Both are GuestFn (Ctx* in rcx). Proof of concept only.
void patch_fiber_dispatch() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (!enabled()) return;
        const GuestFn lifted = fn_lookup(kDispatchFiber);
        if (!lifted || !fn_lookup(kFiberEntry)) {
            NXLOG_W("net", "cloud: no lifted function at the sdk fiber addresses (not 2.0.1a?): fibers are not redirected");
            return;
        }
        uint8_t* at = reinterpret_cast<uint8_t*>(lifted);
        uint8_t code[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};
        const uint64_t target = reinterpret_cast<uint64_t>(&dispatch_fiber);
        std::memcpy(code + 2, &target, 8);
        DWORD old = 0;
        if (!VirtualProtect(at, sizeof code, PAGE_EXECUTE_READWRITE, &old)) {
            NXLOG_W("net", "cloud: cannot patch the lifted DispatchFiberContext (error %lu)", GetLastError());
            return;
        }
        std::memcpy(at, code, sizeof code);
        VirtualProtect(at, sizeof code, old, &old);
        FlushInstructionCache(GetCurrentProcess(), at, sizeof code);
        NXLOG_I("net", "cloud: lifted DispatchFiberContext at %p redirected to the host fiber switch", static_cast<void*>(at));
        // DEBUG: which function entries stored as words in main did the lifter not lift? (cloud_entries.txt: module offsets in hex)
        if (FILE* f = std::fopen("cloud_entries.txt", "r")) {
            FILE* o = std::fopen("cloud_entries_missing.txt", "w");
            unsigned long long off = 0;
            int total = 0, missing = 0;
            while (std::fscanf(f, "%llx", &off) == 1) {
                ++total;
                if (!fn_lookup(kMainBase + off)) {
                    ++missing;
                    if (o) std::fprintf(o, "0x%llx\n", off);
                }
            }
            std::fclose(f);
            if (o) std::fclose(o);
            NXLOG_I("net", "cloud: %d of %d stored function entries in main are not lifted (cloud_entries_missing.txt)", missing, total);
        }
        // main+0x1769584 (2.0.1a): a thread entry the game hands to the SDK when it travels to a cloud island. The lifter never saw
        // it (it is only referenced as a function pointer), so the first call traps. It is five instructions; here it is as host code:
        //   fn = [x0+0x1c0]; fn([x0+0x1c8]); if ([x0+0x1c0] != [main+0x34ae828]) main+0x17698f8(x0);
        if (!fn_lookup(kMainBase + 0x1769584)) {
            fn_register(kMainBase + 0x1769584, [](::Ctx* c) {
                const uint64_t obj = c->x[0], lr = c->x[30];
                auto rd = [](uint64_t a) { return *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(a)); };
                c->x[0] = rd(obj + 0x1c8);
                call_indirect(c, rd(obj + 0x1c0));
                if (rd(obj + 0x1c0) != rd(kMainBase + 0x34ae828)) {
                    c->x[0] = obj;
                    call_indirect(c, kMainBase + 0x17698f8);
                }
                c->x[30] = lr;
            });
            NXLOG_I("net", "cloud: main+0x1769584 (cloud island thread entry) answered on the host");
        }
    });
}

}  // namespace nxrt::services::netsvc::cloud
