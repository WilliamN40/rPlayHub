# core/ — the portable video path. No platform dependencies, so this builds anywhere.
#
#   make test     build and run the unit tests, including against real recordings if present
#
# Windows (MSYS2 UCRT64/MinGW): binaries get a .exe suffix so make sees the files the compiler
# actually writes and does not relink every time. The byte-compare checks (scripts/check-*.sh)
# need a Python 3 with the host/ modules importable; PYTHON is exported to them for scripts that
# honour it (MSYS2's is `python3` from the mingw-w64-ucrt-x86_64-python package).

CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -O2 -g

PYTHON ?= python3
export PYTHON

UNAME := $(shell uname 2>/dev/null)
ifeq ($(OS),Windows_NT)
  EXE := .exe
else ifneq (,$(findstring NT,$(UNAME)))
  EXE := .exe
endif

HEVC_SAMPLE ?= ../screen3.h265
H264_SAMPLE ?= ../build/screen3.h264

TESTS := test_rp_video_stream test_rp_xpc test_rp_remotexpc test_rp_media_offer test_rp_rtp test_rp_bigreply
BINS  := $(addsuffix $(EXE),$(TESTS))

all: $(BINS)

test_rp_video_stream$(EXE): test_rp_video_stream.c rp_video_stream.c rp_video_stream.h rp_geometry.c rp_geometry.h
	$(CC) $(CFLAGS) -o $@ test_rp_video_stream.c rp_video_stream.c rp_geometry.c

test_rp_remotexpc$(EXE): test_rp_remotexpc.c rp_remotexpc.c rp_http2.c rp_xpc.c
	$(CC) $(CFLAGS) -o $@ $^

test_rp_xpc$(EXE): test_rp_xpc.c rp_xpc.c rp_xpc.h
	$(CC) $(CFLAGS) -o $@ test_rp_xpc.c rp_xpc.c

test_rp_media_offer$(EXE): test_rp_media_offer.c rp_media_offer.c
	$(CC) $(CFLAGS) -o $@ $^ -lz

test_rp_rtp$(EXE): test_rp_rtp.c rp_rtp.c
	$(CC) $(CFLAGS) -o $@ $^

test_rp_bigreply$(EXE): test_rp_bigreply.c rp_remotexpc.c rp_xpc.c rp_http2.c
	$(CC) $(CFLAGS) -o $@ $^

# `make test_rp_xpc` etc. keep working on Windows as aliases for the .exe targets.
ifneq ($(EXE),)
$(TESTS): %: %$(EXE)
.PHONY: $(TESTS)
endif

test: $(BINS)
	./test_rp_video_stream$(EXE) $(HEVC_SAMPLE) $(H264_SAMPLE)
	@echo "core/rp_xpc tests"
	@./test_rp_xpc$(EXE) >/dev/null && echo "  encoder + reader OK (byte-compare against Python: scripts/check-xpc.sh)"
	@./test_rp_remotexpc$(EXE) | tail -6
	@./test_rp_bigreply$(EXE)
	@../scripts/check-handshake.sh || true
	@echo "core/rp_media_offer + rp_rtp (byte-compared against the Python and a real capture)"
	@../scripts/check-offer.sh || true
	@../scripts/check-rtp.sh || true

clean:
	rm -f $(TESTS) $(addsuffix .exe,$(TESTS))

.PHONY: all test clean
