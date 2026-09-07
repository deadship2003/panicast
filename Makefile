# panicast — unified build abstraction (LIF-006).
#   Wraps CMake/Ninja behind the standard 9-target interface. All build
#   artifacts land in ./bin/. Environment prep lives in setup.sh (never
#   compiles); this layer never installs system packages.
#
# Standard flow:
#   ./setup.sh && ./build.sh     (top-level scripts)
#   make build                   (or drive make directly)
#   sudo make install            (file-layer deploy → /usr/local)
#
# Overridables:
#   make build BUILD_TYPE=Debug  JOBS=8
#   make install PREFIX=/usr/local
BUILD_DIR  := build
OUTPUT_DIR := bin
PREFIX     ?= /usr/local
BUILD_TYPE ?= Release
JOBS       ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# -DBUILD_TESTING is explicit per target so the build-dir cache never carries
# a sticky testing flag into plain release builds.
CMAKE_CONF := cmake -B $(BUILD_DIR) -G Ninja -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DBUILD_TESTING=OFF

# Files lint/fmt operate on: the C++ sources of this repo.
CXX_SOURCES := $(shell find src include tests -name '*.cpp' -o -name '*.h' -o -name '*.hpp' 2>/dev/null)

.PHONY: all build clean distclean fmt lint test install uninstall

all: build

build:
	$(CMAKE_CONF)
	cmake --build $(BUILD_DIR) --parallel $(JOBS)

clean:
	rm -rf $(OUTPUT_DIR)

distclean: clean
	rm -rf $(BUILD_DIR)

fmt:
	clang-format -i $(CXX_SOURCES)

# Static analysis via clang-tidy against the configured build dir.
# Default scope: files changed vs HEAD (fast feedback). Override:
#   make lint LINT_FILES="src/app/app_run.cpp include/panicast/app/app.h"
lint:
	@if [ -z "$(LINT_FILES)" ]; then \
	    files=$$(git diff --name-only HEAD -- '*.cpp' '*.h' '*.hpp' 2>/dev/null || true); \
	else \
	    files="$(LINT_FILES)"; \
	fi; \
	if [ -z "$$files" ]; then \
	    echo "lint: no C++ files changed (nothing to check)"; \
	    exit 0; \
	fi; \
	if [ ! -f $(BUILD_DIR)/compile_commands.json ]; then \
	    echo "lint: configuring $(BUILD_DIR) for compile_commands.json"; \
	    $(CMAKE_CONF) >/dev/null || exit 1; \
	fi; \
	clang-tidy -p $(BUILD_DIR) $$files

test:
	$(CMAKE_CONF) -DBUILD_TESTING=ON
	cmake --build $(BUILD_DIR) --parallel $(JOBS)
	ctest --test-dir $(BUILD_DIR) --output-on-failure

# File-layer install (LIF-006). Service registration is a separate layer:
#   panicast service install   (LIF-001, unit generation)
install:
	sudo cmake --install $(BUILD_DIR) --prefix $(PREFIX)

# Inverse of the install manifest above (binary, JS runtime, docs, man page).
# Existence-gated: no sudo prompt when nothing is installed.
uninstall:
	@files="$(PREFIX)/bin/panicast $(PREFIX)/bin/qjs $(PREFIX)/share/doc/panicast/README.md $(PREFIX)/share/doc/panicast/LICENSE $(PREFIX)/share/man/man1/panicast.1"; \
	rm_list=""; \
	for f in $$files; do [ -e "$$f" ] && rm_list="$$rm_list $$f"; done; \
	if [ -n "$$rm_list" ]; then \
	    echo "removing:$$rm_list"; \
	    sudo rm -f $$rm_list; \
	    sudo rmdir $(PREFIX)/share/doc/panicast 2>/dev/null || true; \
	else \
	    echo "uninstall: nothing installed under $(PREFIX) — nothing to do"; \
	fi
