/*
 * api_server.h — the daemon's side of the contract the app already speaks.
 *
 * The Swift player talks newline-delimited JSON on one port and reads a raw Annex-B byte stream
 * on another. That contract is defined in app/api/PROTOCOL.md and is already implemented by the
 * Python engine, so serving exactly the same thing from C means the app connects to either
 * without knowing the difference — which is what makes the port verifiable one method at a time
 * instead of as a single flag day.
 *
 * Unimplemented methods return an explicit error naming themselves. A daemon that answers
 * plausibly to something it cannot do is worse than one that refuses.
 */
#ifndef API_SERVER_H
#define API_SERVER_H

#include "../core/rp_remotexpc.h"

#define API_PORT    9876
#define STREAM_PORT 9877
/* The device's sound: AAC-ELD, 48 kHz stereo, 480 samples a frame, one frame per record --
 * <u16 BE length><u32 BE RTP timestamp><frame>. Negotiated when the first listener connects. */
#define AUDIO_PORT  9878

/* One attached device, as usbmuxd reports it.
 *
 * The daemon streams from exactly one device, but the sidebar should show every device the Mac
 * can see -- Device Hub does, and a list that hides the phone you are looking for is worse than
 * no list. Only the bound one can be mirrored; the rest are listed so they can be selected. */
typedef struct {
    char udid[64];
    char connection[16];          /* "USB" or "Network", as usbmuxd spells it */
    int  device_id;               /* usbmuxd's handle, for connecting to the device's lockdown */
    char name[128];               /* DeviceName, ProductVersion, ProductType from lockdown; */
    char version[32];             /* empty when the device did not answer (not paired, asleep) */
    char product_type[32];
} api_device;

#define API_MAX_DEVICES 16

/* Fills `out` with the attached devices, returning how many -- or -1 when usbmuxd itself cannot
 * be reached (no daemon; on Windows, Apple Mobile Device Service not installed or not running),
 * which is a different answer from "reachable, zero devices". Implemented in imd.c. */
int usbmux_enumerate(api_device *out, int max);

/* What the GUI should tell the user when usbmuxd is unreachable (list_devices' usbmuxd_error). */
#ifdef _WIN32
#define USBMUXD_MISSING_MESSAGE "Apple Mobile Device Service is not running. Install Apple Devices (Microsoft Store) or iTunes."
#elif defined(__APPLE__)
#define USBMUXD_MISSING_MESSAGE "usbmuxd is not reachable (/var/run/usbmuxd). It is part of macOS; a reboot normally restores it."
#else
#define USBMUXD_MISSING_MESSAGE "usbmuxd is not running. Install it (e.g. apt install usbmuxd) and plug the iPhone in, or start it with 'sudo systemctl start usbmuxd'."
#endif

/* The Info tab's data for `udid`: lockdown GetValue over a session, rendered as JSON in the shape
 * host/deviceinfo.py produced ({"default":{..},"com.apple.mobile.battery":{..},...,
 * "unavailable":{..},"session_error":..}). Returns non-zero with {"error":..} in `json`. */
int cdhost_device_info(const char *udid, char *json, size_t cap);

/* Which of `devs` a bring-up would bind: the first whose udid starts with `want` when that is
 * non-empty, else the USB entry over a wifi one. -1 when none qualifies. */
int imd_pick_device(const api_device *devs, int n, const char *want);

/* Stage-3 tunnel bringup via libimobiledevice (host-c/imd.c). Does Layers 0-2 and returns a
 * connected, TLS'd CoreDeviceProxy channel as an opaque idevice_connection_t in *out_conn; the
 * caller wraps it in a conn_t and runs the CoreDeviceProxy handshake + pump over it. Binds the
 * device imd_pick_device(want) chooses and fills its udid, name and iOS version. Returns 0 on
 * success; on failure everything it opened is released, so the caller may simply try again.
 * Guarded by a 30 s watchdog that exits the process if a library call hangs (see imd.c). */
int imd_bringup(const char *want, void **out_conn, char *udid_out, size_t udidlen,
                char *devname, size_t dnlen, char *prodver, size_t pvlen);
/* Why the last imd_bringup failed, phrased for the user ("" when there is nothing specific):
 * locked, waiting for Trust, Trust declined, Developer Mode off. */
const char *imd_last_error(void);
int imd_conn_send(void *conn, const void *buf, size_t n);
int imd_conn_recv(void *conn, void *buf, size_t n);
/* Drop a connection imd_bringup returned (and the device handle it rides on), when bring-up fails
 * after it -- e.g. the CoreDeviceProxy handshake -- and the caller goes back to waiting. */
void imd_conn_close(void *conn);

