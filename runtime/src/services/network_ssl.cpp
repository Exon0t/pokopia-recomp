// SPDX-License-Identifier: GPL-3.0-or-later
// ssl (ssl, ssl:s) and nim:eca: offline TLS and shop services. See network.cpp for the shared provenance and rules.
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
// ssl: ssl (ISslService). Switchbrew "SSL services". CreateContext answers. Connection calls fail with ssl InvalidSocket: there is no
// socket to run TLS over. Local settings (options, session cache mode, ALPN, verify option) are accepted.
// =============================================================================================================================================

std::shared_ptr<ipc::Service> make_ssl_connection(const std::shared_ptr<int>& conns);

std::shared_ptr<ipc::Service> make_ssl_context() {
    {
        NetState& s = st();
        std::lock_guard<std::mutex> lk(s.mu);
        s.ssl_contexts++;
    }
    auto conns = std::make_shared<int>(0);     // live connections of this context
    return tracked(
        "nn::ssl::sf::ISslContext",
        [] {
            NetState& s = st();
            std::lock_guard<std::mutex> lk(s.mu);
            if (s.ssl_contexts > 0) s.ssl_contexts--;
        },
        [conns](ipc::Service& svc) {
            rows(svc, "ssl", {
                {0, "SetOption", K::Ok},
                {1, "GetOption", K::S32, 0},
                {6, "RemoveServerPki", K::Ok},
                {7, "RemoveClientPki", K::Ok},
                {9, "AddPolicyOid", K::Ok},
                {11, "RemoveCrl", K::Ok},
                {13, "GeneratePrivateKeyAndCert", K::Fail, kSslInvalidOption},
            });
            cmd(svc, "ssl", 2, "CreateConnection", [conns](ipc::Request&, ipc::Response& rsp) -> uint32_t {
                rsp.push_object(make_ssl_connection(conns));
                return 0;
            });
            cmd(svc, "ssl", 3, "GetConnectionCount", [conns](ipc::Request&, ipc::Response& rsp) -> uint32_t {
                rsp.push(static_cast<uint32_t>(*conns));
                return 0;
            });
            for (uint32_t id : {4u, 5u, 8u, 10u, 12u}) {
                const char* name = id == 4 ? "ImportServerPki" : id == 5 ? "ImportClientPki" : id == 8 ? "RegisterInternalPki"
                                 : id == 10 ? "ImportCrl" : "ImportClientCertKeyPki";
                cmd(svc, "ssl", id, name, [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
                    rsp.push(alloc_pki_id());
                    return 0;
                });
            }
        });
}

