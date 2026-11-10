# Test helpers for hyprshare. Client-side protocol code is generated with wayland-scanner.

HYPRLAND_PROTOCOLS ?= $(shell pkg-config --variable=pkgdatadir hyprland-protocols)
HYPRLAND_SRC       ?= $(HOME)/.cache/hypxrland-worktrees/hyprshare-headers
WLR_TOPLEVEL_XML   ?= $(HYPRLAND_SRC)/protocols/wlr-foreign-toplevel-management-unstable-v1.xml
EXPORT_XML         ?= $(HYPRLAND_PROTOCOLS)/protocols/hyprland-toplevel-export-v1.xml

CC     ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
GEN     = build/gen

all: build/capture-probe

$(GEN):
	mkdir -p $(GEN)

$(GEN)/hyprland-toplevel-export-v1-client.h: $(EXPORT_XML) | $(GEN)
	wayland-scanner client-header $< $@
$(GEN)/hyprland-toplevel-export-v1.c: $(EXPORT_XML) | $(GEN)
	wayland-scanner private-code $< $@
$(GEN)/wlr-foreign-toplevel-management-unstable-v1-client.h: $(WLR_TOPLEVEL_XML) | $(GEN)
	wayland-scanner client-header $< $@
$(GEN)/wlr-foreign-toplevel-management-unstable-v1.c: $(WLR_TOPLEVEL_XML) | $(GEN)
	wayland-scanner private-code $< $@

PROTO_SRC = $(GEN)/hyprland-toplevel-export-v1.c $(GEN)/wlr-foreign-toplevel-management-unstable-v1.c
PROTO_HDR = $(GEN)/hyprland-toplevel-export-v1-client.h $(GEN)/wlr-foreign-toplevel-management-unstable-v1-client.h

build/capture-probe: capture-probe.c $(PROTO_SRC) $(PROTO_HDR)
	$(CC) $(CFLAGS) -I$(GEN) capture-probe.c $(PROTO_SRC) -o $@ $(shell pkg-config --cflags --libs wayland-client)

clean:
	rm -rf build

.PHONY: all clean
