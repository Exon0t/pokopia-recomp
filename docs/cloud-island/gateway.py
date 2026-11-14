#!/usr/bin/env python3
"""Plaintext-to-TLS gateway for the Recompia cloud-island proof of concept.

The runtime's ssl service passes the game's bytes through untouched (no TLS in the runtime yet), and its bsd service sends every
connection the game makes to the NPLN names on guest port 443 to this gateway instead. The gateway opens a real TLS connection to
Raikaru's pokopia-cloud-server (server.py) for each one and pumps bytes both ways. One SNI for every connection (the server's
certificate covers every NPLN name). ALPN offers h2 first (gRPC), then http/1.1 (storage uploads work over either).

  python gateway.py [--listen 127.0.0.1:18443] [--server 127.0.0.1:443] [--sni gamesync.npln.nintendo.net] [--ca ca.crt] [--log gateway.log]

Without --ca the server certificate is not verified (local proof of concept only).
"""
import argparse, select, socket, ssl, threading, time, sys, datetime

def log(f, msg):
    line = f"{datetime.datetime.now().strftime('%H:%M:%S.%f')[:-3]} {msg}"
    print(line, flush=True)
    if f:
        f.write(line + "\n"); f.flush()

def pump(src, dst, tag, stats, f):
    try:
        while True:
            data = src.recv(65536)
            if not data:
                break
            stats[tag] += len(data)
            if stats[tag] == len(data):
                log(f, f"{stats['id']} {tag} first bytes: {data[:24]!r}")
            dst.sendall(data)
    except (OSError, ssl.SSLError) as e:
        log(f, f"{stats['id']} {tag} ended: {e}")
    finally:
        try: dst.shutdown(socket.SHUT_WR)
        except OSError: pass

def handle(conn, addr, args, ctx, f, n):
    stats = {"id": f"c{n}", "game->server": 0, "server->game": 0}
    host, port = args.server.rsplit(":", 1)
    try:
        raw = socket.create_connection((host, int(port)), timeout=10)
        up = ctx.wrap_socket(raw, server_hostname=args.sni)
        up.settimeout(None)
        log(f, f"c{n} from {addr[0]}:{addr[1]} -> TLS {host}:{port} sni={args.sni} alpn={up.selected_alpn_protocol()} {up.version()}")
    except Exception as e:
        log(f, f"c{n} upstream connect failed: {e}")
        conn.close(); return
    # One thread per connection moves both directions: an SSLSocket must not be read and written from two threads at once.
    # The TLS side is non-blocking: after the handshake a TLS 1.3 server sends session tickets, which make the socket readable
    # without any application data, and a blocking read there would hold up the upload going the other way.
    conn.settimeout(None)
    up.setblocking(False)
    def note(tag, data):
        stats[tag] += len(data)
        if stats[tag] == len(data):
            log(f, f"{stats['id']} {tag} first bytes: {data[:24]!r}")
    def up_send(data):
        view = memoryview(data)
        while view:
            try:
                view = view[up.send(view):]
            except ssl.SSLWantWriteError:
                select.select([], [up], [], 30)
            except ssl.SSLWantReadError:
                select.select([up], [], [], 30)
    def up_recv():
        """Application bytes, b"" at end of stream, None when there is nothing to read yet."""
        try:
            return up.recv(65536)
        except (ssl.SSLWantReadError, ssl.SSLWantWriteError):
            return None
    open_c = open_u = True
    try:
        while open_u:
            want = ([conn] if open_c else []) + [up]
            ready = [up] if up.pending() else select.select(want, [], [], 60)[0]
            if conn in ready:
                data = conn.recv(65536)
                if data:
                    note("game->server", data); up_send(data)
                else:
                    open_c = False
            if up in ready:
                data = up_recv()
                if data:
                    note("server->game", data); conn.sendall(data)
                elif data == b"":
                    open_u = False
    except (OSError, ssl.SSLError) as e:
        log(f, f"c{n} ended: {e}")
    for s in (conn, up):
        try: s.close()
        except OSError: pass
    log(f, f"c{n} closed: game->server {stats['game->server']} B, server->game {stats['server->game']} B")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listen", default="127.0.0.1:18443")
    ap.add_argument("--server", default="127.0.0.1:443")
    ap.add_argument("--sni", default="gamesync.npln.nintendo.net")
    ap.add_argument("--ca", default=None)
    ap.add_argument("--log", default=None)
    args = ap.parse_args()
    f = open(args.log, "a", encoding="utf-8") if args.log else None
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    if args.ca:
        ctx.load_verify_locations(args.ca); ctx.check_hostname = True; ctx.verify_mode = ssl.CERT_REQUIRED
    else:
        ctx.check_hostname = False; ctx.verify_mode = ssl.CERT_NONE
    ctx.set_alpn_protocols(["http/1.1"])   # the runtime tells the game http/1.1
    lh, lp = args.listen.rsplit(":", 1)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((lh, int(lp))); srv.listen(32)
    log(f, f"gateway listening plaintext on {args.listen}, upstream TLS {args.server} (verify={'ca' if args.ca else 'off'})")
    n = 0
    while True:
        conn, addr = srv.accept(); n += 1
        threading.Thread(target=handle, args=(conn, addr, args, ctx, f, n), daemon=True).start()

if __name__ == "__main__":
    main()
