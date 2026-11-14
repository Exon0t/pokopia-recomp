// SPDX-License-Identifier: GPL-3.0-or-later
// Cloud Island proof of concept. With [net] cloud_server = <ipv4> the game's NPLN names (*.npln.nintendo.net, *.npln.srv.nintendo.net)
// resolve to that address, its AF_INET SOCK_STREAM sockets become real host TCP sockets, and the ssl service passes the bytes through
// untouched (the runtime does no TLS yet: a local gateway on 127.0.0.1:[net] cloud_gateway_port takes guest port 443 and does the TLS
// to the server, see Projects/pokopia-tests/cloud-island/gateway.py). Everything here stays off without cloud_server. Experimental.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "../net/vsock.h"

namespace nxrt::services::netsvc::cloud {

bool enabled();                 // [net] cloud_server is an IPv4 address
uint32_t server_ip();           // host order
uint16_t gateway_port();        // guest port 443 at server_ip connects to 127.0.0.1:this instead (0 = direct)
bool is_cloud_name(const std::string& host);
const std::vector<uint8_t>& ca_der();   // [net] cloud_ca: the DER certificate the game should trust (empty: none)

struct PollFd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

// Host TCP sockets, descriptors from kFdBase (above vsock's 0..127 and the co-op module's 0x100..0x11F, below the 1024-bit fd_set).
// Every call answers (ret, errno) with Linux errno numbers, as vsock does.
constexpr int kFdBase = 0x200;
bool is_host_fd(int fd);
bool any_host_fd(const PollFd* fds, size_t n);
vsock::Ret socket(int domain, int type, int protocol);
vsock::Ret eventfd(uint64_t init, int flags);          // bsd EventFd: flags bit 0 semaphore, bit 2 non-blocking; read/write are recv/send of 8 bytes
bool to_gateway(int fd);                                // swap the connection for one to the local TLS gateway (before any data)
int dup(int fd);                                        // a second descriptor of the same socket (-1: bad descriptor); for ssl
vsock::Ret connect(int fd, vsock::Addr a);
vsock::Ret bind(int fd, vsock::Addr a);
vsock::Ret send(int fd, const void* p, size_t n, int flags);
vsock::Ret recv(int fd, void* p, size_t n, int flags);
vsock::Ret sendto(int fd, const void* p, size_t n, int flags, const vsock::Addr* to);     // datagram sockets (host UDP): to may be null
vsock::Ret recvfrom(int fd, void* p, size_t n, int flags, vsock::Addr* from);            // from (may be null) gets the sender
bool is_dgram(int fd);
uint32_t local_ip();          // this PC's IPv4 address on its default route, host order (127.0.0.1 when there is none)
vsock::Ret shutdown(int fd, int how);
vsock::Ret close(int fd);
vsock::Ret fcntl(int fd, int cmd, int arg);
vsock::Ret setsockopt(int fd, int level, int opt, const void* v, size_t n);
vsock::Ret getsockopt(int fd, int level, int opt, void* v, size_t* n);
vsock::Ret getsockname(int fd, vsock::Addr* a);
vsock::Ret getpeername(int fd, vsock::Addr* a);
vsock::Ret poll(PollFd* fds, size_t n, int timeout_ms);                                    // non-host descriptors get revents 0
vsock::Ret select(int nfds, uint64_t* r, uint64_t* w, uint64_t* x, int timeout_ms);       // 1024-bit sets; non-host bits are cleared

// For the ssl passthrough: bytes waiting (-1: bad descriptor); one descriptor's readiness within timeout_ms (events and the answer are
// bit 0 read, bit 1 write, bit 2 except; 0 = timed out, -1 = bad descriptor); the pending SO_ERROR as a Linux errno (-1: bad descriptor).
int pending(int fd);
int wait_one(int fd, uint32_t events, int timeout_ms);
int so_error(int fd);

}  // namespace nxrt::services::netsvc::cloud
