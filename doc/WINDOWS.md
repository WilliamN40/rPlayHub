# rPlayHub on Windows

![rPlayHub on Windows mirroring an iPhone with native menus and inspector](images/rPlayHub-windows.png)

The same engine as macOS and Linux (`cdhost.exe`) and the same SDL2 + Dear ImGui GUI as Linux
(`rplay-gui.exe`), built with MSYS2's UCRT64 toolchain and shipped as a per-user installer and a
portable zip. No driver, no service and no administrator rights: the engine reaches the phone
through its own userspace TCP/IP stack (lwIP) instead of a TUN interface, and runs as an ordinary
child process of the GUI.

## Status — read this first

| | |
|---|---|
| Build (engine, GUI, RVRA FFmpeg) with `scripts/build-windows.sh` | **verified** on Windows 11, MSYS2 UCRT64, gcc 16 |
| Staged folder starts with no MSYS2 on `PATH` (no missing DLL) | **verified** by the build script, every build |
| Installer and portable zip build (`scripts/package-windows.sh`) | **verified** locally |
| Installer: silent install (`/S`), executables start from the install folder, uninstall removes files, shortcuts and registry entries | **verified** |
| Engine with no Apple Mobile Device Service: says so, keeps serving its API on `:9876` | **verified** |
| `make -C core test` under MSYS2 UCRT64 | **passes** |
| The interactive installer pages (license, components, finish text) | built, **not clicked through** |
| CI (`.github/workflows/windows.yml`) builds and packages on every push | written; **not yet run** on GitHub at the time of writing |
| `scripts/fetch-ddi.ps1` downloads the Developer Disk Image | **verified** (fetched build 27A5228h) |
| **Mirroring, control, DDI mount — anything with an iPhone** | **NOT TESTED.** No run of this build against a real phone yet |

