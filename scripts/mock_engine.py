#!/usr/bin/env python3
"""Replay a recorded .h265 (or .h264) as if it were the live engine. No device, no root.

This exists so the app can be developed and verified against real device frames without a phone
attached and without sudo. It speaks the same two sockets as `host/mirror.py` and `host-c/cdhost`:

    127.0.0.1:9877   Annex-B video, parameter sets first, paced to a frame rate, looping
    127.0.0.1:9876   the JSON-line control API — taps and swipes are logged, not injected

    python3 scripts/mock_engine.py [screen3.h265] [--fps 33] [--width 1170] [--height 2532]
                                   [--codec auto|hevc|h264] [--aud] [--advertise hevc|h264]

The codec is read from the file's own parameter sets unless --codec says otherwise; stream_info
reports it the way mirror.py does (json.dumps, so `"codec": "h264"` with a space). --aud ends
every access unit with an Access Unit Delimiter plus a trailing start code, the framing cdhost
uses so viewers can present a picture without waiting for the next one. list_devices answers in
cdhost's shape (devices[] with bound/screen_width/..., usbmuxd, engine_state) and keeps the
mirror.py fields (id, connected, transport, screen_size) the macOS app also reads.

The default width/height are an iPhone 13 Pro's real screen. They deliberately differ from the
coded video size (1184×2576) because the encoder pads to 16-pixel alignment — reproducing that
mismatch here is the point, since it is what the app has to crop and map clicks against.
"""
import argparse
import json
import os
import socket
import sys
import threading
import time

ANNEXB = b"\x00\x00\x00\x01"
LOGICAL_MAX = 16384

# Per-codec NAL grammar. HEVC: 2-byte header, type in bits 1-6. H.264: 1-byte header, bits 0-4.
CODECS = {
    "hevc": {"header": 2, "type": lambda b: (b >> 1) & 0x3F, "params": (32, 33, 34),
             "vcl": lambda t: t < 32, "key": lambda t: 16 <= t <= 23,
             "aud": b"\x46\x01\x50", "names": {32: "VPS", 33: "SPS", 34: "PPS"}},
    "h264": {"header": 1, "type": lambda b: b & 0x1F, "params": (7, 8),
             "vcl": lambda t: 1 <= t <= 5, "key": lambda t: t == 5,
             "aud": b"\x09\x10", "names": {7: "SPS", 8: "PPS"}},
}


def split_nals(data):
    """Annex-B -> NAL payloads, on 3-byte AND 4-byte start codes.

    Splitting on 00 00 00 01 alone silently dropped every NAL behind a 3-byte code — x265 writes
    one before each slice after the first in an access unit, so the IDR went missing. A 4-byte code
    is a 3-byte one with a zero in front, so splitting on 00 00 01 and trimming trailing zeros
    (that zero, plus any trailing_zero_8bits) handles both."""
    nals, i, starts = [], 0, []
    while True:
        j = data.find(b"\x00\x00\x01", i)
        if j < 0:
            break
        starts.append(j + 3)
        i = j + 3
    for k, s in enumerate(starts):
        e = starts[k + 1] - 3 if k + 1 < len(starts) else len(data)
        nal = data[s:e].rstrip(b"\x00")
        if nal:
            nals.append(nal)
    return nals


def sniff_codec(nals):
    """What the parameter sets say: HEVC VPS/SPS/PPS headers are 40 01 / 42 01 / 44 01; an H.264
    SPS is type 7 (with forbidden_zero_bit clear). None when the file shows neither."""
    for n in nals:
        if len(n) >= 2 and n[0] in (0x40, 0x42, 0x44) and n[1] == 0x01:
            return "hevc"
        if not (n[0] & 0x80) and (n[0] & 0x1F) == 7:
            return "h264"
    return None


