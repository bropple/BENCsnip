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
            src/core/sn_export.cpp
CORE_OBJ := $(CORE_SRC:.cpp=.o)
CORE_LIB := libbencsnip.a

GUI_SRC  := src/gui/main.cpp \
            src/gui/sn_gui.cpp \
            src/gui/sn_bin.cpp \
            src/gui/sn_track.cpp \
            src/gui/sn_player.cpp \
            src/gui/sn_tarr.cpp \
            src/gui/sn_filedlg.cpp \
            src/gui/sn_dialog.cpp
GUI_OBJ  := $(GUI_SRC:.cpp=.o)
GUI      := bencsnip$(EXE)

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
            assets/icon/star-256.png \
            assets/fonts/OFL.txt \
            LICENSE \
            NOTICE
GUI_OBJ  += $(EMBED_OBJ)

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
  FF_PC   := PKG_CONFIG_PATH=$(CURDIR)/vendor/ffmpeg/lib/pkgconfig pkg-config --static
  FF_FROM := vendor/ffmpeg
else ifdef FFMPEG
  FF_PC   := PKG_CONFIG_PATH=$(FFMPEG)/lib/pkgconfig pkg-config --static
  FF_FROM := FFMPEG=$(FFMPEG)
else
  FF_PC   := pkg-config
  FF_FROM := pkg-config
endif

FF_CFLAGS := $(shell $(FF_PC) --cflags $(FF_MODS) 2>/dev/null)
FF_LIBS   := $(shell $(FF_PC) --libs $(FF_MODS) 2>/dev/null)

CPPFLAGS += $(FF_CFLAGS)

# Windows: compile the icon in, link as a GUI subsystem binary so a
# double-click does not open a console behind the window, and link the
# toolchain's own runtime statically - without it the binary imports
# libstdc++-6.dll and friends, which live inside an MSYS2 installation and
# nowhere else.
ifeq ($(OS),Windows_NT)
  GUI_RES  := src/gui/bencsnip.res.o
  GUI_LINK := -mwindows -static -static-libgcc -static-libstdc++
else
  GUI_RES  :=
  GUI_LINK :=
endif

LDLIBS_GUI := $(RL_LIBS) $(RL_SYS) $(FF_LIBS)

.PHONY: all gui core test probe clean info check-deps

all: gui

gui: check-deps $(GUI)

core: $(CORE_LIB)

# ------------------------------------------------------------------
# Rules
# ------------------------------------------------------------------
%.o: %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(RL_CFLAGS) -c $< -o $@

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
	  SN_ICON_PNG     assets/icon/star-256.png \
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
	      src/core/*.d src/gui/*.d tools/*.d tests/*.d

-include $(CORE_SRC:.cpp=.d) $(GUI_SRC:.cpp=.d) $(PROBE_SRC:.cpp=.d) \
         $(TEST_SRC:.cpp=.d) src/gui/sn_embed.d

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

# The icon set, from the star the program draws. See tools/make-icons.sh.
.PHONY: icons
icons: $(GUI)
	tools/make-icons.sh
