#!/usr/bin/env bash
# Run tests/run-isolated.sh inside a throwaway podman container, nested under a headless labwc.
#
#   tests/run-in-container.sh [log-dir]
#
# Uses the hypxrland-ctr session image (it ships labwc, kitty and jq) but NOT its Hyprland: the
# host's installed /usr/lib/hypxrland is bind-mounted read-only and tested instead, together with
# the host's exact libhyprutils (the image's is a patch release newer). No systemd, no host sockets,
# no host runtime dir: the container sees only the GPU render node and read-only mounts.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
IMAGE="${HYPRSHARE_TEST_IMAGE:-localhost/hypxrland-ctr:session-057a}"
LOGS="$(realpath -m "${1:-$ROOT/tests/build/container-logs}")"

# Pick the AMD render node (the reliable test GPU on this box), else the first one.
GPU=""
for r in /sys/class/drm/renderD*; do
    [[ "$(cat "$r/device/vendor" 2>/dev/null)" == 0x1002 ]] && GPU="/dev/dri/$(basename "$r")" && break
done
[[ -n "$GPU" ]] || GPU="$(ls /dev/dri/renderD* | head -1)"

HOSTLIBS="$(mktemp -d)"
trap 'rm -rf "$HOSTLIBS"' EXIT
cp -L /usr/lib/libhyprutils.so.* "$HOSTLIBS/" 2>/dev/null || true

mkdir -p "$LOGS"
NAME="hyprshare-test-$$"
echo "container $NAME, image $IMAGE, GPU $GPU, logs -> $LOGS"

podman run --rm --name "$NAME" \
    --entrypoint /bin/bash \
    --userns=keep-id --user "$(id -u):$(id -g)" --group-add keep-groups \
    --security-opt label=disable \
    --device "$GPU" \
    -v /usr/lib/hypxrland:/opt/hypxrland:ro \
    -v "$HOSTLIBS":/opt/hostlibs:ro \
    -v "$ROOT":/hyprshare:ro \
    -v "$LOGS":/logs \
    -e GPU="$GPU" \
    "$IMAGE" -c '
        set -u
        NEST=$(mktemp -d /tmp/nest.XXXXXX); chmod 0700 "$NEST"
        env -i PATH=/usr/bin HOME=/tmp XDG_RUNTIME_DIR="$NEST" WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 \
            WLR_RENDERER_ALLOW_SOFTWARE=1 WLR_RENDER_DRM_DEVICE="$GPU" labwc >/logs/labwc.out 2>&1 &
        LABWC=$!
        for _ in $(seq 100); do [[ -S "$NEST/wayland-0" ]] && break; sleep 0.1; done
        [[ -S "$NEST/wayland-0" ]] || { echo "labwc did not come up"; cat /logs/labwc.out; exit 1; }
        NEST_WAYLAND="$NEST/wayland-0" HYPRLAND_BIN=/opt/hypxrland/Hyprland HYPRCTL_BIN=/opt/hypxrland/hyprctl \
            HYPRLAND_LIBRARY_PATH=/opt/hostlibs TERM_BIN=/usr/bin/kitty LOG_EXPORT_DIR=/logs \
            /hyprshare/tests/run-isolated.sh /hyprshare/build/hyprshare-v2.so
        RC=$?
        kill "$LABWC" 2>/dev/null
        exit $RC
    '