std::shared_ptr<ipc::Service> make_ssl_connection(const std::shared_ptr<int>& conns) {
    ++(*conns);
    std::shared_ptr<int> c = conns;
    return tracked(
        "nn::ssl::sf::ISslConnection",
        [c] { --(*c); },
        [](ipc::Service& svc) {
            cmd(svc, "ssl", 1, "SetHostName", [](ipc::Request& req, ipc::Response&) -> uint32_t {
                ask_ssl_host(req);
                return 0;
            });
            rows(svc, "ssl", {
                {0, "SetSocketDescriptor", K::Fail, kSslInvalidSocket},
                {2, "SetVerifyOption", K::Ok},
                {3, "SetIoMode", K::Ok},
                {4, "GetSocketDescriptor", K::S32, 0xFFFFFFFFu},   // -1: none was set
                {5, "GetHostName", K::U32, 0},
                {6, "GetVerifyOption", K::U32, 0},
                {7, "GetIoMode", K::U32, 0},
                {8, "DoHandshake", K::Fail, kSslInvalidSocket},
                {9, "DoHandshakeGetServerCert", K::Fail, kSslInvalidSocket},
                {10, "Read", K::Fail, kSslInvalidSocket},
                {11, "Write", K::Fail, kSslInvalidSocket},
                {12, "Pending", K::S32, 0},
                {13, "Peek", K::Fail, kSslInvalidSocket},
                {14, "Poll", K::U32, 0},
                {15, "GetVerifyCertError", K::U32, 0},
                {16, "GetNeededServerCertBufferSize", K::U32, 0},
                {17, "SetSessionCacheMode", K::Ok},
                {18, "GetSessionCacheMode", K::U32, 0},
                {19, "FlushSessionCache", K::Ok},
                {20, "SetRenegotiationMode", K::Ok},
                {21, "GetRenegotiationMode", K::U32, 0},
                {22, "SetOption", K::Ok},
                {23, "GetOption", K::U8, 0},
                {24, "GetVerifyCertErrors", K::Ok},
                {25, "GetCipherInfo", K::Fail, kSslInvalidSocket},
                {26, "SetNextAlpnProto", K::Ok},
                {27, "GetNextAlpnProto", K::Zeros, 0, 8},          // (u32 state, u32 size) = (0, 0)
                {28, "SetDtlsSocketDescriptor", K::Fail, kSslInvalidSocket},
                {29, "GetDtlsHandshakeTimeout", K::U64, 0},
                {30, "SetPrivateOption", K::Ok},
                {31, "SetSrtpCiphers", K::Ok},
                {32, "GetSrtpCipher", K::U32, 0},
                {33, "ExportKeyingMaterial", K::Fail, kSslInvalidSocket},
                {34, "SetIoTimeout", K::Ok},
                {35, "GetIoTimeout", K::U64, 0},
                {36, "GetSessionTicket", K::U32, 0},
                {37, "SetSessionTicket", K::Ok},
            });
            if (!cloud::enabled()) return;
            // Cloud Island proof of concept: no TLS here. The connection passes the game's bytes to its socket untouched; the local
            // gateway that guest port 443 is sent to does the TLS. Command layouts: Switchbrew "SSL services", libnx ssl.c (ISC).
            struct Pass {
                int fd = -1;        // our own descriptor of the game's socket (cloud::dup)
                int given = -1;     // the descriptor the game passed
                uint32_t io = 1;    // IoMode: 1 blocking, 2 non-blocking
                bool http11 = false;   // the game offered http/1.1 in SetNextAlpnProto
                ~Pass() {
                    if (fd >= 0) cloud::close(fd);
                }
            };
            auto ps = std::make_shared<Pass>();
            constexpr uint32_t kWouldBlock = res(123, 204), kTimeout = res(123, 205), kReset = res(123, 209);
            cmd(svc, "ssl", 0, "SetSocketDescriptor", [ps](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
                const int fd = req.arg<int32_t>(0);
                cloud::to_gateway(fd);
                const int d = cloud::dup(fd);
                if (d < 0) return kSslInvalidSocket;
                ps->fd = d;
                ps->given = fd;
                NXLOG_I("net", "cloud: ssl passthrough on fd %d", fd);
                rsp.push(static_cast<int32_t>(fd));   // the game closes this one; ours keeps the socket open
                return 0;
            });
            cmd(svc, "ssl", 3, "SetIoMode", [ps](ipc::Request& req, ipc::Response&) -> uint32_t {
                ps->io = req.arg<uint32_t>(0);
                return 0;
            });
            cmd(svc, "ssl", 4, "GetSocketDescriptor", [ps](ipc::Request&, ipc::Response& rsp) -> uint32_t {
                rsp.push(static_cast<int32_t>(ps->given));
                return 0;
            });
            cmd(svc, "ssl", 7, "GetIoMode", [ps](ipc::Request&, ipc::Response& rsp) -> uint32_t {
                rsp.push(ps->io);
                return 0;
            });
            auto handshake = [ps](bool certs) {
                return [ps, certs](ipc::Request&, ipc::Response& rsp) -> uint32_t {
                    if (ps->fd < 0) return kSslInvalidSocket;
                    // the TCP connect may still be in progress on a non-blocking socket: writable means connected
                    const int w = cloud::wait_one(ps->fd, 2, ps->io == 2 ? 0 : 15000);
                    if (w == 0) return ps->io == 2 ? kWouldBlock : kTimeout;
                    if (w < 0 || (w & 4) || cloud::so_error(ps->fd) != 0) return kReset;
                    NXLOG_I("net", "cloud: ssl handshake answered (passthrough) on fd %d", ps->given);
                    if (certs) {
                        rsp.push(uint32_t{0});
                        rsp.push(uint32_t{0});
                    }
                    return 0;
                };
            };
            cmd(svc, "ssl", 8, "DoHandshake", handshake(false));
            cmd(svc, "ssl", 9, "DoHandshakeGetServerCert", handshake(true));
            auto read = [ps](bool peek) {
                return [ps, peek](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
                    if (ps->fd < 0) return kSslInvalidSocket;
                    const ipc::Buffer out = req.out_buf(0);
                    if (ps->io != 2) {
                        const int w = cloud::wait_one(ps->fd, 1, 300000);
                        if (w == 0) return kTimeout;
                    }
                    const vsock::Ret r = cloud::recv(ps->fd, buf_mut(out), static_cast<size_t>(out.size),
                                                     vsock::kMsgDontwait | (peek ? vsock::kMsgPeek : 0));
                    if (r.err == vsock::kEagain) return kWouldBlock;
                    if (r.err) return kReset;
                    rsp.push(static_cast<uint32_t>(r.ret));
                    return 0;
                };
            };
            cmd(svc, "ssl", 10, "Read", read(false));
            cmd(svc, "ssl", 13, "Peek", read(true));
            cmd(svc, "ssl", 11, "Write", [ps](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
                if (ps->fd < 0) return kSslInvalidSocket;
                const ipc::Buffer in = req.in_buf(0);
                if (ps->io != 2 && cloud::wait_one(ps->fd, 2, 300000) == 0) return kTimeout;
                const vsock::Ret r = cloud::send(ps->fd, buf_ptr(in), static_cast<size_t>(in.size), vsock::kMsgDontwait);
                if (r.err == vsock::kEagain) return kWouldBlock;
                if (r.err) return kReset;
                rsp.push(static_cast<uint32_t>(r.ret));
                return 0;
            });
            cmd(svc, "ssl", 12, "Pending", [ps](ipc::Request&, ipc::Response& rsp) -> uint32_t {
                const int n = ps->fd < 0 ? 0 : cloud::pending(ps->fd);
                rsp.push(static_cast<int32_t>(n < 0 ? 0 : n));
                return 0;
            });
            cmd(svc, "ssl", 14, "Poll", [ps](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
                if (ps->fd < 0) return kSslInvalidSocket;
                const int w = cloud::wait_one(ps->fd, req.arg<uint32_t>(0), static_cast<int>(req.arg<uint32_t>(4)));
                if (w == 0) return kTimeout;
                if (w < 0) return kReset;
                rsp.push(static_cast<uint32_t>(w));
                return 0;
            });
            // ALPN: the gateway speaks http/1.1 upstream (the storage endpoints take it). The game is told http/1.1 when it offered it,
            // else "no ALPN".
            cmd(svc, "ssl", 26, "SetNextAlpnProto", [ps](ipc::Request& req, ipc::Response&) -> uint32_t {
                const ipc::Buffer in = req.in_buf(0);
                const uint8_t* p = buf_ptr(in);
                for (uint64_t i = 0; in.addr && i < in.size;) {
                    const uint64_t len = p[i];
                    if (len == 8 && i + 9 <= in.size && std::memcmp(p + i + 1, "http/1.1", 8) == 0) ps->http11 = true;
                    i += 1 + len;
                }
                return 0;
            });
            cmd(svc, "ssl", 27, "GetNextAlpnProto", [ps](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
                const uint32_t n = ps->http11 ? static_cast<uint32_t>(buf_write(req.out_buf(0), "http/1.1", 8)) : 0;
                rsp.push(uint32_t{ps->http11 ? 1u : 0u});   // AlpnProtoState: Negotiated / NoSupport
                rsp.push(n);
                return 0;
            });
        });
}

}  // namespace

