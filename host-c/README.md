# cdhost — CoreDevice host in C

The C side of rPlayHub. Same protocol as the Python harness in `../host/`, verified live against
the same iPhone (iPhone13,3, iOS 26.5.2).

C and Swift are the shipping languages: **C** for the portable protocol core, **Swift** for the
macOS app (the Device Hub-style "View Screen" window). The Python in `../host/` is an
experiment and verification harness only — it is where wire formats get proven before being
committed to in C or Swift.

## Status

| layer | C | notes |
|---|---|---|
| 0 usbmux | ✅ `cdhost.c` | list devices, connect-to-port |
| 1 lockdown | ✅ `cdhost.c` | QueryType, GetValue (real device info) |
| 1.5 TLS session | ✅ `tls_openssl.c` | client-cert from the usbmuxd pair record; `tls_mbedtls.c` builds but see below |
| 2 CoreDevice tunnel | ✅ `cdhost.c` | `CDTunnel` magic + u16 + JSON handshake |
| 3a utun + packet pump | ✅ `cdhost.c` | needs root; RSD then reachable with an ordinary socket |
| 3 RemoteXPC + XPC codec | ✅ `../core/rp_remotexpc.c` | handshake is **byte-identical** to the Python (195 bytes) |
| 3b RSD service discovery | ✅ `cdhost.c` | one handshake returns the ~85-entry service map |
| 4 screen / HID services | TODO | port `../host/screen.py`, `../host/hid.py` |
| 2b RemotePairing (direct wifi door) | TODO | the big one — see `../deps/AccessorySDK/PROVENANCE.md` |

The protocol core is compiled from `../core`, not duplicated here: `rp_xpc.c`, `rp_http2.c` and
`rp_remotexpc.c` contain no sockets, no allocation and no Apple headers, which is what lets the
same files build for Linux and Windows. `cdhost.c` holds only the platform plumbing — usbmuxd,
OpenSSL, utun.

`make -C ../core test` verifies the whole protocol layer **with no device attached**, including
the RemoteXPC opening exchange byte-for-byte against the Python. That matters because the
handshake's ordering and flags come from a captured session rather than from a specification: one
wrong byte and the device closes the connection without explanation.

## Support SDK

The pieces we need are vendored in-repo at **`../deps/AccessorySDK/`** (`Support` + `External`,
~5 MB). That is the relevant SDK — *not* CarPlay, which was only where the earlier experiments
happened.

`../deps/AccessorySDK/PROVENANCE.md` lists where it came from and which file covers which need:
SRP-6a, the pair-setup/pair-verify state machine (`PairingUtils.c`), OPACK, TLV8,
ChaCha20-Poly1305, Curve25519/Ed25519, LibTomMath, BonjourBrowser, and CFLite binary plists. It also
points at `~/iPhoneMirroring/iphone-mirror-client/src/rapport.c`, which is working C for the modern
Pair-Verify flow and the better primary crib.

## Build

- **macOS (fast verify):** `make` → system CoreFoundation + Homebrew OpenSSL, then `./cdhost`.
- **Linux/OEM:** `make` → compiles against the vendored CFLite/crypto headers.

## Lifecycle: the API is up before any device

`cdhost` binds the control API (`127.0.0.1:9876`, plus 9877 video and 9878 audio) **first**, then
waits for a device: it polls usbmux every 2 s and brings the first suitable phone up as soon as one
appears (USB preferred; `--udid <prefix>` / `RPLAY_UDID` restricts it). With no phone, or with no
usbmuxd at all, it keeps running and answers the API. A failed bring-up (locked, untrusted, too
old) goes back to waiting with a per-device backoff -- 5, 10, 20, 40, then 60 s -- during which
other attached devices are tried first; the backoff resets on a successful bind, when the device
detaches from usbmux, or when it is selected explicitly. If a bring-up hangs past the 30 s
watchdog, the engine relaunches itself (same device preference, the stuck device starting in
backoff) instead of exiting. `--no-wait` restores the old behaviour -- exit 1 when no device can be
bound -- for scripts.

`RPLAY_API_PORT`, `RPLAY_STREAM_PORT` and `RPLAY_AUDIO_PORT` move 9876/9877/9878 (e.g. to run a
test engine beside another one).

