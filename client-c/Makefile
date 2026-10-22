# rplay-view — the portable thin client (ffmpeg software decode + SDL2).
#
# Decode links the RVRA-PATCHED ffmpeg in ../deps/ffmpeg when it has been built
# (scripts/build-ffmpeg-rvra.sh) — required for a correct picture: this stream needs RVRA
# reference resampling, which stock ffmpeg lacks (doc/RVRA-AND-PORTABILITY.md). Without it the
# build falls back to the system ffmpeg via pkg-config and prints a warning: everything works,
# but pictures garble from the first resolution downshift under motion.
#
# Needs: pkg-config, SDL2 (and, for the fallback, system libavcodec/libavutil).
#   macOS:  brew install ffmpeg sdl2
#   Debian: apt install libavcodec-dev libavutil-dev libsdl2-dev
#
# Windows: MSYS2 UCRT64 (gcc/g++, make, pkgconf, SDL2, zlib). Build from a real MSYS2 shell, not
# Git Bash, which mangles /ucrt64 paths:
#   MSYSTEM=UCRT64 CHERE_INVOKING=1 /c/msys64/usr/bin/bash -lc 'make -C client-c rplay-gui'
# produces client-c/rplay-gui.exe: one fully static, GUI-subsystem executable (SDL2, FFmpeg,
# libstdc++, winpthread all linked in), with the icon, version info and application manifest from
# rplay-gui.rc. rplay-view is not ported to Windows.

FFMPEG_DIR ?= ../deps/ffmpeg

# The Linux client's version: what scripts/package-deb.sh stamps on the .deb and the release tag
# (linux-v<version>) carries. Independent of the macOS app's MARKETING_VERSION. The Windows build
# stamps it into rplay-gui.exe's version resource too.
RPLAYHUB_VERSION := 0.1.3

ifeq ($(OS),Windows_NT)
  WINDOWS := 1
else ifneq ($(filter MINGW% MSYS%,$(shell uname -s 2>/dev/null)),)
  WINDOWS := 1
endif

CFLAGS += -Wall -Wextra -O2
ifdef WINDOWS
  EXE     := .exe
  WINDRES ?= windres
  # Lean windows.h everywhere, so <SDL_syswm.h> (which includes it) does not drag in winsock.h
  # ahead of the <winsock2.h> the sources use.
  CFLAGS  += -DWIN32_LEAN_AND_MEAN -DNOMINMAX
  # --static: SDL2's own system dependencies, for linking libSDL2.a rather than SDL2.dll.
  LIBS    := $(shell pkg-config --static --libs sdl2)
else
  LIBS    := $(shell pkg-config --libs sdl2)
endif

ifneq ($(wildcard $(FFMPEG_DIR)/libavcodec/libavcodec.a),)
  # The patched tree FIRST, before SDL's pkg-config flags drag in /opt/homebrew/include --
  # the system ffmpeg headers live there too, and silently shadowing the patched ones with
  # them only works while the two versions happen to match.
  ifdef WINDOWS
    # Not -I on the tree itself: its top level holds files named VERSION, RELEASE, ... and on a
    # case-insensitive filesystem C++'s <version> resolves to ffmpeg's VERSION. Stage just the
    # public headers (rule below) and point -I at those.
    FFINC  := .build/ffmpeg-include
    FF_HDR_STAMP := $(FFINC)/.stamp
    CFLAGS += -I$(FFINC)
  else
    CFLAGS += -I$(FFMPEG_DIR)
  endif
  # -lz: the patched tree is configured --enable-zlib so libavcodec has the PNG decoder the GUI
  # uses for app icons.
  LIBS   += $(FFMPEG_DIR)/libavcodec/libavcodec.a $(FFMPEG_DIR)/libswresample/libswresample.a \
            $(FFMPEG_DIR)/libavutil/libavutil.a -lm -lpthread -lz
else
  $(warning $(FFMPEG_DIR) not built -- linking SYSTEM ffmpeg; RVRA streams will garble under motion. Run scripts/build-ffmpeg-rvra.sh.)
  CFLAGS += $(shell pkg-config --cflags libavcodec libavutil)
  LIBS   += $(shell pkg-config --libs libavcodec libavutil)
endif
CFLAGS += $(shell pkg-config --cflags sdl2)

ifdef WINDOWS
# Not ported (POSIX sockets throughout); the GUI is the Windows client.
.DEFAULT_GOAL := rplay-gui
rplay-view:
	@echo "rplay-view is not ported to Windows -- build rplay-gui instead." >&2; exit 1
else
rplay-view: rplay-view.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)
endif