std::shared_ptr<ipc::Service> make_ssl_service() {
    return make_iface("nn::ssl::sf::ISslService", [](ipc::Service& s) {
        cmd(s, "ssl", 0, "CreateContext", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_ssl_context());
            return 0;
        });
        cmd(s, "ssl", 1, "GetContextCount", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            NetState& n = st();
            std::lock_guard<std::mutex> lk(n.mu);
            rsp.push(static_cast<uint32_t>(n.ssl_contexts));
            return 0;
        });
        // cmd 100 CreateContextForSystem (in: version + pid placeholder): the game's HTTP client (island uploads) opens its context
        // with this one on ssl:s. Same context object.
        cmd(s, "ssl", 100, "CreateContextForSystem", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_ssl_context());
            return 0;
        });
        rows(s, "ssl", {
            {2, "GetCertificates", K::U32, 0},
            {3, "GetCertificateBufSize", K::U32, 0},
            {4, "DebugIoctl", K::Fail, kSslInvalidOption},
            {5, "SetInterfaceVersion", K::Ok},
            {6, "FlushSessionCache", K::U32, 0},
            {7, "SetDebugOption", K::Ok},
            {8, "GetDebugOption", K::BufZero, 0, 1},
            {9, "ClearTls12FallbackFlag", K::Ok},
            {10, "GetCertificateByIndex", K::Ok},
            {11, "GetTrustedCertificateCount", K::U32, 0},
        });
        if (cloud::enabled() && !cloud::ca_der().empty()) {
            // Cloud Island proof of concept: the game builds its own TLS trust store from the console's built-in CA certificates.
            // Whatever ids it asks for, it gets one trusted entry, the [net] cloud_ca certificate. Layout (libnx ssl.h, ISC):
            // BuiltInCertificateInfo {u32 id, u32 status (1 = enabled and trusted), u64 size, u64 offset of the DER in the buffer}.
            auto first_id = [](ipc::Request& req) {
                uint32_t id = 0xFFFFFFFFu;
                const ipc::Buffer in = req.in_buf(0);
                const uint64_t n = in.addr ? in.size / 4 : 0;
                if (n) buf_read(in, &id, 4);
                NXLOG_I("net", "cloud: the game asks for %llu built-in CA certificate(s), first id %d: answering with the cloud CA",
                        static_cast<unsigned long long>(n), static_cast<int32_t>(id));
                return id == 0xFFFFFFFFu ? 1u : id;
            };
            cmd(s, "ssl", 3, "GetCertificateBufSize", [first_id](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
                first_id(req);
                rsp.push(static_cast<uint32_t>(0x18 + cloud::ca_der().size()));
                return 0;
            });
            cmd(s, "ssl", 2, "GetCertificates", [first_id](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
                const std::vector<uint8_t>& der = cloud::ca_der();
                std::vector<uint8_t> out(0x18 + der.size(), 0);
                const uint32_t id = first_id(req), status = 1;
                const uint64_t size = der.size(), off = 0x18;
                std::memcpy(out.data(), &id, 4);
                std::memcpy(out.data() + 4, &status, 4);
                std::memcpy(out.data() + 8, &size, 8);
                std::memcpy(out.data() + 16, &off, 8);
                std::memcpy(out.data() + 0x18, der.data(), der.size());
                const uint64_t w = buf_write(req.out_buf(0), out.data(), out.size());
                rsp.push(static_cast<uint32_t>(w == out.size() ? 1 : 0));
                return 0;
            });
        }
    });
}

