#!/usr/bin/env bash
# Build rPlayHub for Windows -- the engine, the GUI and a runnable folder -- in one command.
#
# Run it inside an MSYS2 **UCRT64** shell (not Git Bash: it rewrites /ucrt64 paths). From Git
# Bash or PowerShell:
#
#   C:\msys64\usr\bin\bash -lc 'cd /d/rPlayHub && scripts/build-windows.sh'   # with MSYSTEM=UCRT64
#
# Steps, each idempotent, so a re-run only redoes what changed:
#   1. pacman packages (skip with RPLAYHUB_SKIP_PACMAN=1 -- CI installs them via setup-msys2)
#   2. lwIP 2.2.1 into deps/lwip, Dear ImGui + nlohmann/json (scripts/fetch-gui-deps.sh)
#   3. the RVRA-patched FFmpeg (scripts/build-ffmpeg-rvra.sh) -- and a hard check that the patch
#      is really in the tree: an unpatched FFmpeg links and runs fine and garbles every picture
#      after the first resolution downshift, with no warning (doc/RVRA-AND-PORTABILITY.md)
#   4. the engine:  make -C host-c IMD_PREFIX=/ucrt64 OPENSSL=/ucrt64  -> host-c/cdhost.exe
#   5. the GUI:     make -C client-c rplay-gui                         -> client-c/rplay-gui.exe
#   6. stage build/windows/rPlayHub/: both .exe, every non-system DLL they load (found
#      recursively from the PE import tables, taken from /ucrt64/bin), fonts, icon, licences,
#      THIRD-PARTY-NOTICES.txt and fetch-ddi.ps1
#   7. start both staged executables with PATH reduced to C:\Windows\System32, so a DLL that was
#      not staged fails here instead of on a user's machine
#
# scripts/package-windows.sh turns the staged folder into the installer and the portable zip.
#
# Environment:
#   RPLAYHUB_SKIP_PACMAN=1   do not run pacman
#   RPLAYHUB_SKIP_FFMPEG=1   do not (re)run the FFmpeg build; deps/ffmpeg must already be built
#                            and patched -- the patch check still runs
#   JOBS=N                   parallel make jobs for FFmpeg, engine and GUI (default: min(nproc, 4))
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
die()  { echo "build-windows: error: $*" >&2; exit 1; }
step() { echo; echo "== build-windows: $*"; }

[ "${MSYSTEM:-}" = UCRT64 ] || die "run inside MSYS2 UCRT64 (MSYSTEM=UCRT64), not '${MSYSTEM:-plain bash}'.
  From Git Bash:  MSYSTEM=UCRT64 CHERE_INVOKING=1 /c/msys64/usr/bin/bash -lc 'cd $root && scripts/build-windows.sh'"
# Capped at 4 by default: FFmpeg's largest files need ~1 GB per cc1, and with one per core
# MSYS2 builds have died with "cc1.exe: out of memory" on a shared machine.
JOBS="${JOBS:-$(n=$(nproc); [ "$n" -gt 4 ] && n=4; echo "$n")}"
export JOBS
version="$(sed -n 's/^RPLAYHUB_VERSION := \([0-9.]*\).*/\1/p' client-c/Makefile)"
[ -n "$version" ] || die "no RPLAYHUB_VERSION in client-c/Makefile"
stage="$root/build/windows/rPlayHub"

# ---------------------------------------------------------------------------------------------
step "1/7 packages"
UCRT_PKGS="gcc make pkgconf SDL2 zlib openssl nasm nsis python
           libimobiledevice libimobiledevice-glue libplist libusbmuxd"
MSYS_PKGS="make diffutils patch tar xz git curl zip"
if [ "${RPLAYHUB_SKIP_PACMAN:-0}" = 1 ]; then
    echo "skipped (RPLAYHUB_SKIP_PACMAN=1)"
else
    # --needed: already-installed packages are left alone, so this is a no-op on a set-up machine
    # and needs no network then.
    pacman -S --needed --noconfirm $(for p in $UCRT_PKGS; do echo "mingw-w64-ucrt-x86_64-$p"; done) $MSYS_PKGS
