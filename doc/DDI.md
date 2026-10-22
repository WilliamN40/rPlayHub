# The Developer Disk Image (DDI)

rPlayHub talks to the iPhone's developer services, and on iOS 17 and later those services only
answer once a **Developer Disk Image** is mounted on the phone. The phone forgets it on every
reboot, so it has to be mounted again each time.

**rPlayHub's engine mounts it for you** (`host-c/ddi.c`), with no Xcode and no Device Hub. What
it needs from you is the image files, and those are **Apple's software, which we cannot put in
this repository**. This page is how to get them.

## Two variants

| Variant | Used for | How the engine puts it on the phone | Folder |
|---|---|---|---|
| **Cryptex DDI** | iOS 27 and later | installed as a cryptex through `cryptexd` (`com.apple.security.cryptexd.remote`) | `iOS_DDI/Cryptex/` |
| **Personalized DDI** | iOS 17 to 26 | mounted through `mobile_image_mounter` | `iOS_DDI/Restore/` |

The two matter because **a phone newer than the DDI can only get the Cryptex variant signed.** The
personalized image's ticket is tied to the boards listed in its manifest. Build 27A5228h stops at
chip 0x8150, so an iPhone19,7 (chip 0x8160) has no entry. Borrowing another chip's entry gets
`STATUS=94&MESSAGE=This device isn't eligible for the requested build.` from Apple. The cryptex
identity in the same manifest is personalized by product class (`Cryptex1,UseProductClass`), not
by board, so it is not tied to the manifest's list of phones. Apple still decides whether to
sign it.

On iOS 27 the engine tries the Cryptex DDI first, which is also what pymobiledevice3's
`mounter auto-mount` does. It falls back to the personalized image only when the Cryptex files
are missing.

## Do you need to do anything?

