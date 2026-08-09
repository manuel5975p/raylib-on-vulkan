# ---------------------------------------------------------------------------
# raylib-on-vulkan tests Makefile - builds every test beside its source file.
#
# Assumes librayvulkan.a has been built in ../src (it is built on demand via
# `make -C ../src` if missing).
#
#   make                          build all tests
#   make PLATFORM=WAYLAND         select backend: X11 WAYLAND WIN32 MACOS ANDROID
#   make test                     run the test suite (60s timeout per test)
#   make clean                    remove test binaries
#
# Tests probe edge cases: crashes/hangs are expected findings, so runs are
# capped by a timeout. Window tests are skipped without DISPLAY/WAYLAND_DISPLAY.
# ---------------------------------------------------------------------------

.PHONY: all test clean
.DEFAULT_GOAL := all

CC      ?= cc
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
ALL_CFLAGS = $(CFLAGS) -std=c99 -Wall -Wextra -Wno-unused-parameter \
             -I$(RAYVK_PATH)/src
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

# --- Test list ---------------------------------------------------------------
TESTS_NOWINDOW := test_text test_colors test_images test_compression test_files test_collision
TESTS_WINDOW   := test_window_smoke test_render_readback
TEST_BINS := $(addsuffix $(EXE),$(TESTS_NOWINDOW) $(TESTS_WINDOW))

$(RAYVK_LIB):
	$(MAKE) -C $(RAYVK_PATH)/src PLATFORM=$(PLATFORM)

$(TEST_BINS): %$(EXE): %.c $(RAYVK_LIB)
	$(CC) $(ALL_CFLAGS) $< -o $@ $(RAYVK_LIB) $(LDFLAGS) $(LDLIBS)

all: $(TEST_BINS)

TIMEOUT_CMD := $(shell command -v timeout 2>/dev/null)
RUN_TIMEOUT := $(if $(TIMEOUT_CMD),$(TIMEOUT_CMD) 60,)

test: $(TEST_BINS)
	@fail=0; \
	for t in $(TESTS_NOWINDOW); do \
	    echo "== $$t"; \
	    $(RUN_TIMEOUT) ./$$t$(EXE) || { echo "FAILED: $$t"; fail=1; }; \
	done; \
	if [ -n "$$DISPLAY$$WAYLAND_DISPLAY" ]; then \
	    for t in $(TESTS_WINDOW); do \
	        echo "== $$t"; \
	        $(RUN_TIMEOUT) ./$$t$(EXE) || { echo "FAILED: $$t"; fail=1; }; \
	    done; \
	else \
	    echo "skipping window tests (no DISPLAY/WAYLAND_DISPLAY)"; \
	fi; \
	exit $$fail

clean:
	rm -f $(TEST_BINS)
