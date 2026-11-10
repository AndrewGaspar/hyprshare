# hyprshare — Hyprland plugin (spike)
#
# Builds against a Hyprland SOURCE TREE checked out at exactly the commit the running compositor was
# built from (NOT /usr/include/hyprland: that belongs to stock hyprland, with different layouts).
# The tree must have been configured once so src/version.h and protocols/*.hpp exist — see README.md
# ("Preparing the headers tree") or scripts/prepare-headers.sh.

HYPRLAND_SRC ?= $(HOME)/.cache/hypxrland-worktrees/hyprshare-headers
# glaze is header-only and only reachable through some Hyprland headers; same pin as the package.
GLAZE_INCLUDE ?= $(HOME)/.cache/hyprshare-tools/glaze-b518eec7/include

CXX ?= c++

# Defines that hyprland_lib is compiled with in the hypxrland package build (Release, WITH_OPENXR=ON,
# Vulkan headers present, systemd). Taken from build-pkg/compile_commands.json. HAVE_OPENXR and
# NDEBUG change what headers declare, so they must match the compositor.
HYPRLAND_DEFINES = \
	-DNDEBUG \
	-DHAVE_OPENXR=1 \
	-DHAVE_XR_VULKAN_PROBE=1 \
	-DUSES_SYSTEMD \
	-DHAS_EXECINFO \
	-DNOMINMAX \
	-DGLSLANG_IS_SHARED_LIBRARY=1 \
	'-DHYPRLAND_VERSION="0.56.2"' \
	'-DDATAROOTDIR="/usr/share"'

PKGS = pixman-1 libdrm pangocairo libinput libudev wayland-server xkbcommon aquamarine hyprutils hyprlang hyprgraphics hyprcursor lua

CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=gnu++26 -fPIC -fno-lto -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-narrowing -Wno-pointer-arith
CPPFLAGS += $(HYPRLAND_DEFINES) \
	-I$(HYPRLAND_SRC) -I$(HYPRLAND_SRC)/src -I$(HYPRLAND_SRC)/protocols \
	-isystem $(GLAZE_INCLUDE) \
	$(shell pkg-config --cflags $(PKGS))
LDFLAGS += -shared
LDLIBS += $(shell pkg-config --libs lua wayland-server)

SRC = src/main.cpp src/ShareRedirector.cpp
OBJ = $(SRC:src/%.cpp=build/%.o)
TARGET = build/hyprshare.so

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

build/%.o: src/%.cpp src/*.hpp | build check-tree
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

build:
	mkdir -p build

check-tree:
	@test -f $(HYPRLAND_SRC)/src/version.h || { echo "$(HYPRLAND_SRC)/src/version.h missing: prepare the headers tree first (README.md)"; exit 1; }
	@test -f $(HYPRLAND_SRC)/protocols/hyprland-toplevel-export-v1.hpp || { echo "generated protocol headers missing in $(HYPRLAND_SRC)/protocols"; exit 1; }

# Prints the hash this plugin was compiled for (from the tree's version.h) next to the live one.
hash: check-tree
	@echo "plugin tree: $$(sed -n 's/^#define GIT_COMMIT_HASH *"\(.*\)"/\1/p' $(HYPRLAND_SRC)/src/version.h)"
	@echo "compositor:  $$(hyprctl version -j 2>/dev/null | sed -n 's/.*"commit": *"\([0-9a-f]*\)".*/\1/p')"

tests:
	$(MAKE) -C tests

clean:
	rm -rf build
	$(MAKE) -C tests clean

.PHONY: all check-tree hash tests clean
