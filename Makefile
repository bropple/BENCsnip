# BENCsnip
#
#   make            the editor
#   make test       the core tests, which need no raylib and no window
#   make probe      a command-line media probe, for checking the media layer
#   make info       what the build decided about raylib and ffmpeg
#   make clean      remove everything the build made
#
# The core is built without raylib on purpose. Everything that knows what a
# clip is, how to decode one and how to write one out lives there, and a
# dependency on a windowing library in that half is exactly the thing that
# makes a headless render or a test impossible later.

CXX      ?= c++
CC       ?= cc
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wno-unused-parameter -MMD -MP
CFLAGS   ?= -O2 -g
CFLAGS   += -std=c99 -Wall -Wextra -MMD -MP
CPPFLAGS += -Isrc/core

# `all` is defined a long way down, and the first target in a makefile is what
# a bare `make` builds. Saying so here means a rule can be added anywhere above
# it without quietly becoming the default.
.DEFAULT_GOAL := all

UNAME_S := $(shell uname -s)

# gcc appends .exe on Windows no matter what -o says. A target named without
# it is a target make never finds, so every invocation relinks.
ifeq ($(OS),Windows_NT)
  EXE := .exe
else
  EXE :=
endif

CORE_SRC := src/core/sn_media.cpp \
            src/core/sn_render.cpp \
            src/core/sn_timeline.cpp \
            src/core/sn_project.cpp \
            src/core/sn_gif.cpp \
            src/core/sn_export.cpp
CORE_OBJ := $(CORE_SRC:.cpp=.o)
CORE_LIB := libbencsnip.a

# The GL probe is compiled into the executable as well as shipped beside it -
# see the note at the top of tools/glprobe.c. Behind --glprobe, so it costs a
# few kilobytes and nothing else.
PROBE_IN_GUI := tools/glprobe.c
PROBE_IN_GUI_OBJ := tools/glprobe.embedded.o

GUI_SRC  := src/gui/main.cpp \
            src/gui/sn_gui.cpp \
            src/gui/sn_bin.cpp \
            src/gui/sn_track.cpp \
            src/gui/sn_player.cpp \
            src/gui/sn_tarr.cpp \
            src/gui/sn_filedlg.cpp \
            src/gui/sn_appmenu.cpp \
            src/gui/sn_dialog.cpp \
            src/gui/sn_peaks.cpp
GUI_OBJ  := $(GUI_SRC:.cpp=.o)
GUI      := bencsnip$(EXE)

# The macOS file panels and menu bar are Objective-C++, because they are AppKit
# calls rather than a program to run - see the top of sn_filedlg_mac.mm. Only
# these are, and only on Darwin; the .cpp beside each holds the other two
# platforms and compiles to nothing on this one.
ifeq ($(UNAME_S),Darwin)
  GUI_MM  := src/gui/sn_filedlg_mac.mm \
             src/gui/sn_appmenu_mac.mm
  GUI_OBJ += $(GUI_MM:.mm=.o)
  # NSOpenPanel and NSSavePanel are AppKit. raylib asks for the Cocoa umbrella,
  # which covers it, but only when raylib was found somewhere that sets RL_SYS
  # - a pkg-config raylib leaves that empty and the link then fails on the
  # panels with nothing saying which framework they came from. Named here so it
  # holds however raylib was found, the way -lcomdlg32 does on Windows.
  MAC_LIBS := -framework AppKit
endif

PROBE_SRC := tools/probe.cpp
PROBE_OBJ := $(PROBE_SRC:.cpp=.o)
PROBE     := bencsnip-probe$(EXE)

TEST_SRC := tests/core_test.cpp
TEST_OBJ := $(TEST_SRC:.cpp=.o)
TEST     := bencsnip-test$(EXE)

