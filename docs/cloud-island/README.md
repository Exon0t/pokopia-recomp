# Cloud Islands against a self-hosted server (experimental)

With `[net] cloud_server` set, the game's own NPLN client talks to a self-hosted server such as
[Raikaru/pokopia-cloud-server](https://github.com/Raikaru/pokopia-cloud-server). Nothing contacts Nintendo: the NPLN names resolve to
the configured address or fail. Everything is off without `cloud_server`.

## Status

Run-tested on Pokopia 2.0.1a with the 0.11.1 runtime: create an island, enter it, upload, return to it (download), a second player
joining by address and Link Code, building together. Two machines on a LAN and over the internet (router port forward, direct UDP).
Run-tested on 0.13.1 with this branch (two copies on one PC): online, travel to the island, upload on arrival and on leaving, the
owner's return (download), a second player joining by address and Link Code, and the game's own "Kick a player". "Send a report"
reaches `Screening/IssueUploadUri`, which the server does not answer yet.

Not done: more than two players, a relay for strict NATs, accounts (the tests used the server's `--insecure-no-auth`), TLS in the
runtime, a lifter site for the fiber switch (see below).

## Configuration (`pkp.ini`)

```ini
[net]
online_test = on
cloud_server = 127.0.0.1          ; IPv4 address of the server
cloud_ca = C:/path/to/ca.der      ; the server's CA certificate, DER
cloud_gateway_port = 18443        ; local plaintext-to-TLS gateway for the island upload and download (0 = off)
cloud_storage_port = 0            ; or: the server's plain HTTP storage port (its --http-port); no gateway needed
```

With `cloud_storage_port` set, the island file goes straight to `cloud_server` on that port in plain HTTP (start the server with
`--http-port <port>`; that flag cannot be combined with its `--client-ca` account mode). The file is not encrypted on the way.
Without it:

Run `gateway.py --server <address>:443 --ca <ca.crt> --sni storage.npln.nintendo.net` next to the game. The game does its own TLS for
gRPC; the island file goes through `nn::ssl`, which the runtime does not implement, so that one connection goes through the gateway.

## The game executable needs 131 more entries

`main-extra-entries-2.0.1a.txt` lists 131 function entries in main's online code (0x1730000 to 0x1770000) that the lifter does not
find: 74 are only stored as 64-bit words, 57 are only reached through a computed address. This branch has them in
`extra_entries['main']` (`recomp/games/pkp/config.py`), and `recomp/nxrecomp/env.py` gives an extra entry in main a real extent (up to
the next known start) instead of 4 bytes. A game built before this change must be rebuilt.
Other computed targets in that range are jump-table labels and must not become entries.

## Version-specific parts still in the runtime

`network_cloud.cpp` redirects `nn::os::detail::DispatchFiberContext` (sdk+0x14f648 in 2.0.1a) to host fibers by patching the lifted
function at run time, and answers one thread entry (main+0x1769584) on the host. Both belong in the lifter as sites.

## Testing with two copies of one save

Two copies of a save are the same player and the host refuses its own id (error 9005). `save_uuid.py <save dir> reroll` gives a copy
a new player id. `[account] network_account_id` must differ between the two as well.