fi
for t in gcc g++ make pkg-config nasm objdump strip patch curl git; do
    command -v "$t" >/dev/null || die "missing tool: $t (install the packages above, or unset RPLAYHUB_SKIP_PACMAN)"
done
pkg-config --exists sdl2 || die "SDL2 not found by pkg-config (mingw-w64-ucrt-x86_64-SDL2)"
for h in libimobiledevice/libimobiledevice.h plist/plist.h usbmuxd.h openssl/ssl.h zlib.h; do
    [ -f "/ucrt64/include/$h" ] || die "/ucrt64/include/$h missing"
done
echo "toolchain: $(gcc --version | head -1)"
echo "libplist $(pkg-config --modversion libplist-2.0 2>/dev/null || echo ?), libimobiledevice $(pkg-config --modversion libimobiledevice-1.0 2>/dev/null || echo ?), libusbmuxd $(pkg-config --modversion libusbmuxd-2.0 2>/dev/null || echo ?), SDL2 $(pkg-config --modversion sdl2)"

# ---------------------------------------------------------------------------------------------
step "2/7 sources: lwIP, Dear ImGui, nlohmann/json"
if [ ! -d deps/lwip/src ]; then
    mkdir -p deps
    git clone --depth 1 --branch STABLE-2_2_1_RELEASE https://github.com/lwip-tcpip/lwip.git deps/lwip
else
    echo "deps/lwip present"
fi
./scripts/fetch-gui-deps.sh

# ---------------------------------------------------------------------------------------------
step "3/7 RVRA-patched FFmpeg"
if [ "${RPLAYHUB_SKIP_FFMPEG:-0}" = 1 ]; then
    echo "build skipped (RPLAYHUB_SKIP_FFMPEG=1)"
else
    ./scripts/build-ffmpeg-rvra.sh
fi
# The check that matters: an unpatched decoder is not a build failure, it is a garbled picture.
grep -q rvra_enable deps/ffmpeg/libavcodec/hevc/hevcdec.h \
    || die "deps/ffmpeg is NOT RVRA-patched (no rvra_enable in libavcodec/hevc/hevcdec.h).
  The GUI would build and then garble every picture after the first resolution downshift.
  Remove deps/ffmpeg and re-run, or apply patches/ffmpeg-rvra.patch by hand."
for a in libavcodec/libavcodec.a libswscale/libswscale.a libswresample/libswresample.a libavutil/libavutil.a; do
    [ -f "deps/ffmpeg/$a" ] || die "deps/ffmpeg/$a missing -- FFmpeg is not built"
done
grep -qa RPLAY_RVRA deps/ffmpeg/libavcodec/libavcodec.a \
    || die "deps/ffmpeg/libavcodec/libavcodec.a has no RVRA code in it -- the tree is patched but the archive predates it; re-run without RPLAYHUB_SKIP_FFMPEG"
echo "RVRA patch: present in source and in libavcodec.a"

# ---------------------------------------------------------------------------------------------
step "4/7 engine (host-c/cdhost.exe)"
make -C host-c -j"$JOBS" IMD_PREFIX=/ucrt64 OPENSSL=/ucrt64
[ -f host-c/cdhost.exe ] || die "host-c/cdhost.exe was not produced"

# ---------------------------------------------------------------------------------------------
step "5/7 GUI (client-c/rplay-gui.exe)"
make -C client-c -j"$JOBS" rplay-gui
[ -f client-c/rplay-gui.exe ] || die "client-c/rplay-gui.exe was not produced"
# The decoder the GUI actually linked must be the patched one (FFmpeg is static in the GUI; the
# patch's RPLAY_RVRA switch is the marker). A system FFmpeg picked up instead would fail here.
grep -qa RPLAY_RVRA client-c/rplay-gui.exe \
    || die "client-c/rplay-gui.exe does not contain the RVRA-patched decoder (no RPLAY_RVRA in it)"
echo "rplay-gui.exe: RVRA-patched decoder linked in"

# ---------------------------------------------------------------------------------------------
step "6/7 stage $stage"
rm -rf "$stage"
mkdir -p "$stage/fonts" "$stage/licenses"
cp host-c/cdhost.exe client-c/rplay-gui.exe "$stage/"
strip --strip-unneeded "$stage/cdhost.exe" "$stage/rplay-gui.exe"