# ------------------------------------------------------------------
# Assets live in the binary, not beside it - see src/gui/sn_embed.h.
# ------------------------------------------------------------------
EMBED    := src/gui/sn_embed.c
EMBED_OBJ:= src/gui/sn_embed.o
EMBED_IN := assets/fonts/TerminusTTF.ttf \
            assets/brand/BENCO_Logo_Terminal.png \
            assets/icon/icon-16.png \
            assets/icon/icon-32.png \
            assets/icon/icon-48.png \
            assets/icon/icon-64.png \
            assets/fonts/OFL.txt \
            LICENSE \
            NOTICE
GUI_OBJ  += $(EMBED_OBJ) $(PROBE_IN_GUI_OBJ)

$(PROBE_IN_GUI_OBJ): $(PROBE_IN_GUI)
	$(CC) $(CFLAGS) -DSN_GLPROBE_EMBEDDED -c $< -o $@

# ------------------------------------------------------------------
# raylib
#
# Three places, most specific first:
#
#   vendor/raylib     a prefix dropped into the tree, which is what the
#                     .gitignore expects and what a fresh clone should set up
#   RAYLIB=/prefix    on the command line, for a copy living elsewhere
#   pkg-config        a system package
#
# A static libraylib.a is preferred where one exists, so the binary runs on a
# machine that has never heard of raylib.
#
# One thing a raylib built elsewhere will not have: tools/no-gamepads.sh. Every
# workflow here runs it against the source before building, because on Windows
# GLFW's joystick support costs ten seconds of startup on a machine with a lot
# of peripherals and this program does not read joysticks. A raylib built
# without it works perfectly well and opens its window slowly on exactly one
# kind of machine. The script says the rest.
# ------------------------------------------------------------------
VENDOR_RL := $(wildcard vendor/raylib/lib/libraylib.a)

ifneq ($(VENDOR_RL),)
  RL_CFLAGS := -Ivendor/raylib/include
  RL_LIBS   := $(VENDOR_RL)
  RL_FROM   := vendor/raylib
else ifdef RAYLIB
  RL_CFLAGS := -I$(RAYLIB)/include
  RL_STATIC := $(wildcard $(RAYLIB)/lib/libraylib.a)
  RL_LIBS   := $(if $(RL_STATIC),$(RL_STATIC),-L$(RAYLIB)/lib -lraylib)
  RL_FROM   := RAYLIB=$(RAYLIB)
else
  RL_CFLAGS := $(shell pkg-config --cflags raylib 2>/dev/null)
  RL_LIBS   := $(shell pkg-config --libs --static raylib 2>/dev/null)
  RL_FROM   := pkg-config
endif

ifeq ($(RL_FROM),pkg-config)
  RL_SYS :=
else ifeq ($(UNAME_S),Darwin)
  RL_SYS := -framework CoreVideo -framework IOKit -framework Cocoa \
            -framework GLUT -framework OpenGL
else ifeq ($(OS),Windows_NT)
  RL_SYS := -lopengl32 -lgdi32 -lwinmm
else
  RL_SYS := -lGL -lm -lpthread -ldl -lrt -lX11
endif

# ------------------------------------------------------------------
# ffmpeg
#
# The whole point of this program is that it opens whatever you drag into it,
# and that is libav's job, not something to reimplement. Same arrangement as
# raylib: a static prefix in vendor/ffmpeg is preferred, because a release
# binary that needs six shared libraries of a particular soname is a binary
# that runs on the machine that built it.
#
# Build one with tools/build-ffmpeg.sh, or point FFMPEG= at a prefix.
# ------------------------------------------------------------------
FF_MODS := libavformat libavcodec libswscale libswresample libavutil

VENDOR_FF := $(wildcard vendor/ffmpeg/lib/libavformat.a)

ifneq ($(VENDOR_FF),)
  FF_PC   := PKG_CONFIG_PATH="$(CURDIR)/vendor/ffmpeg/lib/pkgconfig" pkg-config --static
  FF_FROM := vendor/ffmpeg
else ifdef FFMPEG
  FF_PC   := PKG_CONFIG_PATH="$(FFMPEG)/lib/pkgconfig" pkg-config --static
  FF_FROM := FFMPEG=$(FFMPEG)