/* Bind a different device. Does not return on success.
 *
 * Implemented by re-executing this daemon with the new udid, rather than by unwinding the session
 * in place. Startup is a linear sequence -- lockdown, TLS, CoreDeviceProxy, utun, two pump
 * threads, RSD, media -- with no teardown path, and writing one only to use it here would be a
 * lot of new code whose failure mode is leaked utun interfaces and threads.
 *
 * Re-exec is not a dodge: binding another device needs a fresh tunnel and a fresh media session
 * anyway, since the device permits one stream per session and will not restart it. The disconnect
 * is inherent to the operation, not a cost of doing it this way, and the kernel releases every
 * resource for free. */
void cdhost_rebind(const char *udid);

/* Everything the server needs to answer questions about the session it belongs to. */
typedef struct {
    const char *udid;
    const char *device_name;
    const char *product_version;
    const char *tunnel_addr;      /* device address inside the tunnel */
    long        rsd_port;
    int         screen_w, screen_h;

    /* RSD-discovered service ports, 0 when the device did not offer them. */
    long        screenshot_port;
    long        display_port;
    long        hid_port;
    long        diag_port;            /* diagnostics_relay.shim.remote: restart/shutdown/sleep */
    long        app_port;             /* coredevice.appservice: list/launch/terminate apps */
    long        syslog_port;          /* syslog_relay.shim.remote: the console stream */
    long        instproxy_port;       /* installation_proxy.shim.remote: the app list */
    long        misagent_port;        /* misagent.shim.remote: provisioning profiles */
    long        mcinstall_port;       /* MCInstall.shim.remote: configuration profiles */
    long        afc_port;             /* afc.shim.remote: the Media partition */
    long        crashcopy_port;       /* crashreportcopymobile.shim.remote: AFC over crash reports */
    long        crashmover_port;      /* crashreportmover.shim.remote: poke before listing */
    long        mounter_port;         /* mobile_image_mounter.shim.remote: DDI self-activation */
    long        cryptexd_port;        /* security.cryptexd.remote: the iOS 27 Cryptex DDI install */
    long        sbservices_port;      /* springboardservices.shim.remote: per-app icon PNGs */
    /* Candidates for Device Hub's Settings tab (appearance/accessibility actions). Which of these
     * actually answers them is not yet known -- see doc/COREDEVICE-ACTIONS.md -- so all three are
     * resolved and `settings_probe` tries each with a read-only action. */
    long        configuration_port;   /* coredevice.configuration */
    long        devicecontrol_port;   /* coredevice.devicecontrol */
    long        deviceinfo_port;      /* coredevice.deviceinfo */
    long        location_port;        /* coredevice.locationservice: simulated location */
    const char *ddi_dir;             /* where the bundled DDI lives, or NULL to search defaults */

    /* Needed to negotiate the media stream: the offer carries our address, and the SSRC in it
     * must be the same value the RTCP session uses. */
    const char *our_addr;
    uint32_t    ssrc;
    double      keyframe_every_s;
} api_session;

/* Where the engine stands, as list_devices reports it ("engine_state"). */
typedef enum {
    API_ENGINE_WAITING = 0,   /* API up, no session: no device yet, or usbmuxd unreachable */
    API_ENGINE_BINDING,       /* a device was found and bring-up is in progress */
    API_ENGINE_READY          /* a session is bound; every method is available */
} api_engine_state;

/* Bind 9876/9877/9878 and start serving on a background thread, before any device is bound.
 * `session` must stay valid for the process lifetime; its fields are read only once the state is
 * READY. Returns non-zero if the listeners could not be created (another cdhost running). */
int  api_start(api_session *session);
void api_set_engine_state(api_engine_state st);
api_engine_state api_engine_state_get(void);
/* The last bring-up failure, shown as list_devices' "engine_error" while not ready; NULL clears. */
void api_set_engine_error(const char *msg);
/* Close every listening and streaming socket (the Windows rebind path, so the relaunched engine
 * can bind the same ports). The API stops answering. */
void api_close_listeners(void);
/* Block until the accept loop stops (it normally never does). */
void api_wait(void);

/* The userspace tunnel's connection ended (usernet.c's reader). Restarts the engine so it goes
 * back to waiting for the device instead of serving a dead session. Does not return. */
void cdhost_tunnel_died(const char *why);

/* The 30 s bring-up watchdog fired (imd.c, on its own thread): relaunch the engine with the same
 * device preference, the stuck device starting in backoff. Exits with code 2 only if the relaunch
 * itself fails. Does not return. */
void cdhost_bringup_timed_out(void);

/* While bring-up is in progress, the udid being bound (returns 1 and fills `out`); else 0. */
int cdhost_binding_udid(char *out, size_t cap);

/* api_start + mark the session ready + serve until the process is killed. Returns non-zero if the
 * listeners could not be created. */
int api_serve(api_session *session);

#endif /* API_SERVER_H */