namespace {

// =============================================================================================================================================
// nim:eca: the shop (ECA) interface. Creation succeeds; the shop objects answer their creation commands, nothing needs the network.
// =============================================================================================================================================

std::shared_ptr<ipc::Service> make_shop_async() {
    return make_iface("nn::ec::IShopServiceAsync", [](ipc::Service&) {});   // no command table is read yet: any command is unhandled
}

std::shared_ptr<ipc::Service> make_shop_accessor() {
    return make_iface("nn::ec::IShopServiceAccessor", [](ipc::Service& s) {
        cmd(s, "nim:eca", 0, "CreateAsyncInterface", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_shop_async());
            return 0;
        });
    });
}

std::shared_ptr<ipc::Service> make_shop_server() {
    return make_iface("nn::ec::IShopServiceAccessServer", [](ipc::Service& s) {
        cmd(s, "nim:eca", 0, "CreateAccessorInterface", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_shop_accessor());
            return 0;
        });
    });
}

}  // namespace

std::shared_ptr<ipc::Service> make_nim_eca() {
    return make_iface("nn::nim::detail::IShopServiceAccessServerInterface", [](ipc::Service& s) {
        auto create = [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            close_copied(req);
            rsp.push_object(make_shop_server());
            return 0;
        };
        cmd(s, "nim:eca", 0, "CreateServerInterface", create);
        rows(s, "nim:eca", {
            {4, "IsLargeResourceAvailable", K::U8, 0},
        });
        cmd(s, "nim:eca", 5, "CreateServerInterface2", create);
    });
}

}  // namespace nxrt::services::netsvc