else
  FF_PC   := pkg-config
  FF_FROM := pkg-config
endif

FF_CFLAGS := $(shell $(FF_PC) --cflags $(FF_MODS) 2>/dev/null)
FF_LIBS   := $(shell $(FF_PC) --libs $(FF_MODS) 2>/dev/null)

CPPFLAGS += $(FF_CFLAGS)

# Which ffmpeg the objects were compiled against, remembered between runs.
#
# Switching prefixes - a system ffmpeg one day, vendor/ffmpeg or FFMPEG= the
# next - changes the headers without changing any source file, so make sees
# nothing to do and relinks objects built against one libav against the shared
# libraries of another. AVFrame is a different size between major versions, so
# the result builds, runs, and crashes somewhere unrelated with a stack that
# points at libavutil. This makes the objects depend on the answer.
FF_STAMP := .ffmpeg-prefix

.PHONY: FORCE
FORCE:

$(FF_STAMP): FORCE
	@printf '%s\n' '$(FF_FROM) | $(FF_CFLAGS)' | cmp -s - $@ || \
	  printf '%s\n' '$(FF_FROM) | $(FF_CFLAGS)' > $@

# Windows: compile the icon in, link as a GUI subsystem binary so a
# double-click does not open a console behind the window, and link the
# toolchain's own runtime statically - without that the binary imports
# libstdc++-6.dll, libgcc_s_seh-1.dll and libwinpthread-1.dll, which live
# inside an MSYS2 installation and nowhere else. It runs on the machine that
# built it and fails to start on every machine that downloads it.
#
# How much else is linked statically depends on which ffmpeg was found, and
# the two cases must not be confused:
#
#   vendor/ffmpeg or FFMPEG=   a static prefix, so -static: everything goes in
#                              the executable and nothing has to be shipped
#                              beside it. This is what release.yml builds.
#
#   pkg-config                 MSYS2's ffmpeg, which is DLLs. A bare -static
#                              here makes the linker prefer MSYS2's static
#                              libavutil.a instead - and then fail on the
#                              hundred symbols its private dependencies would
#                              have provided, from libva to BCryptGenRandom.
#                              So the runtime is pinned piece by piece and
#                              ffmpeg stays a DLL.
ifeq ($(OS),Windows_NT)
  GUI_RES  := src/gui/bencsnip.res.o
  # GetOpenFileName and GetSaveFileName live in comdlg32. raylib brings in
  # gdi32, winmm and opengl32 and nothing else, so without this the link fails
  # on two symbols and nowhere says which library they belong to.
  WIN_LIBS := -lcomdlg32
  GUI_LINK := -mwindows -static -static-libgcc -static-libstdc++
  # -static is what pins the runtime, and it has to stay in force to the end
  # of the link line. An explicit -lwinpthread earlier does not do the job:
  # gcc appends libstdc++.a *after* everything given here, so its pthread
  # references come up once the static archive has already been passed and
  # are answered by the driver's own -lwinpthread - which is the import
  # library, and libwinpthread-1.dll ends up in the import table anyway.
  #
  # Against MSYS2's packaged ffmpeg, then, only ffmpeg is dynamic, and it is
  # marked so where it appears rather than by relaxing the whole link.
  ifeq ($(FF_FROM),pkg-config)
    FF_LINK := -Wl,-Bdynamic $(FF_LIBS) -Wl,-Bstatic
  endif
else
  GUI_RES  :=
  GUI_LINK :=
endif

# Everywhere but the Windows-with-packaged-ffmpeg case, this is just FF_LIBS.
FF_LINK ?= $(FF_LIBS)

LDLIBS_GUI := $(RL_LIBS) $(RL_SYS) $(FF_LINK) $(WIN_LIBS) $(MAC_LIBS)

.PHONY: all gui core test probe clean info check-deps

all: gui

gui: check-deps $(GUI)

