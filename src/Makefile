# ---------------------------------------------------------------------------
# raylib-on-vulkan src Makefile - builds librayvulkan.a (GNU Make alternative to CMake).
#
# Requires: GNU Make, a C99 compiler, glslc. On Windows use an MSYS2/MinGW or
# w64devkit shell (a POSIX sh is needed for the recipes).
#
#   make                          build librayvulkan.a (into this directory)
#   make PLATFORM=WAYLAND         select backend: X11 WAYLAND WIN32 MACOS ANDROID
#   make clean                    remove objects, generated files and the library
#
# Examples and tests have their own Makefiles: ../examples and ../tests.
#
# Cross-compiling (e.g. Android): set CC/AR to the NDK toolchain and keep
# HOSTCC a native compiler (it builds the shader embedding tool). Android
# consumers must link with -Wl,-u,ANativeActivity_onCreate.
# ---------------------------------------------------------------------------

.PHONY: all lib clean
.DEFAULT_GOAL := all

CC      ?= cc
HOSTCC  ?= $(CC)
AR      ?= ar
GLSLC   ?= glslc
PKG_CONFIG ?= pkg-config

RAYVK_SRC_PATH     ?= .
RAYVK_RELEASE_PATH ?= $(RAYVK_SRC_PATH)
GENDIR             := $(RAYVK_SRC_PATH)/generated

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
ALL_CFLAGS = $(CFLAGS) -std=c99 -Wall -Wextra -Wno-unused-parameter -MMD -MP \
             -DPLATFORM_$(PLATFORM) -DVK_NO_PROTOTYPES \
             -I$(RAYVK_SRC_PATH) -I$(RAYVK_SRC_PATH)/external -I$(GENDIR)

ifeq ($(PLATFORM),WAYLAND)
    WL_PKGS := wayland-client wayland-cursor xkbcommon
    ALL_CFLAGS += $(shell $(PKG_CONFIG) --cflags $(WL_PKGS)) -I$(GENDIR)/wayland
endif
ifeq ($(PLATFORM),MACOS)
    # objc_msgSend must be cast to the exact message signature at every call site
    ALL_CFLAGS += -Wno-cast-function-type
endif

# --- Sources ----------------------------------------------------------------
SRCS := rcore.c rlvk.c rshapes.c rtextures.c rtext.c rmodels.c raudio.c \
        utils.c external/volk.c

ifeq ($(PLATFORM),X11)
    SRCS += platforms/rcore_x11.c
endif
ifeq ($(PLATFORM),WAYLAND)
    SRCS += platforms/rcore_wayland.c
endif
ifeq ($(PLATFORM),WIN32)
    SRCS += platforms/rcore_win32.c
endif
ifeq ($(PLATFORM),MACOS)
    SRCS += platforms/rcore_macos.c
endif
ifeq ($(PLATFORM),ANDROID)
    SRCS += platforms/rcore_android.c
endif

OBJS := $(SRCS:%.c=%.o)

# --- Default shaders: GLSL -> SPIR-V -> embedded C arrays -------------------
SHADERS    := shaders/default.vert shaders/default.frag
EMBED_TOOL := $(GENDIR)/embed_spirv$(EXE)

$(EMBED_TOOL): ../tools/embed_spirv.c
	@mkdir -p $(@D)
	$(HOSTCC) -O2 -o $@ $<

# $(1) = shader path, $(2) = symbol base (dots replaced by underscores)
define SHADER_RULE
$(GENDIR)/$(2)_spv.c: $(1) $(EMBED_TOOL)
	@mkdir -p $$(@D)
	$(GLSLC) -O -g $(1) -o $(GENDIR)/$(2).spv
	$(EMBED_TOOL) $(GENDIR)/$(2).spv $(2)_spv $$@
endef
# -g keeps OpName for SPIR-V reflection
$(foreach s,$(SHADERS),$(eval $(call SHADER_RULE,$(s),$(subst .,_,$(notdir $(s))))))

SHADER_GEN := $(foreach s,$(SHADERS),$(GENDIR)/$(subst .,_,$(notdir $(s)))_spv.c)
OBJS += $(SHADER_GEN:.c=.o)

# --- Wayland protocol glue --------------------------------------------------
ifeq ($(PLATFORM),WAYLAND)
    WAYLAND_SCANNER ?= wayland-scanner
    WLPROTO_DIR := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
    ifeq ($(WLPROTO_DIR),)
        $(error Could not determine the wayland-protocols pkgdatadir)
    endif

    WLPROTO_XML := $(WLPROTO_DIR)/stable/xdg-shell/xdg-shell.xml \
                   $(WLPROTO_DIR)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml \
                   $(WLPROTO_DIR)/unstable/relative-pointer/relative-pointer-unstable-v1.xml \
                   $(WLPROTO_DIR)/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml

    WL_GEN_DIR := $(GENDIR)/wayland
    WL_HEADERS := $(foreach x,$(WLPROTO_XML),$(WL_GEN_DIR)/$(basename $(notdir $(x)))-client-protocol.h)
    WL_SOURCES := $(foreach x,$(WLPROTO_XML),$(WL_GEN_DIR)/$(basename $(notdir $(x)))-protocol.c)

$(WL_GEN_DIR)/%-client-protocol.h:
	@mkdir -p $(@D)
	$(WAYLAND_SCANNER) client-header $(filter %/$*.xml,$(WLPROTO_XML)) $@

$(WL_GEN_DIR)/%-protocol.c:
	@mkdir -p $(@D)
	$(WAYLAND_SCANNER) private-code $(filter %/$*.xml,$(WLPROTO_XML)) $@

    OBJS += $(WL_SOURCES:.c=.o)

    # Pattern-rule outputs count as intermediates: keep them or make regenerates every build
.SECONDARY: $(WL_HEADERS) $(WL_SOURCES)

    # wayland-scanner output triggers warnings we do not control
$(WL_GEN_DIR)/%.o: private ALL_CFLAGS += -w
platforms/rcore_wayland.o: $(WL_HEADERS)
endif

# --- Library ----------------------------------------------------------------
LIB := $(RAYVK_RELEASE_PATH)/librayvulkan.a

all: lib
	@echo "raylib-on-vulkan: built for platform $(PLATFORM)"

lib: $(LIB)

$(LIB): $(OBJS)
	@mkdir -p $(@D)
	$(AR) rcs $@ $(OBJS)

%.o: %.c
	$(CC) $(ALL_CFLAGS) -c $< -o $@

clean:
	rm -f $(LIB) *.o *.d external/*.o external/*.d platforms/*.o platforms/*.d
	rm -rf $(GENDIR)

-include $(OBJS:.o=.d)