# Every DLL the executables import that lives in /ucrt64/bin, and every DLL those import, to a
# fixed point. Read from the PE import tables (objdump) rather than by loading the program
# (ldd), so a DLL that fails to load still has its own imports found. Anything not in
# /ucrt64/bin is a Windows system DLL (KERNEL32, WS2_32, ucrtbase, api-ms-win-*) and stays out.
declare -A seen=()
queue=("$stage/cdhost.exe" "$stage/rplay-gui.exe")
while [ "${#queue[@]}" -gt 0 ]; do
    f="${queue[0]}"; queue=("${queue[@]:1}")
    for dll in $(objdump -p "$f" | sed -n 's/^\s*DLL Name: *//p' | tr -d '\r'); do
        key="${dll,,}"
        [ -n "${seen[$key]:-}" ] && continue
        seen[$key]=1
        src="/ucrt64/bin/$dll"
        [ -f "$src" ] || continue           # a system DLL
        cp "$src" "$stage/$dll"
        queue+=("$stage/$dll")
    done
done
# ldd as a cross-check: anything it resolves under /ucrt64 must have been staged above.
for f in "$stage"/*.exe; do
    for p in $(ldd "$f" 2>/dev/null | awk '$3 ~ /^\/ucrt64\// {print $3}'); do
        [ -f "$stage/$(basename "$p")" ] || die "$(basename "$f") loads $p but it was not staged"
    done
done

cp client-c/fonts/*.ttf "$stage/fonts/"
cp client-c/rplay-gui.ico "$stage/rplayhub.ico"
cp LICENSE "$stage/LICENSE.txt"
cp scripts/fetch-ddi.ps1 "$stage/fetch-ddi.ps1"

# Licence texts for what is inside (THIRD-PARTY-NOTICES.txt refers to these names).
L="$stage/licenses"; M=/ucrt64/share/licenses
cp patches/ffmpeg-rvra.patch               "$L/ffmpeg-rvra.patch"
cp deps/ffmpeg/COPYING.LGPLv2.1            "$L/LGPL-2.1.txt"
cp deps/ffmpeg/LICENSE.md                  "$L/FFmpeg-LICENSE.md"
cp deps/imgui/LICENSE.txt                  "$L/imgui-LICENSE.txt"
cp deps/lwip/COPYING                       "$L/lwIP-COPYING.txt"
# MSYS2 packages move their licence files between releases (the CI runner's gcc-libs had no
# COPYING.RUNTIME where this machine's did), so each is looked up: the listed paths first, then
# any file of that name under $M; a miss is reported loudly but does not stop the build.
lic() {   # lic <dest name> <file name> <candidate path>...
    local dest=$1 name=$2; shift 2
    local f
    for f in "$@"; do [ -f "$M/$f" ] && { cp "$M/$f" "$L/$dest"; return; }; done
    f="$(find "$M" -name "$name" -type f 2>/dev/null | head -1)"
    if [ -n "$f" ]; then cp "$f" "$L/$dest"; else echo "WARNING: licence $name not found for $dest" >&2; fi
}
lic SDL2-LICENSE.txt              LICENSE.txt                    SDL2/LICENSE.txt
lic OpenSSL-LICENSE.txt           LICENSE                        openssl/LICENSE openssl/LICENSE.txt
lic zlib-LICENSE.txt              LICENSE                        zlib/LICENSE
lic winpthreads-COPYING.txt       COPYING                        winpthreads/COPYING libwinpthread/COPYING
lic MinGW-w64-runtime.txt         COPYING.MinGW-w64-runtime.txt  crt/COPYING.MinGW-w64-runtime.txt
lic MinGW-w64.txt                 COPYING.MinGW-w64.txt          crt/COPYING.MinGW-w64.txt
lic GCC-RUNTIME-EXCEPTION.txt     COPYING.RUNTIME                gcc-libs/COPYING.RUNTIME gcc/COPYING.RUNTIME
lic GCC-RUNTIME-GPL-3.0.txt       COPYING3                       gcc-libs/COPYING3 gcc/COPYING3
cat > "$L/MIT-nlohmann-json.txt" <<'MIT'
MIT License

Copyright (c) 2013-2023 Niels Lohmann

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
MIT

# THIRD-PARTY-NOTICES.txt: the template plus what this build actually contains -- each bundled
# DLL with the MSYS2 package (and version) it came from.
ffmpeg_version="$(sed -n 's/^VERSION=//p' scripts/build-ffmpeg-rvra.sh)"
ffmpeg_configure="$(sed -n 's/^#define FFMPEG_CONFIGURATION "\(.*\)"$/\1/p' deps/ffmpeg/config.h 2>/dev/null)"
imgui_version="$(sed -n 's/^IMGUI_TAG=//p' scripts/fetch-gui-deps.sh)"
json_version="$(sed -n 's/^JSON_TAG=//p' scripts/fetch-gui-deps.sh)"
bundled="rPlayHub $version, built $(date -u +%Y-%m-%d) with $(gcc --version | head -1)."$'\n'
bundled+="rplay-gui.exe, cdhost.exe -- this repository (GPL-3.0-or-later)"$'\n'
for d in "$stage"/*.dll; do
    owner="$(pacman -Qqo "/ucrt64/bin/$(basename "$d")" 2>/dev/null | head -1 || true)"
    ver="$( [ -n "$owner" ] && pacman -Q "$owner" 2>/dev/null | cut -d' ' -f2 || true)"
    bundled+="$(basename "$d") -- ${owner:-unknown package} ${ver}"$'\n'
done
python3 - "$root/packaging/windows/THIRD-PARTY-NOTICES.txt" "$stage/THIRD-PARTY-NOTICES.txt" \
    "$ffmpeg_version" "${ffmpeg_configure:-see scripts/build-ffmpeg-rvra.sh}" "$imgui_version" "$json_version" "$bundled" <<'PY'
import sys
src, dst, ffv, ffc, imv, jsv, bundled = sys.argv[1:8]
t = open(src, encoding="utf-8").read()
for k, v in {"@FFMPEG_VERSION@": ffv, "@FFMPEG_CONFIGURE@": ffc, "@IMGUI_VERSION@": imv,
             "@JSON_VERSION@": jsv, "@BUNDLED@": bundled.rstrip("\n")}.items():
    t = t.replace(k, v)
# CRLF: this file is read in Notepad.
open(dst, "w", encoding="utf-8", newline="\r\n").write(t)
PY
sed -i 's/$/\r/' "$stage/LICENSE.txt"

# ---------------------------------------------------------------------------------------------
step "7/7 start the staged executables with a clean PATH"
# Windows searches the executable's own folder, then System32, then PATH. With PATH cut down to
# the system folders and the working directory elsewhere, a DLL that was not staged makes the
# process die with STATUS_DLL_NOT_FOUND (0xC0000135; bash reports 127) instead of being found in
# /ucrt64/bin the way it would be on this machine.
clean_path="/c/Windows/System32:/c/Windows:/c/Windows/System32/Wbem"
check_start() {   # check_start <exe> <args...>
    local exe="$1"; shift
    local out rc=0
    out="$(cd / && env PATH="$clean_path" "$exe" "$@" 2>&1)" || rc=$?
    echo "$out" | head -5 | sed 's/^/    /'
    case "$rc" in
        0) echo "  $(basename "$exe") $*: exit 0 -- OK" ;;
        127|53|3221225781|-1073741515) die "$(basename "$exe") failed to start: a DLL is missing from the staged folder (exit $rc)" ;;
        *) die "$(basename "$exe") $* exited $rc" ;;
    esac
}
check_start "$stage/cdhost.exe" --help
check_start "$stage/rplay-gui.exe" --help

echo
echo "build-windows: staged rPlayHub $version in build/windows/rPlayHub:"
( cd "$stage" && find . -type f | sed 's|^\./||' | sort | while read -r f; do
      printf '  %-40s %8s KB\n' "$f" "$(( $(stat -c %s "$f") / 1024 ))"; done )
echo "build-windows: next: scripts/package-windows.sh  (installer + portable zip)"
