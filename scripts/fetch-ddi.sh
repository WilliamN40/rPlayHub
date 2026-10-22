#!/usr/bin/env bash
# Download Apple's Developer Disk Image, which the engine mounts on the phone after every reboot.
#
# iOS 17+ drops the DDI on reboot, and the CoreDevice services rPlayHub uses stay quiet until one
# is mounted again. The engine mounts it by itself (host-c/ddi.c): it asks Apple's signing server
# for a ticket bound to your phone, then uploads the image. What it needs from you is the image
# files -- and those are Apple's software, which this repository cannot redistribute.
#
# On a Mac with Xcode (or Device Hub) installed there is nothing to do: the engine finds Xcode's
# copy in /Library/Developer/DeveloperDiskImages/iOS_DDI. Everywhere else -- Linux, a Mac without
# Xcode -- this fetches the same files from doronz88/DeveloperDiskImage, the public mirror that
# pymobiledevice3 downloads from, into the folder the engine looks in. Both variants are fetched:
# the Cryptex DDI (iOS 27, installed through cryptexd; the only one Apple signs for phones newer
# than the DDI, e.g. the iPhone 18 series) into <dest>/Cryptex, and the personalized DDI
# (iOS 17-26) into <dest>/Restore:
#
#   ./scripts/fetch-ddi.sh                 # -> ~/.local/share/rplayhub/iOS_DDI
#   ./scripts/fetch-ddi.sh /some/dir       # -> /some/dir, then run the engine with RPLAY_DDI=/some/dir
#
# DDI_REF picks a branch, tag or commit of that repository (default: main).
set -euo pipefail

DEST="${1:-${XDG_DATA_HOME:-$HOME/.local/share}/rplayhub/iOS_DDI}"
REF="${DDI_REF:-main}"
IMAGES="https://github.com/doronz88/DeveloperDiskImage/raw/$REF/PersonalizedImages"

# fetch <url> <dir> <files...> -- the manifest goes last: a folder with a manifest is one the
# engine will try to use, so it must not appear before the files it names are complete.
fetch() {
    local url="$1" dir="$2"; shift 2
    mkdir -p "$dir"
    for f in "$@"; do
        echo "fetching $(basename "$dir")/$f"
        curl -fL --retry 3 -o "$dir/$f.part" "$url/$f"
        mv "$dir/$f.part" "$dir/$f"
    done
}

# The Cryptex DDI: <dir>/Cryptex/BuildManifest.plist, whose "Developer Disk Image Cryptex" identity
# names the other four files. They share names with the personalized files, hence their own folder.
fetch "$IMAGES/Xcode_iOS_DDI_Cryptex" "$DEST/Cryptex" \
    Image.dmg Image.dmg.trustcache Image.dmg.cryptex_info Image.dmg.root_hash BuildManifest.plist

# The personalized DDI: <dir>/Restore/BuildManifest.plist; the engine takes the image and
# trust-cache paths from the manifest, which in this copy are Image.dmg and Image.dmg.trustcache.
fetch "$IMAGES/Xcode_iOS_DDI_Personalized" "$DEST/Restore" Image.dmg Image.dmg.trustcache BuildManifest.plist

build_of() {
    command -v python3 >/dev/null || { echo "?"; return; }
    python3 -c 'import plistlib,sys; print(plistlib.load(open(sys.argv[1],"rb")).get("ProductBuildVersion","?"))' \
            "$1/BuildManifest.plist" 2>/dev/null || echo "?"
}
echo
echo "DDI in $DEST -- Cryptex build $(build_of "$DEST/Cryptex"), personalized build $(build_of "$DEST/Restore")"
case "$DEST" in
    "${XDG_DATA_HOME:-$HOME/.local/share}/rplayhub/iOS_DDI") echo "the engine finds it there on its own." ;;
    *) echo "run the engine with RPLAY_DDI=$DEST" ;;
esac
echo "keep the phone unlocked when the engine starts: the phone refuses the mount while locked."
