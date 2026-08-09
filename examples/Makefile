# ---------------------------------------------------------------------------
# raylib-on-vulkan examples Makefile - builds every example beside its source file.
#
# Assumes librayvulkan.a has been built in ../src (it is built on demand via
# `make -C ../src` if missing).
#
#   make                          build all examples (and their SPIR-V shaders)
#   make PLATFORM=WAYLAND         select backend: X11 WAYLAND WIN32 MACOS ANDROID
#   make core                     build one category: audio core models others
#                                 shaders shapes text textures
#   make core/core_basic_window   build a single example
#   make clean                    remove example binaries and generated .spv
# ---------------------------------------------------------------------------

.PHONY: all clean spv audio core models others shaders shapes text textures
.DEFAULT_GOAL := all

CC      ?= cc
GLSLC   ?= glslc
PKG_CONFIG ?= pkg-config

RAYVK_PATH ?= ..
RAYVK_LIB  := $(RAYVK_PATH)/src/librayvulkan.a

# --- Platform backend selection --------------------------------------------
ifeq ($(origin PLATFORM), undefined)
    ifeq ($(OS),Windows_NT)
        PLATFORM := WIN32
    else
        UNAME := $(shell uname -s)
        ifeq ($(UNAME),Darwin)
            PLATFORM := MACOS
        else ifneq ($(findstring MINGW,$(UNAME))$(findstring MSYS,$(UNAME)),)
            PLATFORM := WIN32
        else
            PLATFORM := X11
        endif
    endif
endif

ifeq ($(filter $(PLATFORM),X11 WAYLAND WIN32 MACOS ANDROID),)
    $(error Unknown PLATFORM '$(PLATFORM)' (use X11, WAYLAND, WIN32, MACOS or ANDROID))
endif

EXE :=
ifeq ($(PLATFORM),WIN32)
    EXE := .exe
endif

# --- Flags ------------------------------------------------------------------
CFLAGS  ?= -O2
ALL_CFLAGS = $(CFLAGS) -std=gnu99 -Wall -Wextra -Wno-unused-parameter \
             -DVK_NO_PROTOTYPES \
             -I$(RAYVK_PATH)/src -I$(RAYVK_PATH)/src/external
LDLIBS  :=
LDFLAGS ?=

ifeq ($(PLATFORM),X11)
    LDLIBS += -lX11 -lXrandr -lXcursor -lXi -lXfixes -lm -lpthread -ldl
endif
ifeq ($(PLATFORM),WAYLAND)
    LDLIBS += $(shell $(PKG_CONFIG) --libs wayland-client wayland-cursor xkbcommon) -lm -lpthread -ldl
endif
ifeq ($(PLATFORM),WIN32)
    LDLIBS += -luser32 -lgdi32 -lshell32 -lwinmm -lole32
endif
ifeq ($(PLATFORM),MACOS)
    LDLIBS += -framework Cocoa -framework QuartzCore -framework CoreGraphics -lobjc
endif

# --- Example list ------------------------------------------------------------
# Exclusions (keep in sync with CMakeLists.txt and <category>/exclude.cmake):
EXCLUDED := \
    others/raylib_opengl_interop \
    others/rlgl_standalone \
    core/core_basic_window_web \
    core/core_input_gestures_web \
    shaders/shaders_rlgl_compute

CATEGORIES := audio core models others shaders shapes text textures
EXAMPLES   := $(filter-out $(EXCLUDED),$(basename $(wildcard $(CATEGORIES:%=%/*.c))))
EXAMPLE_BINS := $(addsuffix $(EXE),$(EXAMPLES))

# --- Vulkan GLSL shaders -> SPIR-V beside their sources ----------------------
VKSHADERS := $(wildcard $(CATEGORIES:%=%/resources/shaders/vk/*.vert) \
                        $(CATEGORIES:%=%/resources/shaders/vk/*.frag) \
                        $(CATEGORIES:%=%/resources/shaders/vk/*.comp))
SPVFILES  := $(addsuffix .spv,$(VKSHADERS))

%.spv: %
	$(GLSLC) -O -g $< -o $@

spv: $(SPVFILES)

# --- Build rules -------------------------------------------------------------
$(RAYVK_LIB):
	$(MAKE) -C $(RAYVK_PATH)/src PLATFORM=$(PLATFORM)

$(EXAMPLE_BINS): %$(EXE): %.c $(RAYVK_LIB)
	$(CC) $(ALL_CFLAGS) $< -o $@ $(RAYVK_LIB) $(LDFLAGS) $(LDLIBS)

all: $(EXAMPLE_BINS) $(SPVFILES)
	@echo "raylib-on-vulkan examples: built for platform $(PLATFORM)"

# Category shortcut targets: `make core`, `make shapes`, ...
audio:    $(filter audio/%,$(EXAMPLE_BINS))
core:     $(filter core/%,$(EXAMPLE_BINS))
models:   $(filter models/%,$(EXAMPLE_BINS))
others:   $(filter others/%,$(EXAMPLE_BINS))
shaders:  $(filter shaders/%,$(EXAMPLE_BINS)) $(SPVFILES)
shapes:   $(filter shapes/%,$(EXAMPLE_BINS))
text:     $(filter text/%,$(EXAMPLE_BINS))
textures: $(filter textures/%,$(EXAMPLE_BINS))

clean:
	rm -f $(EXAMPLE_BINS) $(SPVFILES)