| Your setup | What to do |
|---|---|
| Mac with **Xcode or Device Hub** installed | Nothing. The engine uses Xcode's copy in `/Library/Developer/DeveloperDiskImages/iOS_DDI`. |
| **Linux**, or a Mac **without Xcode** | Download the files once, below. |
| **Windows** | Download the files once: [Windows](#windows). |
| Phone already connected to a Mac running Xcode or Device Hub **since its last reboot** | Nothing for now: the image is already mounted. You will need the files after the next reboot. |

## Download

From a clone of this repository:

```sh
./scripts/fetch-ddi.sh
```

That downloads both variants (about 32 MB) into `~/.local/share/rplayhub/iOS_DDI/`, which is
where the engine looks:

```
iOS_DDI/Cryptex/   BuildManifest.plist  Image.dmg  Image.dmg.trustcache
                   Image.dmg.cryptex_info  Image.dmg.root_hash
iOS_DDI/Restore/   BuildManifest.plist  Image.dmg  Image.dmg.trustcache
```

The two variants use the same file names, so each gets its own folder. Then start the engine as
usual; there is nothing to configure.

The files come from [doronz88/DeveloperDiskImage](https://github.com/doronz88/DeveloperDiskImage),
the public mirror that [pymobiledevice3](https://github.com/doronz88/pymobiledevice3) downloads
from (`PersonalizedImages/Xcode_iOS_DDI_Cryptex` and `.../Xcode_iOS_DDI_Personalized`). They ship
with Xcode, and are the same for every iPhone. The default ref, `main`, has the newest iOS DDI the
mirror publishes. As of 2026-10-22 that is **27A5228h** for both variants. PR #29 ("Update DDIs to
27A266a") was closed without merging, and the newest commit (#34, 2026-09-30) added tvOS DDIs only.

### Without the script

```sh
base=https://github.com/doronz88/DeveloperDiskImage/raw/main/PersonalizedImages
mkdir -p ~/.local/share/rplayhub/iOS_DDI/Cryptex ~/.local/share/rplayhub/iOS_DDI/Restore
cd ~/.local/share/rplayhub/iOS_DDI/Cryptex
for f in Image.dmg Image.dmg.trustcache Image.dmg.cryptex_info Image.dmg.root_hash BuildManifest.plist; do
    curl -fLO $base/Xcode_iOS_DDI_Cryptex/$f
done
cd ../Restore
for f in Image.dmg Image.dmg.trustcache BuildManifest.plist; do
    curl -fLO $base/Xcode_iOS_DDI_Personalized/$f
done
```

### Windows

The installer adds a Start-menu entry, **rPlayHub → Fetch Developer Disk Image**, which runs
`fetch-ddi.ps1` from the install folder. From a clone of the repository it is:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\fetch-ddi.ps1
```

It downloads the same files from the same mirror as `fetch-ddi.sh`, into
`%LOCALAPPDATA%\rplayhub\iOS_DDI\Cryptex\` and `...\iOS_DDI\Restore\`. The engine looks there by itself, and the GUI also
passes that folder to the engine as `RPLAY_DDI` when it exists. `fetch-ddi.ps1 D:\some\dir`
downloads elsewhere; then set `RPLAY_DDI=D:\some\dir` for the engine. `DDI_REF` (or `-Ref`) picks
a branch, tag or commit of the mirror, as for the shell script.

**Verified on Windows 11 on 2026-10-22.** The setup was an iPhone19,7 (chip 0x8160, iOS 27.0.1,
24A446) on USB through the Apple Devices app's usbmuxd. The engine ran in userspace-networking
mode. `fetch-ddi.ps1` fetched 27A5228h. The engine installed the Cryptex DDI with one TSS request
(`com.apple.MobileAsset.DDI 27.1.5228.8`). RSD then went from 64 services to 85, displayservice
included, and a 14-second capture from the video port decoded to 558 frames with no errors. The
personalized variant cannot work on that phone (see [Two variants](#two-variants)).

### From a Mac with Xcode

Copy the whole folder `/Library/Developer/DeveloperDiskImages/iOS_DDI` to the other machine.
Xcode's `Restore/BuildManifest.plist` carries the cryptex identity next to the personalized ones,
so this one folder serves both variants. Then
either put it at `~/.local/share/rplayhub/iOS_DDI` (on Windows, `%LOCALAPPDATA%\rplayhub\iOS_DDI`)
or point the engine at it:

```sh
RPLAY_DDI=/path/to/iOS_DDI ./host-c/cdhost
```

### Somewhere else

`./scripts/fetch-ddi.sh /some/dir` downloads into another folder; run the engine with
`RPLAY_DDI=/some/dir`. The folder must contain `Cryptex/BuildManifest.plist` or
`Restore/BuildManifest.plist`.

## When the engine mounts it

Every time the engine starts, it checks whether an image is mounted, and mounts one if not. For
that the phone must be:

- **Unlocked.** The phone refuses the mount while locked. Unlock it and restart the engine.
- **In Developer Mode** (Settings → Privacy & Security → Developer Mode). Turning it on needs a
  reboot and a confirmation on the phone.
- **Paired** with this computer (the "Trust This Computer" prompt).

The computer needs **internet access** for the mount: the engine asks Apple's signing server
(`gs.apple.com`) for a ticket tied to your phone and this boot. No Apple ID is involved.

The Cryptex DDI is installed with persistence 2, as Xcode installs it. pymobiledevice3 found that
an install like this survived a reboot on a virtual iPhone. That has not been checked on a
physical phone here, and the engine checks and installs on every start anyway.

## How the Cryptex install works

This was ported from pymobiledevice3 11.42 (`services/cryptexd.py`, `restore/tss.py`
`add_cryptex1_tags`, `remote/remotexpc.py`). Each request is `{"routine": ..., "argv": {...}}` on
a fresh RemoteXPC connection to cryptexd:

1. `copy-installed`: already installed? Its `remote-cryptex-array` lists
   `com.apple.MobileAsset.DDI` when it is. The image mounter is also asked about a personalized
   image, which would hold the same mount point.
2. `read-personalization-id`: `img4_chip_chip`, `img4_chip_ecid` and `img4_chip_cpro`.
3. `get-nonce` with `nonce-domain-handle` = the identity's `Cryptex1,NonceDomain` (4). The value
   is a handle, not an index; index 4 is a different domain. The reply is a 2-byte lead, the
   48-byte nonce, then its length as a little-endian u32.
4. A TSS request carrying `@Cryptex1,Ticket` and the identity's `Cryptex1,*` tags.
   `Cryptex1,UDID` is the chip ID and ECID as 8 bytes each, big-endian. The request also carries
   the `Digest` of each component marked `Personalize: true` and no `Ap*` tags. The answer is a
   `Cryptex1,Ticket`.
5. `install` with `auth` 0, `client-version` 3, `cryptex1-properties` (all integers uint64),
   `image-type-index` 10, `nonce-persistence` 1 and `persistence` 2. The image, trust cache,
   ticket, info plist and volume hash are sent as XPC file transfers 1 to 5. Each payload then goes
   on its own client-initiated HTTP/2 stream (5, 7, 9...), under flow control. This is
   `rp_rxpc_send_file` in `core/rp_remotexpc.c`.

Afterwards the engine reads RSD again, because the new services are not in the map it read
before the install.

`RPLAY_TSS_DUMP=<file>` saves the ticket request, and `RPLAY_TSS_DRYRUN=1` stops before contacting
Apple, so the cryptexd queries can be checked without spending a signing request.

## What you will see

In the engine's output, under `Layer 3c: developer disk image`:

| Message | Meaning |
|---|---|
| `already mounted` | Nothing to do. |
| `mounted (no Xcode/Device Hub needed)` | The engine mounted it. On iOS 27 it then prints `Layer 3b again` with the new services. |
| `Apple's signing server (TSS) refused: STATUS=94&MESSAGE=...` | Apple will not sign that variant for this phone. For the personalized variant on a phone newer than the DDI, this is expected; fetch the Cryptex files. |
| `cryptexd install failed: <text> (<domain>: <code>)` | cryptexd rejected the install; the text is the phone's own. |
| `the device is locked -- unlock it, then reconnect` | Unlock the phone and restart the engine. |
| `no DDI files: run scripts/fetch-ddi.sh ...` | The files were not found. See [Download](#download) (on Windows, [Windows](#windows)). |
| `could not mount the DDI (rc=-4)` | Apple's signing server could not be reached, or refused. Check the internet connection. |

## Newer iOS versions

A DDI covers a range of iOS versions, and a new iOS release can need a newer one. If mounting
fails after an iOS update, fetch again — the mirror tracks Xcode releases — or copy the folder
from an up-to-date Xcode.