# The full GUI (mirror pane + inspector tabs) — a SEPARATE app: Dear ImGui over the same SDL2
# renderer. rplay-view stays the untouched minimal reference viewer; stream-core.c is the GUI's
# verbatim copy of its stream engine (kept in sync by hand — the reference captures verify both).
# Needs deps/imgui + deps/json (scripts/fetch-gui-deps.sh) and a C++ compiler; `make` alone still
# builds only the minimal viewer.
IMGUI_DIR ?= ../deps/imgui
IMGUI_SRC := $(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_draw.cpp $(IMGUI_DIR)/imgui_tables.cpp \
             $(IMGUI_DIR)/imgui_widgets.cpp $(IMGUI_DIR)/backends/imgui_impl_sdl2.cpp \
             $(IMGUI_DIR)/backends/imgui_impl_sdlrenderer2.cpp
UI_SRC    := ui/window_effects.cc ui/display_window.cc ui/twin_view.cc ui/fold_view.cc ui/png_decode.cc
UI_HDR    := ui/theme.h ui/icons.h ui/window_effects.h ui/display_window.h ui/twin_view.h ui/fold_view.h ui/png_decode.h

GUI_LIBS  := $(LIBS)
ifneq ($(wildcard $(FFMPEG_DIR)/libswscale/libswscale.a),)
  GUI_LIBS := $(FFMPEG_DIR)/libswscale/libswscale.a $(GUI_LIBS)
else
  CFLAGS   += $(shell pkg-config --cflags libswscale 2>/dev/null)
  GUI_LIBS += $(shell pkg-config --libs libswscale 2>/dev/null)
endif

CXXFLAGS += $(CFLAGS) -std=c++17 -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends -I../deps/json -Iui \
            -DRPLAYHUB_VERSION=\"$(RPLAYHUB_VERSION)\"

ifeq ($(shell pkg-config --exists x11 xext gl 2>/dev/null && echo 1),1)
  CXXFLAGS += -DRPLAYHUB_HAVE_X11 $(shell pkg-config --cflags x11 xext gl)
  GUI_LIBS += $(shell pkg-config --libs x11 xext gl)
endif

GUI_BIN  := rplay-gui$(EXE)
GUI_DEPS := rplay-gui.cpp $(UI_SRC) $(UI_HDR) stream-core.c stream-core.h audio.c audio.h
GUI_RES  :=

ifdef WINDOWS
  # Everything static (-static also picks libSDL2.a over SDL2.dll), so the exe runs anywhere with
  # no DLLs beside it. -mwindows (from sdl2's pkg-config) makes it a GUI-subsystem app with no
  # console; rplay-gui attaches to the parent console or logs to %LOCALAPPDATA%\rplayhub instead.
  GUI_LDFLAGS := -static -static-libgcc -static-libstdc++ -s
  GUI_LIBS    += -ldwmapi -lws2_32 -lcomdlg32 -lshell32 -lole32 -loleaut32 -luser32 -lgdi32 \
                 -lwinmm -limm32 -lversion -lsetupapi -luuid -lbcrypt -lsecur32 -ladvapi32
  GUI_RES     := rplay-gui.res.o
  VER_WORDS   := $(subst ., ,$(RPLAYHUB_VERSION))

$(GUI_RES): rplay-gui.rc rplay-gui.manifest rplay-gui.ico
	$(WINDRES) -DRPLAYHUB_VER_MAJOR=$(word 1,$(VER_WORDS)) -DRPLAYHUB_VER_MINOR=$(word 2,$(VER_WORDS)) \
	    -DRPLAYHUB_VER_PATCH=$(word 3,$(VER_WORDS)) -O coff -i rplay-gui.rc -o $@

# `make rplay-gui` keeps working on Windows, where the file is rplay-gui.exe.
rplay-gui: $(GUI_BIN)
.PHONY: rplay-gui
endif

ifdef FF_HDR_STAMP
$(FF_HDR_STAMP): $(FFMPEG_DIR)/libavcodec/libavcodec.a
	rm -rf $(FFINC) && mkdir -p $(FFINC)
	cd $(FFMPEG_DIR) && find libavcodec libavutil libswscale libswresample -name '*.h' \
	    -exec cp --parents {} $(abspath $(FFINC)) \;
	touch $@
endif

$(GUI_BIN): $(GUI_DEPS) $(GUI_RES) $(FF_HDR_STAMP)
	$(CC) $(CFLAGS) -c stream-core.c -o stream-core.o
	$(CC) $(CFLAGS) -c audio.c -o audio.o
	$(CXX) $(CXXFLAGS) -o $@ rplay-gui.cpp $(UI_SRC) stream-core.o audio.o $(IMGUI_SRC) $(GUI_RES) \
	    $(GUI_LDFLAGS) $(GUI_LIBS)

clean:
	rm -f rplay-view rplay-gui rplay-gui.exe rplay-gui.res.o stream-core.o audio.o
	rm -rf .build

.PHONY: clean
