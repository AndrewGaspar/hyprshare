# Test helpers for hyprshare. Client-side protocol code is generated with wayland-scanner.

HYPRLAND_PROTOCOLS ?= $(shell pkg-config --variable=pkgdatadir hyprland-protocols)
HYPRLAND_SRC       ?= $(HOME)/.cache/hypxrland-worktrees/hyprshare-headers
WLR_TOPLEVEL_XML   ?= $(HYPRLAND_SRC)/protocols/wlr-foreign-toplevel-management-unstable-v1.xml
EXPORT_XML         ?= $(HYPRLAND_PROTOCOLS)/protocols/hyprland-toplevel-export-v1.xml
DMABUF_XML         ?= $(HYPRLAND_SRC)/subprojects/wayland-protocols/stable/linux-dmabuf/linux-dmabuf-v1.xml
XDG_SHELL_XML      ?= $(HYPRLAND_SRC)/subprojects/wayland-protocols/stable/xdg-shell/xdg-shell.xml
VPOINTER_XML       ?= $(HYPRLAND_SRC)/protocols/wlr-virtual-pointer-unstable-v1.xml
SCREENCOPY_XML     ?= $(HYPRLAND_SRC)/protocols/wlr-screencopy-unstable-v1.xml
EXT_SOURCE_XML     ?= $(HYPRLAND_SRC)/subprojects/wayland-protocols/staging/ext-image-capture-source/ext-image-capture-source-v1.xml
EXT_TOPLEVEL_XML   ?= $(HYPRLAND_SRC)/subprojects/wayland-protocols/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml
EXT_COPY_XML       ?= $(HYPRLAND_SRC)/subprojects/wayland-protocols/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml

CC     ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
GEN     = build/gen

all: build/capture-probe build/click-logger build/vpointer build/output-probe

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
$(GEN)/linux-dmabuf-v1-client.h: $(DMABUF_XML) | $(GEN)
	wayland-scanner client-header $< $@
$(GEN)/linux-dmabuf-v1.c: $(DMABUF_XML) | $(GEN)
	wayland-scanner private-code $< $@

$(GEN)/xdg-shell-client.h: $(XDG_SHELL_XML) | $(GEN)
	wayland-scanner client-header $< $@
$(GEN)/xdg-shell.c: $(XDG_SHELL_XML) | $(GEN)
	wayland-scanner private-code $< $@
$(GEN)/wlr-virtual-pointer-unstable-v1-client.h: $(VPOINTER_XML) | $(GEN)
	wayland-scanner client-header $< $@
$(GEN)/wlr-virtual-pointer-unstable-v1.c: $(VPOINTER_XML) | $(GEN)
	wayland-scanner private-code $< $@

$(GEN)/wlr-screencopy-unstable-v1-client.h: $(SCREENCOPY_XML) | $(GEN)
	wayland-scanner client-header $< $@
$(GEN)/wlr-screencopy-unstable-v1.c: $(SCREENCOPY_XML) | $(GEN)
	wayland-scanner private-code $< $@
$(GEN)/ext-image-capture-source-v1-client.h: $(EXT_SOURCE_XML) | $(GEN)
	wayland-scanner client-header $< $@
$(GEN)/ext-image-capture-source-v1.c: $(EXT_SOURCE_XML) | $(GEN)
	wayland-scanner private-code $< $@
$(GEN)/ext-image-copy-capture-v1-client.h: $(EXT_COPY_XML) | $(GEN)
	wayland-scanner client-header $< $@
$(GEN)/ext-image-copy-capture-v1.c: $(EXT_COPY_XML) | $(GEN)
	wayland-scanner private-code $< $@

$(GEN)/ext-foreign-toplevel-list-v1.c: $(EXT_TOPLEVEL_XML) | $(GEN)
	wayland-scanner private-code $< $@

OUTPUT_SRC = $(GEN)/ext-foreign-toplevel-list-v1.c $(GEN)/wlr-screencopy-unstable-v1.c $(GEN)/ext-image-capture-source-v1.c $(GEN)/ext-image-copy-capture-v1.c
OUTPUT_HDR = $(GEN)/wlr-screencopy-unstable-v1-client.h $(GEN)/ext-image-capture-source-v1-client.h $(GEN)/ext-image-copy-capture-v1-client.h

PROTO_SRC = $(GEN)/hyprland-toplevel-export-v1.c $(GEN)/wlr-foreign-toplevel-management-unstable-v1.c $(GEN)/linux-dmabuf-v1.c
PROTO_HDR = $(GEN)/hyprland-toplevel-export-v1-client.h $(GEN)/wlr-foreign-toplevel-management-unstable-v1-client.h $(GEN)/linux-dmabuf-v1-client.h

build/capture-probe: capture-probe.c $(PROTO_SRC) $(PROTO_HDR)
	$(CC) $(CFLAGS) -I$(GEN) capture-probe.c $(PROTO_SRC) -o $@ $(shell pkg-config --cflags --libs wayland-client gbm)

build/output-probe: output-probe.c $(OUTPUT_SRC) $(OUTPUT_HDR)
	$(CC) $(CFLAGS) -I$(GEN) output-probe.c $(OUTPUT_SRC) -o $@ $(shell pkg-config --cflags --libs wayland-client)

build/click-logger: click-logger.c $(GEN)/xdg-shell.c $(GEN)/xdg-shell-client.h
	$(CC) $(CFLAGS) -I$(GEN) click-logger.c $(GEN)/xdg-shell.c -o $@ $(shell pkg-config --cflags --libs wayland-client)

build/vpointer: vpointer.c $(GEN)/wlr-virtual-pointer-unstable-v1.c $(GEN)/wlr-virtual-pointer-unstable-v1-client.h
	$(CC) $(CFLAGS) -I$(GEN) vpointer.c $(GEN)/wlr-virtual-pointer-unstable-v1.c -o $@ $(shell pkg-config --cflags --libs wayland-client)

clean:
	rm -rf build

.PHONY: all clean