core: $(CORE_LIB)

# ------------------------------------------------------------------
# Rules
# ------------------------------------------------------------------
%.o: %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(RL_CFLAGS) -c $< -o $@

# -fno-objc-arc is the default and is said out loud because the file is written
# for it: the panels are autoreleased class methods held only for the length of
# one @autoreleasepool, and nothing in there is retained or released by hand.
%.o: %.mm
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(RL_CFLAGS) -fno-objc-arc -c $< -o $@

$(CORE_OBJ) $(GUI_OBJ) $(PROBE_OBJ) $(TEST_OBJ): $(FF_STAMP)

$(CORE_OBJ): %.o: %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(CORE_LIB): $(CORE_OBJ)
	$(AR) rcs $@ $^

mkembed$(EXE): tools/mkembed.c
	$(CC) $(CFLAGS) $< -o $@

$(EMBED): mkembed$(EXE) $(EMBED_IN) Makefile
	./mkembed$(EXE) $@ \
	  SN_FONT_TTF     assets/fonts/TerminusTTF.ttf \
	  SN_LOGO_PNG     assets/brand/BENCO_Logo_Terminal.png \
	  SN_ICON_16      assets/icon/icon-16.png \
	  SN_ICON_32      assets/icon/icon-32.png \
	  SN_ICON_48      assets/icon/icon-48.png \
	  SN_ICON_64      assets/icon/icon-64.png \
	  SN_LICENSE_OFL  assets/fonts/OFL.txt \
	  SN_LICENSE_MIT  LICENSE \
	  SN_NOTICE       NOTICE

$(EMBED_OBJ): $(EMBED)
	$(CC) $(CFLAGS) -c $< -o $@

src/gui/bencsnip.res.o: src/gui/bencsnip.rc assets/icon/bencsnip.ico
	windres $< -o $@

$(GUI): $(GUI_OBJ) $(GUI_RES) $(CORE_LIB)
	$(CXX) $(CXXFLAGS) $(GUI_LINK) $(GUI_OBJ) $(GUI_RES) $(CORE_LIB) \
	  $(LDLIBS_GUI) -o $@

probe: $(PROBE)

$(PROBE): $(PROBE_OBJ) $(CORE_LIB)
	$(CXX) $(CXXFLAGS) $^ $(FF_LIBS) -o $@

test: $(TEST)
	./$(TEST)

$(TEST): $(TEST_OBJ) $(CORE_LIB)
	$(CXX) $(CXXFLAGS) $^ $(FF_LIBS) -o $@

# ------------------------------------------------------------------
# Diagnosis, for when the build decided wrong
# ------------------------------------------------------------------
check-deps:
	@if [ -z "$(RL_LIBS)" ]; then \
	  echo "  No raylib found. Put one in vendor/raylib (include/ and lib/),"; \
	  echo "  pass RAYLIB=/prefix, or install the system package."; \
	  exit 1; \
	fi
	@if [ -z "$(FF_LIBS)" ]; then \
	  echo "  No ffmpeg development libraries found. Install them:"; \
	  echo "    pacman -S ffmpeg     apt install libavformat-dev libavcodec-dev \\"; \
	  echo "                              libswscale-dev libswresample-dev"; \
	  echo "  or build a static prefix into vendor/ffmpeg."; \
	  exit 1; \
	fi

info:
	@echo "  platform     = $(UNAME_S)"
	@echo "  raylib from  = $(RL_FROM)"
	@echo "  raylib libs  = $(RL_LIBS) $(RL_SYS)"
	@echo "  ffmpeg from  = $(FF_FROM)"
	@echo "  ffmpeg cflags= $(FF_CFLAGS)"
	@echo "  ffmpeg libs  = $(FF_LIBS)"