def load_access_units(path, codec="auto"):
    """Split the file into parameter sets and access units, the way the app expects them."""
    nals = split_nals(open(path, "rb").read())
    if codec == "auto":
        codec = sniff_codec(nals) or ("h264" if path.lower().endswith((".h264", ".264", ".avc"))
                                      else "hevc")
    g = CODECS[codec]
    hl = g["header"]
    params, units, current = {}, [], []
    for nal in nals:
        if len(nal) <= hl:
            continue
        t = g["type"](nal[0])
        if t in g["params"]:
            params[t] = nal
            continue
        if g["vcl"](t):
            # First slice of a picture: HEVC first_slice_segment_in_pic_flag, H.264
            # first_mb_in_slice == 0 (ue(v) '1') — both the top bit after the NAL header.
            if (nal[hl] & 0x80) and current:
                units.append(current)
                current = []
            current.append(nal)
    if current:
        units.append(current)
    return codec, params, units


class MockEngine:
    def __init__(self, path, fps, width, height, start_at=0, burst=0, codec="auto", aud=False,
                 advertise=None):
        self.codec, self.params, self.units = load_access_units(path, codec)
        self.advertise = advertise or self.codec   # what stream_info claims
        self.grammar = CODECS[self.codec]
        self.aud = aud
        self.started = time.time()
        self.bytes_sent = 0
        self.fps = fps
        self.start_at = start_at
        self.burst = burst
        self.screen = (width, height)
        self.taps = 0
        self.swipes = 0
        self.loops = 0
        self.nals_sent = 0
        self._clients = []
        self._lock = threading.Lock()
        self.recorder = None        # mirrors the real engine's recording feature
        names = self.grammar["names"]
        missing = [names[n] for n in self.grammar["params"] if n not in self.params]
        if missing:
            print(f"warning: file has no {missing} parameter sets — a decoder cannot start")
        keys = sum(1 for u in self.units if any(self.grammar["key"](self._type(n)) for n in u))
        print(f"loaded {len(self.units)} {self.codec.upper()} access units ({keys} with a keyframe), "
              f"parameter sets {[names[t] for t in sorted(self.params)]} from {path}")
        if not keys:
            print("warning: no keyframe in the file — a viewer will never decode anything")

    def _type(self, nal):
        return self.grammar["type"](nal[0]) if nal else -1

    # ---------------- video ----------------
    def _preamble(self):
        return b"".join(ANNEXB + self.params[t] for t in self.grammar["params"] if t in self.params)

    def serve_video(self):
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", 9877))
        srv.listen(8)
        print("video   : 127.0.0.1:9877")
        while True:
            c, _ = srv.accept()
            with self._lock:
                self._clients.append(c)
            try:
                c.sendall(self._preamble())     # late joiners need these first
            except OSError:
                self._drop(c)
            print(f"  viewer connected ({len(self._clients)} total)")

    def _drop(self, c):
        with self._lock:
            if c in self._clients:
                self._clients.remove(c)
        try:
            c.close()
        except OSError:
            pass

    def pump_video(self):
        """One access unit per frame interval, looping forever."""
        interval = 1.0 / self.fps
        # start_at reproduces a late join: begin mid-GOP so the viewer gets no keyframe until the
        # loop wraps. That is the exact condition that made the first live run render black.
        i = min(self.start_at, len(self.units) - 1)
        if i:
            print(f"starting mid-GOP at access unit {i} — no keyframe until the loop wraps "
                  f"in ~{(len(self.units) - i) / self.fps:.0f}s")
        while True:
            if not self._clients:
                time.sleep(0.1)
                continue
            unit = self.units[i]
            payload = b"".join(ANNEXB + n for n in unit)
            # Re-send parameter sets each loop so a decoder can resynchronize.
            if i == 0:
                payload = self._preamble() + payload
            if self.aud:
                # cdhost's end-of-frame marker: AUD plus a trailing start code, which completes the
                # AUD at once so a viewer can submit the picture without waiting for the next one.
                payload += ANNEXB + self.grammar["aud"] + ANNEXB
            with self._lock:
                targets = list(self._clients)
            for c in targets:
                try:
                    c.sendall(payload)
                except OSError:
                    self._drop(c)
            self.nals_sent += len(unit)
            self.bytes_sent += len(payload)
            with self._lock:
                rec = self.recorder
            if rec:
                for n in unit:
                    rec["f"].write(ANNEXB + n)
                    rec["nals"] += 1
                    rec["bytes"] += len(n) + 4
                    if self.grammar["key"](self._type(n)):
                        rec["saw_keyframe"] = True
            i += 1
            if i >= len(self.units):
                i = 0
                self.loops += 1
            # Bursty delivery: send N units with no gap, then sleep for the time they represent.
            # The average rate is unchanged; the arrival pattern is not, and that is the point.
            if self.burst:
                if i % self.burst == 0:
                    time.sleep(interval * self.burst)
            else:
                time.sleep(interval)

    # ---------------- control ----------------
    def dispatch(self, method, params):
        if method == "ping":
            return {"pong": True}
        if method == "list_devices":
            # cdhost's shape (host-c/api_server.c method_list_devices): every row carries udid,
            # name, os_version AND product_version, product_type, flat screen_width/height (the
            # bound device only), connection, bound, binding; the envelope adds usbmuxd and
            # engine_state. The mirror.py spellings ride along -- id, connected, transport,
            # nested screen_size -- because the macOS app reads those as fallbacks.
            # Two rows so the sidebar is actually exercised: the replayed device, bound, plus a
            # second attached one that is not.
            w, h = self.screen
            return {"devices": [
                {"udid": "MOCK-REPLAY", "name": "iPhone13 (replay)",
                 "os_version": "27.0", "product_version": "27.0", "product_type": "iPhone14,2",
                 "screen_width": w, "screen_height": h, "connection": "usb",
                 "bound": True, "binding": False,
                 "id": "mock-0", "connected": True, "transport": "mock",
                 "screen_size": {"w": w, "h": h}},
                {"udid": "MOCK-OTHER", "name": "Christina's iPhone",
                 "os_version": "26.5.2", "product_version": "26.5.2", "product_type": "iPhone13,3",
                 "screen_width": 0, "screen_height": 0, "connection": "usb",
                 "bound": False, "binding": False,
                 "id": "mock-1", "connected": True, "transport": "usbmux"},
            ], "usbmuxd": True, "engine_state": "ready"}
        if method == "select_device":
            udid = params.get("udid")
            print(f"  SELECT {udid} (replay engine serves one device; ignored)")
            return {"udid": "MOCK-REPLAY", "rebinding": False}
        if method == "stream_info":
            up = max(time.time() - self.started, 1e-6)
            return {"port": 9877, "codec": self.advertise, "container": "annexb",
                    "viewers": len(self._clients), "nals": self.nals_sent,
                    "rtp_packets": 0, "loops": self.loops, "uptime_s": int(up),
                    "mbps": round(self.bytes_sent * 8 / up / 1e6, 2),
                    "engine": "mock", "streaming": True,
                    "recording": self._rec_stats()}
        if method == "start_recording":
            path = params.get("path") or "replay-recording.h265"
            with self._lock:
                if self.recorder:
                    raise ValueError(f"already recording to {self.recorder['path']}")
                d = os.path.dirname(os.path.abspath(path))
                if d:
                    os.makedirs(d, exist_ok=True)
                f = open(path, "wb")
                f.write(self._preamble())
                self.recorder = {"path": path, "f": f, "nals": 0, "bytes": 0,
                                 "started": time.time(), "saw_keyframe": False}
            print(f"  RECORD start -> {path}")
            return self._rec_stats()
        if method == "stop_recording":
            with self._lock:
                if not self.recorder:
                    raise ValueError("not recording")
                rec, self.recorder = self.recorder, None
            stats = self._stats_of(rec)
            rec["f"].close()
            print(f"  RECORD stop  {stats}")
            return stats
        if method == "tap":
            self.taps += 1
            fx, fy = self._fractions(params)
            print(f"  TAP   fx={fx:.4f} fy={fy:.4f}  -> device px "
                  f"({fx * self.screen[0]:.0f}, {fy * self.screen[1]:.0f})")
            return {"x": int(fx * LOGICAL_MAX), "y": int(fy * LOGICAL_MAX),
                    "logical_max": LOGICAL_MAX}
        if method == "swipe":
            self.swipes += 1
            fx0, fy0 = self._fractions(params, "x0", "y0")
            fx1, fy1 = self._fractions(params, "x1", "y1")
            print(f"  SWIPE ({fx0:.3f},{fy0:.3f}) -> ({fx1:.3f},{fy1:.3f})")
            return {"from": [int(fx0 * LOGICAL_MAX), int(fy0 * LOGICAL_MAX)],
                    "to": [int(fx1 * LOGICAL_MAX), int(fy1 * LOGICAL_MAX)],
                    "logical_max": LOGICAL_MAX}
        if method == "take_screenshot":
            raise ValueError("replay engine has no screenshot service")
        raise KeyError(method)

    def _stats_of(self, rec):
        return {"path": rec["path"], "nals": rec["nals"], "bytes": rec["bytes"],
                "duration_s": round(time.time() - rec["started"], 2),
                "saw_keyframe": rec["saw_keyframe"]}

    def _rec_stats(self):
        with self._lock:
            return self._stats_of(self.recorder) if self.recorder else None

    def _fractions(self, params, xk="x", yk="y"):
        fx, fy = params.get("f" + xk), params.get("f" + yk)
        if fx is not None and fy is not None:
            return float(fx), float(fy)
        x, y = params.get(xk), params.get(yk)
        if x is None or y is None:
            raise ValueError(f"need {xk}/{yk} or f{xk}/f{yk}")
        return float(x) / self.screen[0], float(y) / self.screen[1]

    def serve_control(self):
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", 9876))
        srv.listen(16)
        print("control : 127.0.0.1:9876")
        while True:
            c, _ = srv.accept()
            threading.Thread(target=self._control_conn, args=(c,), daemon=True).start()

    def _control_conn(self, sock):
        buf = b""
        try:
            while True:
                chunk = sock.recv(4096)
                if not chunk:
                    return
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    if not line.strip():
                        continue
                    sock.sendall(json.dumps(self._handle(line)).encode() + b"\n")
        except OSError:
            pass
        finally:
            try:
                sock.close()
            except OSError:
                pass

    def _handle(self, line):
        try:
            req = json.loads(line)
        except ValueError as e:
            return {"ok": False, "error": {"code": "bad_json", "message": str(e)}}
        rid, method = req.get("id"), req.get("method")
        try:
            resp = {"ok": True, "result": self.dispatch(method, req.get("params") or {})}
        except KeyError:
            resp = {"ok": False, "error": {"code": "unsupported_method", "message": str(method)}}
        except ValueError as e:
            resp = {"ok": False, "error": {"code": "bad_request", "message": str(e)}}
        if rid is not None:
            resp["id"] = rid
        return resp


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file", nargs="?", default="screen3.h265")
    ap.add_argument("--codec", choices=["auto", "hevc", "h264"], default="auto",
                    help="the file's codec; auto reads its parameter sets (default)")
    ap.add_argument("--aud", action="store_true",
                    help="end each access unit with AUD + start code, as cdhost does")
    ap.add_argument("--advertise", choices=["hevc", "h264"],
                    help="the codec stream_info CLAIMS (default: the real one). cdhost says hevc "
                         "even on an H.264 stream; this reproduces a client trusting that")
    ap.add_argument("--fps", type=float, default=33.0)
    ap.add_argument("--width", type=int, default=1170)
    ap.add_argument("--height", type=int, default=2532)
    ap.add_argument("--start-at", type=int, default=0,
                    help="begin at this access unit — use it to reproduce a late, keyframe-less join")
    ap.add_argument("--burst", type=int, default=0, metavar="N",
                    help="send N access units back-to-back then sleep, instead of pacing evenly. "
                         "Live RTP arrives in bursts (400+ packets/s), and even pacing hides bugs "
                         "that only appear when several frames are assembled in the same instant.")
    args = ap.parse_args()

    engine = MockEngine(args.file, args.fps, args.width, args.height, args.start_at, args.burst,
                        args.codec, args.aud, args.advertise)
    threading.Thread(target=engine.serve_video, daemon=True).start()
    threading.Thread(target=engine.serve_control, daemon=True).start()
    threading.Thread(target=engine.pump_video, daemon=True).start()
    print(f"replaying {engine.codec.upper()} at {args.fps} fps, screen {args.width}x{args.height}"
          f"{', AUD-framed' if args.aud else ''}. Ctrl-C to stop.\n", flush=True)
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print(f"\nstopped. {engine.taps} taps, {engine.swipes} swipes, "
              f"{engine.nals_sent} NALs sent, {engine.loops} loops")
    return 0


if __name__ == "__main__":
    sys.exit(main())
