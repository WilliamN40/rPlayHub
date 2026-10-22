#!/usr/bin/env bash
# ALTERNATIVE PATH -- the documented Windows build is MSYS2 UCRT64 (scripts/build-windows.sh),
# which builds FFmpeg with gcc via scripts/build-ffmpeg-rvra.sh. Use this only for the MSVC GUI
# build (scripts/build-gui-msvc.bat). It is not exercised by CI.
#
# Builds the RVRA-patched FFmpeg with MSVC as static .lib files into deps/ffmpeg-dist, from its own
# source tree deps/ffmpeg-msvc (deps/ffmpeg is the gcc build; mixing the two configurations in one
# tree breaks both). The source is the same release and the same patches/ffmpeg-rvra.patch as
# build-ffmpeg-rvra.sh, and the build REFUSES to run on an unpatched tree: an unpatched decoder
# links and runs fine and garbles every picture after the first resolution downshift, with no
# warning (doc/RVRA-AND-PORTABILITY.md).
#
# Run it from an MSYS2 shell (any MSYSTEM) that can see MSVC's cl.exe. Any of:
#   - start MSYS2 from an "x64 Native Tools Command Prompt for VS" with
#       C:\msys64\msys2_shell.cmd -msys -use-full-path
#   - or let this script find Visual Studio with vswhere and import vcvars64.bat itself
#   - or RPLAYHUB_MSVC_ENV=/path/to/script.sh, a bash script that puts cl/link/lib on PATH and
#     sets INCLUDE/LIB (e.g. a portable-msvc install's msvc-shell.sh); it is sourced
# MSYS2 needs: pacman -S make diffutils nasm (or mingw-w64-ucrt-x86_64-nasm) pkgconf.
# Note: MSYS2's /usr/bin/link.exe shadows MSVC's link.exe; this script checks for that.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
die() { echo "build-ffmpeg-msvc: error: $*" >&2; exit 1; }

# --- MSVC environment -------------------------------------------------------------------------
if [ -n "${RPLAYHUB_MSVC_ENV:-}" ]; then
    # shellcheck disable=SC1090
    source "$RPLAYHUB_MSVC_ENV"
fi
if ! command -v cl >/dev/null; then
    vswhere="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
    [ -x "$vswhere" ] || die "cl.exe not on PATH and no vswhere.exe -- start MSYS2 from a Developer prompt (see the header)"
    vs="$("$vswhere" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | tr -d '\r')"
    [ -n "$vs" ] || die "vswhere found no Visual Studio with the C++ x64 tools"
    vcvars="$vs\\VC\\Auxiliary\\Build\\vcvars64.bat"
    echo "importing the MSVC environment from $vcvars"
    # Run vcvars64 in cmd and import PATH, INCLUDE, LIB and LIBPATH from what it leaves behind.
    # Through a .bat file: quoting a path with spaces on a cmd /c line from MSYS is unreliable.
    envbat="$(mktemp -d)/msvcenv.bat"
    printf '@call "%s" >nul\r\n@set\r\n' "$vcvars" > "$envbat"
    while IFS='=' read -r k v; do
        v="${v%$'\r'}"
        case "${k^^}" in
            PATH)    export PATH="$(cygpath -u -p "$v"):$PATH" ;;
            INCLUDE) export INCLUDE="$v" ;;
            LIB)     export LIB="$v" ;;
            LIBPATH) export LIBPATH="$v" ;;
        esac
    done < <(cmd.exe //d //c "$(cygpath -w "$envbat")")
fi
command -v cl >/dev/null || die "cl.exe still not on PATH"
case "$(command -v link)" in
    /usr/bin/link*) die "MSYS2's /usr/bin/link shadows MSVC's link.exe -- put MSVC first on PATH (or: mv /usr/bin/link.exe /usr/bin/link-coreutils.exe)" ;;
esac
command -v nasm >/dev/null || die "nasm missing (pacman -S nasm, or mingw-w64-ucrt-x86_64-nasm)"

# --- source: the same release and patch as build-ffmpeg-rvra.sh --------------------------------
VERSION="$(sed -n 's/^VERSION=//p' scripts/build-ffmpeg-rvra.sh)"
[ -n "$VERSION" ] || die "cannot read VERSION from scripts/build-ffmpeg-rvra.sh"
TARBALL="deps/ffmpeg-$VERSION.tar.xz"
SRC=deps/ffmpeg-msvc
PREFIX="$root/deps/ffmpeg-dist"
if [ ! -d "$SRC" ]; then
    mkdir -p deps
    [ -f "$TARBALL" ] || curl -fL -o "$TARBALL" "https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz"
    rm -rf "deps/ffmpeg-$VERSION"
    tar xf "$TARBALL" -C deps
    mv "deps/ffmpeg-$VERSION" "$SRC"
fi
cd "$SRC"
if patch -p1 -N --dry-run -s < "$root/patches/ffmpeg-rvra.patch" >/dev/null 2>&1; then
    patch -p1 < "$root/patches/ffmpeg-rvra.patch"
elif patch -p1 -R --dry-run -s < "$root/patches/ffmpeg-rvra.patch" >/dev/null 2>&1; then
    echo "patch already applied"
else
    die "patches/ffmpeg-rvra.patch does not apply to $SRC -- remove $SRC and re-run"
fi
grep -q rvra_enable libavcodec/hevc/hevcdec.h \
    || die "$SRC is NOT RVRA-patched (no rvra_enable in libavcodec/hevc/hevcdec.h) -- refusing to build"

# --- build ------------------------------------------------------------------------------------
if [ ! -f ffbuild/config.mak ]; then
    ./configure --toolchain=msvc --enable-static --disable-shared --disable-doc \
        --disable-autodetect --disable-network --disable-programs \
        --prefix="$(cygpath -m "$PREFIX")"
fi
make -j"${JOBS:-4}"
make install
# build-gui-msvc.bat checks for this marker: the installed headers do not show whether the
# decoder inside the .lib files was patched.
grep -qa RPLAY_RVRA libavcodec/libavcodec.a 2>/dev/null || grep -qa RPLAY_RVRA libavcodec/avcodec.lib 2>/dev/null \
    || die "the built libavcodec has no RVRA code in it"
echo "ffmpeg $VERSION + $(sha256sum "$root/patches/ffmpeg-rvra.patch" | cut -c1-16) ffmpeg-rvra.patch" > "$PREFIX/RVRA-PATCHED"
echo "static RVRA-patched FFmpeg $VERSION installed to deps/ffmpeg-dist (MSVC)"