clean:
	rm -f $(CORE_OBJ) $(GUI_OBJ) $(PROBE_OBJ) $(TEST_OBJ) $(GUI_RES) \
	      $(CORE_LIB) $(GUI) $(PROBE) $(TEST) mkembed$(EXE) $(EMBED) \
	      src/core/*.d src/gui/*.d tools/*.d tests/*.d $(FF_STAMP)

-include $(CORE_SRC:.cpp=.d) $(GUI_SRC:.cpp=.d) $(PROBE_SRC:.cpp=.d) \
         $(TEST_SRC:.cpp=.d) $(GUI_MM:.mm=.d) src/gui/sn_embed.d

# ------------------------------------------------------------------
# Test media. Three small files covering the cases that matter: an mp4 with
# both streams, a webm with different codecs and a different aspect, and an
# audio-only file. Made with the ffmpeg command rather than committed, because
# three megabytes of colour bars is not source.
# ------------------------------------------------------------------
.PHONY: testmedia
testmedia:
	@mkdir -p media
	@command -v ffmpeg >/dev/null || { echo "  no ffmpeg command on PATH"; exit 1; }
	ffmpeg -y -v error -f lavfi -i testsrc2=size=1280x720:rate=30:duration=8 \
	  -f lavfi -i sine=frequency=440:duration=8 \
	  -c:v libx264 -pix_fmt yuv420p -c:a aac media/test1.mp4
	ffmpeg -y -v error -f lavfi -i smptebars=size=640x480:rate=25:duration=5 \
	  -f lavfi -i sine=frequency=220:duration=5 \
	  -c:v libvpx-vp9 -b:v 500k -c:a libopus media/test2.webm
	ffmpeg -y -v error -f lavfi -i sine=frequency=330:duration=6 \
	  -c:a libmp3lame media/test3.mp3
	@# A transparent animation, for the compositing tests: a solid gold square
	@# in the middle of a 240x240 frame with nothing around it. Deliberately a
	@# square rather than a star or a glyph - the test samples one point that
	@# must be opaque and one that must be transparent, and both have to be
	@# somewhere a person reading the test can work out on paper.
	ffmpeg -y -v error -f lavfi -i "color=c=black:s=240x240:r=10:d=0.8" \
	  -vf "drawbox=x=60:y=60:w=120:h=120:color=0xeecb2e:t=fill,\
colorkey=black:0.01:0.0,format=rgba,split[a][b];\
[a]palettegen=reserve_transparent=1:max_colors=32[p];\
[b][p]paletteuse=alpha_threshold=128" media/overlay.gif
	@# An animated GIF where every frame differs from the last, for the loop
	@# tests. overlay.gif cannot do that job: it is one still square, so a
	@# decoder that handed back the same picture forever would pass.
	ffmpeg -y -v error -f lavfi -i "testsrc2=size=160x120:rate=10:duration=0.8" \
	  -vf "format=rgb24,split[a][b];[a]palettegen=max_colors=64[p];[b][p]paletteuse" \
	  media/test7.gif
	@# A smooth gradient, which is the thing a GIF palette is judged on: bars
	@# and flat colour survive any palette at all, and a sky does not.
	ffmpeg -y -v error -f lavfi -i \
	  "gradients=s=320x240:c0=0x102040:c1=0xe0a070:x0=0:y0=0:x1=320:y1=240:d=2:r=10" \
	  -frames:v 20 -c:v libx264 -pix_fmt yuv420p -crf 12 media/test5.mp4
	@# Video with an audio track that has nothing in it, which is what a phone
	@# with the microphone off writes - and what used to arrive with a clip on
	@# the timeline that does nothing.
	ffmpeg -y -v error -f lavfi -i "testsrc2=size=320x240:rate=25:duration=2" \
	  -f lavfi -i "anullsrc=r=48000:cl=stereo" -t 2 -shortest \
	  -c:v libx264 -pix_fmt yuv420p -c:a aac media/test6.mp4

# The icon set, from assets/icon/film_camera_star.svg. Needs librsvg and
# ImageMagick, and nothing else does - which is why the output is committed.
.PHONY: icons
icons:
	tools/make-icons.sh