The screenshot above is from an earlier, MSVC-built development version of the client; that run was
not recorded, and it is not evidence for the build described here. Until a Windows run against an
iOS 27 phone is written up (as `builds/known-good-windows-*` with a README, like the other
platforms), treat Windows as **building, not working**. The open questions only a phone can answer
are in [WINDOWS-PLAN.md](WINDOWS-PLAN.md#unknowns-that-need-a-windows-pc-and-an-ios-27-phone) —
among them whether the Microsoft Store **Apple Devices** app alone (without iTunes) exposes the
service the engine needs.

There is **no release** for Windows yet. CI is set up to build the installer as an artifact
(`rplayhub-windows` on each run of the `windows` workflow); it is unsigned.

## Requirements

- **Windows 10 or 11, x64.**
- **Apple Devices** (Microsoft Store) **or iTunes.** Either installs Apple's USB driver for the
  iPhone and **Apple Mobile Device Service**, which speaks usbmuxd on `127.0.0.1:27015`. rPlayHub
  cannot see a phone without it. (iTunes is known to provide the service; Apple Devices alone is
  expected to and not yet confirmed — see above.)
- **An iPhone on iOS 27 or later**, with **Developer Mode** on (Settings → Privacy & Security →
  Developer Mode; it needs a reboot and a confirmation on the phone), trusted with this PC once
  (the "Trust This Computer" prompt).
- **Apple's Developer Disk Image**, downloaded once — see [DDI.md](DDI.md#windows).
- **Internet access** when the engine mounts the disk image (Apple's signing server issues a
  per-phone ticket; no Apple ID involved).

## Install

**Installer:** run `rPlayHub-<version>-setup-x64.exe`. It installs for the current user only, to
`%LOCALAPPDATA%\Programs\rPlayHub`, with no UAC prompt, and adds:

- Start menu → **rPlayHub**, **Fetch Developer Disk Image**, **Uninstall rPlayHub**
- optionally a desktop shortcut
- an entry in Settings → Apps → Installed apps, which uninstalls it

**Portable:** unzip `rPlayHub-<version>-windows-x64.zip` anywhere and run
`rPlayHub\rplay-gui.exe`. Fetch the disk image with
`powershell -ExecutionPolicy Bypass -File rPlayHub\fetch-ddi.ps1`.

The builds are **not code-signed** yet, so SmartScreen shows "Windows protected your PC" the first
time: **More info → Run anyway**. (Signing is wired into the packaging script; see below.)

## Run

1. Install Apple Devices or iTunes, plug the phone in by USB, unlock it, tap **Trust**.
2. Start menu → **Fetch Developer Disk Image** (once; about 16 MB).
3. Start **rPlayHub**. The GUI starts the engine itself — hidden, in a Job Object so it exits with
   the GUI — and reuses one that is already answering on `127.0.0.1:9876`. Select the phone in the
   sidebar. Keep the phone unlocked the first time after each phone reboot, so the disk image can
   be mounted.

To run the engine on its own, with its output in a console — to watch it, pass flags
(`cdhost.exe --help`), or drive it with `scripts/mirror_check.py` — use `run-host.bat` from a
source checkout, or run `cdhost.exe` from the install folder, and start the GUI with
`rplay-gui.exe --no-engine`. `rplay-gui.exe --help` lists the GUI's options.

### Where things are

| | |
|---|---|
| `%LOCALAPPDATA%\Programs\rPlayHub\` | the installed program (`rplay-gui.exe`, `cdhost.exe`, DLLs, fonts, licences) |
| `%LOCALAPPDATA%\rplayhub\cdhost.log` | the engine's output for the current GUI session (`cdhost.prev.log`: the previous one) |
| `%LOCALAPPDATA%\rplayhub\rplay-gui.log` | the GUI's own output when it was not started from a console |
| `%LOCALAPPDATA%\rplayhub\iOS_DDI\` | the Developer Disk Image (`fetch-ddi.ps1`) |

Uninstalling removes the program and its shortcuts and leaves `%LOCALAPPDATA%\rplayhub\` (logs and
the disk image) in place; delete that folder by hand to remove everything.

## Troubleshooting

**No devices listed / `usbmuxd is not reachable -- Apple Mobile Device Service is not running`**
(in `cdhost.log`). Apple Mobile Device Service is missing or not running. The engine keeps
waiting with its API up (verified); that it then picks the service up once it starts is intended
but not yet verified, so restart rPlayHub if the phone does not appear. Check in PowerShell:

```powershell
Get-Service "Apple Mobile Device Service"                 # Status should be Running
Test-NetConnection 127.0.0.1 -Port 27015                  # TcpTestSucceeded should be True
```

If the service does not exist, install Apple Devices from the Microsoft Store (or iTunes). If it is
stopped, start it from `services.msc` or `Start-Service "Apple Mobile Device Service"` in an
administrator PowerShell. Then unplug and replug the phone, unlock it, and accept **Trust**. If
the phone does not appear in Apple Devices / iTunes either, the problem is the cable or Apple's
driver, not rPlayHub.

**The engine does not start.** Read `%LOCALAPPDATA%\rplayhub\cdhost.log`. If the GUI says
`cdhost.exe was not found`, `cdhost.exe` is not next to `rplay-gui.exe` — reinstall, or keep the
portable folder intact. If something else already listens on `127.0.0.1:9876`/`9877`, the GUI uses
it as the engine; stop the other program (`Get-NetTCPConnection -LocalPort 9876`).

**"Developer Disk Image not found" / services never appear.** Run **Fetch Developer Disk Image**,
unlock the phone, and restart rPlayHub. The engine log's `Layer 3c: developer disk image` section
says what happened; [DDI.md](DDI.md#what-you-will-see) explains each message. Developer Mode must
be on.

**Phone below iOS 27.** Not supported; the engine says so in its log. Use rPlay instead.

**High CPU while mirroring.** Expected for now. The phone's stream needs RVRA reference resampling
(`patches/ffmpeg-rvra.patch`), which runs only in FFmpeg's software HEVC decoder, single-threaded:
1184×2576 at up to 60 fps on one core. GPU decode (D3D11VA) bypasses the patch and garbles the
picture, so it is not used. Fine on a modern desktop; untested on low-end laptops.

**Firewall prompts.** None should appear: the engine and GUI talk only over loopback
(`127.0.0.1:9876`, `9877`, and Apple's `27015`), and the engine's connection to the phone runs
inside the USB tunnel, over its own userspace TCP/IP stack, not a Windows network interface. If
Windows Defender Firewall does ask, that is unexpected; please report it.

**SmartScreen / antivirus.** The binaries are unsigned. SmartScreen: **More info → Run anyway**.
An antivirus quarantining `cdhost.exe` is a false positive; restore it and add an exclusion for
the install folder.

## Build from source

Everything — engine, GUI and FFmpeg — builds with **MSYS2 UCRT64** (gcc). One toolchain means one
set of runtime DLLs and one CI job; the engine does not build with MSVC (pthreads,
libimobiledevice).

1. Install [MSYS2](https://www.msys2.org) to `C:\msys64`.
2. From a clone of this repository, in an **MSYS2 UCRT64** shell (Start menu → "MSYS2 UCRT64"; not
   Git Bash, which rewrites `/ucrt64` paths):

   ```sh
   scripts/build-windows.sh        # packages, lwIP, GUI deps, RVRA FFmpeg, engine, GUI, staging
   scripts/package-windows.sh      # installer + portable zip
   ```

   From Git Bash or PowerShell, enter MSYS2 explicitly:

   ```sh
   MSYSTEM=UCRT64 CHERE_INVOKING=1 /c/msys64/usr/bin/bash -lc 'cd /d/rPlayHub && scripts/build-windows.sh'
   ```

`build-windows.sh` installs the MSYS2 packages it needs (`pacman -S --needed`; skip with
`RPLAYHUB_SKIP_PACMAN=1`), clones lwIP 2.2.1 into `deps/lwip`, fetches Dear ImGui and
nlohmann/json, builds the RVRA-patched FFmpeg into `deps/ffmpeg` (the first time takes a while;
`JOBS=N` caps parallelism, default 4 — more has run out of memory), then

```sh
make -C host-c IMD_PREFIX=/ucrt64 OPENSSL=/ucrt64     # -> host-c/cdhost.exe
make -C client-c rplay-gui                             # -> client-c/rplay-gui.exe
```

and stages a runnable folder in `build/windows/rPlayHub/`: both executables, every non-system DLL
they import (found recursively from the PE import tables, copied from `/ucrt64/bin`), fonts, icon,
`LICENSE.txt`, `THIRD-PARTY-NOTICES.txt` (with the exact package version of every bundled DLL),
licence texts, `licenses/ffmpeg-rvra.patch` and `fetch-ddi.ps1`. It ends by starting both staged
executables with `PATH` reduced to `C:\Windows\System32`, so a DLL that was not bundled fails the
build instead of a user's first run.

It **refuses to continue on an unpatched FFmpeg** (no `rvra_enable` in
`deps/ffmpeg/libavcodec/hevc/hevcdec.h`, or no RVRA code in `libavcodec.a`). An unpatched decoder
links and runs, then garbles every picture after the phone's first resolution downshift with no
warning — see [RVRA-AND-PORTABILITY.md](RVRA-AND-PORTABILITY.md).

`package-windows.sh` builds `build/windows/rPlayHub-<version>-setup-x64.exe` with NSIS
(`packaging/windows/rplayhub.nsi`) and `build/windows/rPlayHub-<version>-windows-x64.zip`. The
version is `RPLAYHUB_VERSION` in `client-c/Makefile`, shared with the Linux package.

**Signing.** Set `RPLAYHUB_SIGN_CMD` to a command that signs one file given as its last argument,
and `package-windows.sh` runs it on every `.exe` and `.dll`, on the uninstaller as NSIS generates
it, and on the installer:

```sh
export RPLAYHUB_SIGN_CMD='"/c/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/signtool.exe" sign /fd SHA256 /tr http://timestamp.acs.microsoft.com /td SHA256 /dlib C:\\path\\Azure.CodeSigning.Dlib.dll /dmdf C:\\path\\metadata.json'
scripts/package-windows.sh
```

Without it the output is unsigned. Only user-mode binaries are involved, so an ordinary (non-EV)
Authenticode certificate or Azure Trusted Signing is enough.

**CI.** `.github/workflows/windows.yml` runs the same two scripts on `windows-latest` with
`msys2/setup-msys2`, plus the core tests and a smoke run of the staged engine with no phone and no
Apple service, and uploads the installer and zip.

### Alternative: the GUI with MSVC

Not the documented path, and not run by CI. `scripts/build-ffmpeg-msvc.sh` builds the RVRA-patched
FFmpeg with MSVC into `deps/ffmpeg-dist` (from its own tree, `deps/ffmpeg-msvc`, and it refuses an
unpatched one), and `scripts/build-gui-msvc.bat` builds `client-c\rplay-gui.exe` against it and the
SDL2 VC package from `scripts\fetch-gui-deps-win.ps1`. Both find Visual Studio with `vswhere`, or
run from an "x64 Native Tools Command Prompt". The engine still comes from the MSYS2 build.
