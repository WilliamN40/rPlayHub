# Windows: feasibility and plan

## Status, 2026-10-22 — live mirroring works on Windows with a real iPhone

Verified on Windows 11 with an iPhone19,7 (iOS 27.0.1, build 24A446) over USB:

- **Apple Devices (Microsoft Store) alone is enough:** `AppleMobileDeviceProcess` serves usbmux
  on `127.0.0.1:27015`. iTunes is not needed (unknown 1 answered).
- **Trust and pairing work through Apple's Windows service** (unknown 2 answered). Lockdown
  failures now say what to do (locked, Trust pending, Trust declined).
- **Developer Mode:** the switch is hidden until a developer tool asks. The engine now asks itself
  (`com.apple.amfi.lockdown` action 0) and tells the user where to turn it on.
- **DDI:** the personalized DDI (27A5228h, and even Xcode 27.1's) has no identity for chip 0x8160.
  Apple's TSS refuses a borrowed one with "This device isn't eligible for the requested build".
  The **Cryptex DDI installed through cryptexd** works. RSD went from 64 services to 85. See
  `doc/DDI.md`.
- **Live video, audio, Info tab:** working. The phone's tiers are 1328×2896, 1088×1920 and
  720×1280, and its screen is 1320×2868.
- **Two decoder bugs fixed on the way:**
  - The trailer matcher only knew the iPhone 13 Pro's sizes.
  - Any matcher on the escaped bytes misses 720×1280, because `0x0500` ends in a zero and
    emulation prevention moves the `03`. The tail is now unescaped before matching
    (patch, `stream-core.c`, `rplay-view.c`).
- **Resolution downshifts were mostly ours.** Same phone, fast scrolling, comparable motion
  (39–41 frames over 20 KB per 15 s):

  | Request | Full resolution | Frames / 15 s |
  |---|---|---|
  | 60 fps, RCTL on (the earlier Windows default) | 42% | 290 |
  | 30 fps, RCTL on | 55% | 471 |
  | 30 fps, RCTL off | 93–99% | 633–854 |

  The Windows GUI now defaults to `RPLAY_FPS=30` and `RPLAY_RCTL=0`. This is very likely the
  cause of AGENTS.md's "our sessions downshift far more than Device Hub's" on every platform,
  but it has only been measured on this phone.
- **Touch** is now streamed live: a `touch` API method (down/move/up) over one persistent HID
  channel, instead of a whole tap or swipe sent on release over a fresh connection each time.

## Status, 2026-10-22 (earlier) — phases 0–5 implemented, phone run pending

Done and checked on Windows 11 without a phone:

- **Build:** `scripts/build-windows.sh` (MSYS2 UCRT64) builds RVRA FFmpeg, `cdhost.exe` and a
  fully static `rplay-gui.exe`. It stages `build/windows/rPlayHub/`. `scripts/package-windows.sh`
  makes a per-user NSIS installer and a portable zip.
- **Clean PATH:** both exes start from the staged layout.
- **CI:** `.github/workflows/windows.yml` written. It has not run on GitHub yet.
- **Engine:** every bug in the list below is fixed, plus the review findings: viewer leak,
  per-device backoff, watchdog relaunch, once-only relaunch.
- **Engine state:** it stays up with no device and reports `usbmuxd` / `engine_state` /
  `engine_error` / `binding` in `list_devices`. `make -C core test` passes under MSYS2.
- **GUI:** it starts the engine itself (Job Object, no admin, log in
  `%LOCALAPPDATA%\rplayhub\cdhost.log`). It tells the user when Apple Mobile Device Service is
  missing, and has a button to Apple Devices in the Microsoft Store.
- **GUI rendering:** D3D11 renderer, PerMonitorV2 manifest, Windows fonts and paths.
- **Video and input through the mock engine** (`scripts/mock_engine.py`, synthetic 1184×2576
  streams):
  - HEVC and H.264 both decode and render, with the crop and notch correct.
  - Taps and swipes land within one screen pixel.
  - The codec is detected from the bitstream itself.
  - Decode costs ~30% of one core at 30 fps and ~53% at 60 fps on an i7-14700T.
- **Fixed on the way:**
  - `patches/ffmpeg-rvra.patch` had a wrong hunk count since `21968b4`, so it applied nowhere.
  - `fe6a7a9` broke the Linux GUI build.

Still unverified, and only a phone can answer it: unknowns 1–4 below, the real RVRA resampling
path on Windows, and the AFC/syslog/DDI paths over lwIP. Not done: Authenticode signing (needs a
certificate), the GPU decode path (phase 6) and RemotePairing (phase 7).

Written 2026-10-22 from a source audit of `host-c/`, `core/` and `client-c/` at `fe6a7a9`. **Nothing
here was built or run** — the audit machine had no compiler toolchain beyond MSYS2's, no Apple
Mobile Device Service and no phone. Every "works" below means "the code says so", not "measured".

## Verdict: yes, and Windows is the easiest desktop to ship

Most of it already exists (commits `21968b4`, `9f01a86`, `fe6a7a9`). Three facts make Windows
tractable, and one of them makes it *easier* than macOS or Linux:

1. **No driver, no admin.** The engine's userspace TCP/IP stack (lwIP, `RPLAY_USERSPACE_NET=1`)
   replaces the utun/TUN interface entirely. macOS and Linux need root for the tunnel; Windows
   needs nothing — no Wintun, no TAP, no kernel-mode signing, no UAC prompt. The engine can be an
   ordinary child process of the GUI.
2. **Apple already ships usbmuxd for Windows.** Apple Mobile Device Service (from iTunes or the
   Apple Devices app) speaks the usbmux protocol on TCP `127.0.0.1:27015` and serves pair records
   and the SystemBUID. libusbmuxd uses that address on Windows by itself, and the engine never
   reads pair records from disk, so no path code is needed. pymobiledevice3's `lockdown
   start-tunnel` uses this exact path (usbmux → lockdown → CoreDeviceProxy) on Windows for iOS
   17.4+, which is strong outside evidence it works.
3. **RVRA is solved in portable code.** `patches/ffmpeg-rvra.patch` resamples reference pictures
   inside FFmpeg's software HEVC decoder (39 dB worst frame against VideoToolbox, per
   `doc/RVRA-AND-PORTABILITY.md`). It already has a `_WIN32` path. Nothing about it is
   Linux-specific.

What stands between today and a shippable Windows build is correctness bugs, an undocumented and
machine-specific build, no installer, no CI, and three things only a real phone can answer.

## The constraints that will not go away

| Constraint | Consequence | Mitigation |
|---|---|---|
| **Apple Mobile Device Service is required.** Without it there is no USB driver for the iPhone and no usbmuxd. | Users must install iTunes or Apple Devices (Microsoft Store). | Detect it (service present, `27015` accepting) and say exactly what to install. Long term, the RemotePairing door removes the dependency — over wifi, with no Apple software. Writing our own USB driver (WinUSB + our own usbmuxd) is possible but would fight Apple's driver for the device; not worth it. |
| **Decode is software-only and single-threaded.** The RVRA patch edits reference frames in place and disables itself if `thread_count != 1`. | 1184×2576 HEVC at 30–60 fps on one core. Fine on a modern desktop, the main risk on low-end laptops. | Measure first (phase 1). If it is short: slice threading may be safe since resampling happens in `hevc_frame_start` before slices decode — check whether the device sends more than one slice per picture. GPU decode (D3D11VA) bypasses the patch and garbles; making it work means resampling the reference surfaces with a compute shader before each submit. Research-grade; not on the critical path. Media Foundation cannot do it at all. |
| **Developer Disk Image** must be fetched (Apple's files, not ours to ship). | First-run step. | A PowerShell fetcher, or fetch from the GUI, into `%LOCALAPPDATA%\rplayhub\iOS_DDI`. |
| **SmartScreen** flags unsigned executables. | Scary first-run dialog. | Authenticode signing (Azure Trusted Signing is the cheapest route). Only user-mode binaries, so no EV/driver signing. |

## Unknowns that need a Windows PC and an iOS 27 phone

Answer these before building on top of them. Each is a single run.

1. **Does the Microsoft Store "Apple Devices" app alone expose `127.0.0.1:27015`?** iTunes
   certainly does. If only iTunes does, the install guidance changes.
2. **First-time trust through Apple's service.** An unpaired phone has to go through
   `SavePairRecord` against Apple's service — does the Trust prompt appear and the record persist?
3. **Decode headroom.** CPU per frame for the patched decoder at the device's full tier, on a
   mid-range laptop. This decides whether the GPU path matters.
4. Wifi devices: does Apple's Windows service list `ConnectionType=Network` entries the way the
   macOS one does? (Nice to have; USB is the product.)

## Current state, by area

### Engine (`host-c/`, `core/`)

Builds only under MSYS2/MinGW-w64 (pthreads via winpthreads, libimobiledevice, libplist,
OpenSSL, zlib); not under MSVC. The core path — device list → lockdown → CoreDeviceProxy → lwIP →
RSD → video/audio/HID — is plausibly working, as `21968b4` claims.

**Bugs, most severe first:**

1. **lwIP sockets handed to Winsock** in `api_server.c`. `relay_open` returns an lwIP fd (768+),
   but `afc_send` writes it with plain `send()` (`api_server.c:1410`), the syslog loop uses
   `select()`/`recv()` (`:1921-1930`), and ~28 sites `close()` it instead of `tun_close()`. Every
   AFC feature (file browsing, crash export, app install) and the console are broken in
   userspace mode — **on Linux too** when `RPLAY_USERSPACE_NET=1`.
2. **`compat.h` maps `close` → `closesocket`.** Combined with (1), `closesocket(768)` on Windows
   can close an *unrelated live socket*, since Winsock handles are kernel handle values in the same
   number range. `usernet_owns(fd)` is just `fd >= 768`, which has the same collision in the other
   direction. Track lwIP fds explicitly, and replace the macro with a `sock_close()` helper.
3. `SO_REUSEADDR` on the API listeners (`api_server.c:2687`) lets a second `cdhost` bind the same
   ports on Windows, defeating the "already running" check. Use `SO_EXCLUSIVEADDRUSE`.
4. The 30 s bring-up watchdog is `alarm()`, compiled out on Windows (`imd.c:231`, `:298`): a
   stuck connect hangs forever.
5. Device rebind re-execs via `execv` (`cdhost.c:658-702`): on Windows that is a new process with
   a new PID, unquoted arguments, and inheritable sockets. Rebind in-process, or `CreateProcess`.
6. No `SetConsoleCtrlHandler`; no `timeBeginPeriod(1)`, so swipe pacing and RTCP timers round to
   15.6 ms.
7. DDI lookup (`ddi.c:414-432`, `cdhost.c:937-944`) only works via `RPLAY_DDI`. `install_app`
   splits paths on `/` only (`api_server.c:1665`). `duration * 1000000L` overflows a 32-bit
   `long` past 2.1 s (`api_server.c:482`, `:529`).
8. `RPLAY_USERSPACE_NET` must be set by hand; on non-Apple builds it should be the default.
9. `tls_openssl.c`/`tls_mbedtls.c` are dead code (nothing calls `rp_tls_connect`); `ddi.c` uses
   OpenSSL directly. `TLS=mbedtls` would fail to link.
10. `core/test_rp_rtp.c` reads stdin in text mode, so the RTP byte-compare would corrupt on
    Windows — hidden by `|| true` in `core/Makefile:30`.

### GUI (`client-c/`)

SDL2 + Dear ImGui, MSVC-built, statically linked against the RVRA-patched FFmpeg. Feature parity
with the Linux client is largely there (`9f01a86`).

1. **`fe6a7a9` broke the Linux build**: `argb_vis` is used at `rplay-gui.cpp:4465`, declared at
   `:4469`.
2. The GUI never starts or stops `cdhost`; `start-rplay.bat` does, and nothing stops it on exit.
3. The "Direct3D11 renderer" is not enforced — no `SDL_HINT_RENDER_DRIVER`, and SDL2 may pick
   D3D9. Diagnostics labels are hardcoded and wrong ("D3D11/DXVA2", always "HEVC", "44.1 kHz" for
   a 48 kHz stream; `rplay-gui.cpp:4142-4143`).
4. No Windows paths for monospace/CJK fonts (`:1000-1015`); config under
   `%USERPROFILE%\.config` instead of `%APPDATA%`; file dialog has no owner window; no app
   manifest (DPI awareness).
5. Build scripts hardcode one developer's paths (`build-ffmpeg-msvc.sh:5,7,19`,
   `build-gui-msvc.bat:9`); `build-ffmpeg-msvc.sh` never fetches or patches FFmpeg, so it can
   silently link an **unpatched** decoder (which garbles with no warning). CMake links FFmpeg from
   `bin/` and misses `dwmapi winmm bcrypt secur32`. `SDL2.dll` is not copied next to the root exe
   that `start-rplay.bat` launches.

### Distribution

No installer, no signing, no Windows CI (`.github/workflows/linux.yml` is the only workflow, and
it does not run `make -C core test`), no doc for building `cdhost.exe`.

## The plan

Ordered so each phase ends in something verifiable. The phone-dependent checks are called out.

### Phase 0 — stop the bleeding (½ day)

- Fix the Linux build break (`argb_vis`).
- Fix the diagnostics labels to report the real codec, renderer and sample rate.
- Add `scripts/build-ffmpeg-msvc.sh` a guard that refuses to build against an unpatched tree
  (grep for `rvra_enable` in `libavcodec/hevc/hevcdec.h`).

### Phase 1 — one reproducible toolchain, and a first real run (2–3 days)

**Decision: MSYS2 UCRT64 for both the engine and the GUI.** The engine cannot build with MSVC
today (pthreads, libimobiledevice), the GUI can build with either, and FFmpeg builds natively in
MSYS2 with the existing `build-ffmpeg-rvra.sh`. One toolchain means one CI job and one set of
runtime DLLs. Keep the MSVC `.bat` as an option, not the documented path.

- `scripts/build-windows.sh`: install the UCRT64 packages (gcc, make, cmake, openssl, zlib, SDL2,
  libplist, libusbmuxd, libimobiledevice — or build the last three with `build-imd-stack.sh` as
  Linux CI does, if MSYS2's are older than libplist 2.3), clone lwIP 2.2.1, build patched FFmpeg,
  `make -C host-c IMD_PREFIX=/ucrt64 OPENSSL=/ucrt64`, `make -C client-c`.
- Link statically where possible; collect the rest with `ldd | grep ucrt64` into `dist/`.
- **On a real phone:** answer unknowns 1–3 above. Record the result as
  `builds/known-good-windows-*` with a README, as on the other platforms.

### Phase 2 — engine correctness (3–5 days)

In order: engine bugs 1–2 (lwIP fd tracking, the `close` macro), then 3–8. Bugs 1–2 also fix
Linux userspace mode. Each is small; the fd tracking is the only one with a design decision
(a bitmap of live lwIP fds owned by `usernet.c`, consulted by `usernet_owns`).

Verify with the existing contract: `scripts/mirror_check.py --tap` against `cdhost.exe`, plus a
manual pass over every Inspector tab (files, crashes, console, apps, install) — the AFC and
syslog paths are exactly the ones that are broken today.

### Phase 3 — one app, not two windows (3–4 days)

- **The GUI owns the engine.** Spawn `cdhost.exe` with `CreateProcess` (hidden console, log to
  `%LOCALAPPDATA%\rplayhub\cdhost.log`) and put it in a Job Object with
  `KILL_ON_JOB_CLOSE`, so it dies with the GUI. Reuse an engine already running on 9876. No
  elevation anywhere. This replaces both `.bat` files.
- **First-run checks, shown in the GUI rather than as a black window:** Apple Mobile Device
  Service missing → link to Apple Devices; no DDI → fetch it; phone below iOS 27 → say so (the
  engine already warns).
- DDI fetch in-app (port `scripts/fetch-ddi.sh` logic, or a PowerShell sibling).
- GUI fixes 3–4: force `direct3d11`, fonts from `C:\Windows\Fonts` (Consolas, Microsoft YaHei /
  Yu Gothic / Malgun), `%APPDATA%` config, owned file dialogs, an app manifest with
  per-monitor-v2 DPI awareness and a `asInvoker` execution level.

### Phase 4 — package and sign (2–3 days, plus certificate lead time)

- **Per-user installer** (no admin, since there is no driver): Inno Setup or NSIS (`makensis` is
  already in MSYS2). Installs to `%LOCALAPPDATA%\Programs\rPlayHub`, Start-menu entry,
  uninstaller. Also publish a portable `.zip`.
- Authenticode-sign `rplay-gui.exe`, `cdhost.exe`, every bundled DLL and the installer.
- Third-party notices: FFmpeg (LGPL, static, with `patches/ffmpeg-rvra.patch` and the
  corresponding source offer), libimobiledevice/libplist/libusbmuxd (LGPL), SDL2 (zlib), lwIP
  (BSD), OpenSSL (Apache-2.0), Dear ImGui (MIT), nlohmann/json (MIT). GPL-3.0 covers the whole.
- Later: a winget manifest.

### Phase 5 — CI (1–2 days)

`.github/workflows/windows.yml` on `windows-latest` with `msys2/setup-msys2` (UCRT64): build
everything via `scripts/build-windows.sh`, run `make -C core test` (after fixing engine bug 10 and
adding `.exe` suffixes), smoke-run `cdhost.exe` (it must start, report "no usbmuxd" cleanly, and
exit), build the installer, upload it as an artifact. Add `make -C core test` to the Linux job
while there.

### Phase 6 — performance (as measured in phase 1)

- If the CPU budget is fine: present path only — keep the `IYUV` upload as now, on the forced
  D3D11 renderer, and check frame pacing with the existing FPS counter.
- If it is not: first try slice threading under the patch; then the GPU path (D3D11VA decode,
  compute-shader reference resampling) as a research spike with its own doc.
- Windows on ARM: the same build under MSYS2 CLANGARM64. Cheap once CI exists.

### Phase 7 — beyond parity

- **RemotePairing direct-wifi door** — on Windows this also removes the Apple software
  dependency, which makes it worth more here than anywhere else.
- Wifi through Apple's service, if unknown 4 comes back positive.

## Estimate

Roughly **3–4 weeks of focused work to a signed beta installer**, assuming access to a Windows PC
and an iOS 27 phone from phase 1. Most risk sits in the three phone-dependent unknowns; the code
work is understood. The order matters more than the estimate: phase 1's real-phone run comes
before phase 2, because the repo's own history is that every expensive bug here was found by
running against the device, not by reading the code.
