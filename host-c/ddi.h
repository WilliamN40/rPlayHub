/* ddi.h -- mount the personalized Developer Disk Image without Xcode or Device Hub.
 *
 * iOS 17+ discards the DDI on every reboot, and the CoreDevice services rPlayHub needs go quiet
 * until one is mounted again. This does what Device Hub does silently on connect: query the
 * phone's identity and per-boot nonce over mobile_image_mounter, ask Apple's TSS server for a
 * ticket bound to this device and boot, upload the image, and mount it. Ported from the proven
 * host/ddi_mount.py (2026-08-23). OpenSSL only (the engine's TLS backend anyway). */
#ifndef RP_DDI_H
#define RP_DDI_H

#include <stddef.h>

/* Return codes for cdhost_ddi_activate. */
#define RP_DDI_OK            0   /* mounted now, or already mounted */
#define RP_DDI_ALREADY       1   /* was already mounted; nothing done */
#define RP_DDI_LOCKED       -1   /* device is locked -- unlock and retry */
#define RP_DDI_NO_DDI       -2   /* the DDI files were not found on disk */
#define RP_DDI_NO_SERVICE   -3   /* the device did not offer mobile_image_mounter */
#define RP_DDI_TSS_FAILED   -4   /* Apple's signing server refused or was unreachable */
#define RP_DDI_ERR          -5   /* other failure (see stderr) */

/* Mount the DDI for the device at `tunnel_addr`, whose mobile_image_mounter is at `mounter_port`.
 * `ddi_dir` is the directory holding Restore/BuildManifest.plist (+ the .dmg and trust cache);
 * NULL means look beside the executable, then the system Xcode location. Idempotent: returns
 * RP_DDI_ALREADY when an image is already mounted. Writes progress to stderr. */
int cdhost_ddi_activate(const char *tunnel_addr, long mounter_port, const char *ddi_dir);

/* Install the Cryptex DDI through cryptexd (com.apple.security.cryptexd.remote at `cryptexd_port`):
 * the iOS 27 path, and the only one for phones newer than the DDI's manifest. `mounter_port` (may
 * be 0) is only asked whether a personalized image already holds the mount point. RP_DDI_NO_DDI
 * when the Cryptex files are not found (<dir>/Cryptex or a Restore manifest with the cryptex
 * identity, in the same directories as above). Writes progress, Apple's refusal text and
 * cryptexd's error text to stderr. RPLAY_TSS_DUMP=<file> saves the ticket request;
 * RPLAY_TSS_DRYRUN=1 stops before contacting Apple. */
int cdhost_ddi_install_cryptex(const char *tunnel_addr, long cryptexd_port, long mounter_port, const char *ddi_dir);

/* The DDI step the engine and the activate_ddi API run: the Cryptex DDI from iOS 27 when cryptexd is
 * advertised (falling back to the personalized image when the Cryptex files are missing), the
 * personalized image below that -- pymobiledevice3's auto_mount. */
int cdhost_ddi_auto(const char *tunnel_addr, long mounter_port, long cryptexd_port,
                    const char *product_version, const char *ddi_dir);

/* True if a personalized image is mounted (CopyDevices). */
int cdhost_ddi_is_mounted(const char *tunnel_addr, long mounter_port);

#endif