When a userspace (lwIP) tunnel dies -- phone unplugged, rebooted, off wifi -- the engine restarts
itself into the waiting state rather than serving a dead session (bug #7), preferring the device
that was selected (or bound) so the default pick cannot grab a different phone. The kernel-utun path
(macOS, root) keeps its old behaviour: `pump_die` exits.

### Engine status in `list_devices`

`list_devices` works with nothing bound and carries three fields beside `devices` (all existing
device fields are unchanged; each device also carries `"binding": true|false`, true for the one
device bring-up is currently working on):

| field | type | meaning |
|---|---|---|
| `usbmuxd` | bool | `false` when libusbmuxd cannot reach the mux daemon **at all** (Windows: Apple Mobile Device Service missing or stopped, nothing on `127.0.0.1:27015`). `true` with an empty `devices` means "reachable, no phone attached". |
| `usbmuxd_error` | string | present only when `usbmuxd` is `false`. Windows: `Apple Mobile Device Service is not running. Install Apple Devices (Microsoft Store) or iTunes.` Linux/macOS: a short usbmuxd message. |
| `engine_state` | string | `waiting_for_device` (API up, no session) · `binding` (device found, bring-up in progress) · `ready` (session bound; every method available). |
| `engine_error` | string | optional; the last bring-up failure (`<udid>: <reason>`), present only while not `ready`. |

```json
{"id":2,"ok":true,"result":{"devices":[],"usbmuxd":false,
 "usbmuxd_error":"Apple Mobile Device Service is not running. Install Apple Devices (Microsoft Store) or iTunes.",
 "engine_state":"waiting_for_device"}}
```

With no session bound:

- `ping`, `quit`, `list_devices`, `device_info` (with an explicit `udid`) and `stream_info` (all
  zeros, `streaming:false`) answer normally.
- `select_device` records the choice and the waiting loop binds that device -- no restart. Once a
  device is bound (or binding), `select_device` restarts the engine on the new one as before;
  selecting the device that is bound, or the one currently binding, answers
  `{"rebinding":false}` and does nothing.
- Viewers/listeners on 9877/9878 that hang up while held are swept within ~1 s.
- Every method that talks to the device answers
  `{"ok":false,"error":{"code":"no_device","message":"no device bound -- the engine is waiting for a device"}}`
  (or `"... still bringing the device up"` while binding).
- Connections to 9877/9878 are accepted and held; their streams start when the device is ready.

## Windows

Built under MSYS2 UCRT64 (see the top-level build notes):
`make -C host-c IMD_PREFIX=/ucrt64 OPENSSL=/ucrt64` → `host-c/cdhost.exe`. No admin, no driver:
userspace networking (lwIP) is the default on every non-Apple build (`RPLAY_USERSPACE_NET=0` is
refused there -- there is no kernel tunnel path), and Apple Mobile Device Service provides usbmuxd.

- **Ctrl-C / Ctrl-Break / closing the console** end the engine like SIGINT/SIGTERM
  (`SetConsoleCtrlHandler`). `timeBeginPeriod(1)` is set at startup.
- **Rebind** (`select_device` once a device is bound) starts a NEW process (`CreateProcess`, same
  arguments, only the std handles inherited) and the old one exits -- **the PID changes**. The new
  engine retries its ports for up to 15 s while the old one releases them (`RPLAY_REBOUND=1`). A
  supervisor (the GUI) should treat the API answering again on 9876 as success, not the exit of
  the process it launched as a crash.
- Listeners use `SO_EXCLUSIVEADDRUSE`, so a second `cdhost.exe` fails to bind and exits.
- **DDI** lookup order: `RPLAY_DDI`, then `%LOCALAPPDATA%\rplayhub\iOS_DDI`, then `iOS_DDI\`
  beside `cdhost.exe`, then `..\deps\iOS_DDI` relative to it, then `$XDG_DATA_HOME`/`$HOME`
  (`.local/share/rplayhub/iOS_DDI`). In each, the Cryptex DDI (iOS 27, installed through cryptexd)
  is looked for in `Cryptex\` and then in a `Restore\` manifest with the cryptex identity; the
  personalized DDI in `Restore\`. See `doc/DDI.md`.
- `RPLAY_ATTACH_TUNNEL` is macOS-only (it parses `ifconfig` for a utun) and is refused on Windows.
- The 30 s bring-up watchdog is a thread on every platform now (no `alarm()`), and relaunches the
  engine rather than exiting; it exits with code 2 only if that relaunch fails.

## Wire notes

Same ground truth as `../host/README.md`: usbmux unix-socket plist framing, lockdown
`<u32 be len><XML plist>`, usbmux `PortNumber` in network byte order.

## TLS backend

Selectable at build time, because the choice is not obvious and because swapping it is the way to
*test* a suspicion rather than argue about it:

    make                # OpenSSL (default)
    make TLS=mbedtls    # currently refused by the Makefile -- see below

mbedtls is what `~/rplay` and `~/carplay-dev` use, and it is what makes the Linux and Windows ports
tractable — no system library to find, no version skew, no shipping question.

**It does not work yet for lockdown.** Measured against a real pair record, mbedtls rejects Apple's
host certificate with `-0x23E0` = `X509_INVALID_NAME` + `ASN1_OUT_OF_DATA`; the private key parses
fine. mbedtls is stricter than OpenSSL about the subject/issuer name encoding Apple uses. No config
flag changes that — it needs a permissive parse or converting the identity before handing it over.
So OpenSSL remains the default and mbedtls is a working skeleton with one known blocker.

Worth knowing for the ports: this blocker is in the **lockdown** path only. The tunnel, RemoteXPC
and every service call above it are plain sockets and carry no TLS of their own.

**Current state (2026-10):** the `tls.h` layer is vestigial. The CoreDeviceProxy TLS is now
libimobiledevice's, nothing calls `rp_tls_connect`, and `ddi.c` (the TSS request) and
libimobiledevice use OpenSSL directly -- so `make TLS=mbedtls` stops with an explanatory error
instead of failing to link. `../deps/mbedtls` is not vendored in this tree.
