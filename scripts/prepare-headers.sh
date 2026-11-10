#!/usr/bin/env bash
# Prepare a Hyprland source tree whose headers match a packaged hypxrland build exactly.
#
#   scripts/prepare-headers.sh [commit] [tree]
#
# Defaults: the commit the RUNNING compositor reports (`hyprctl version -j`), and
# ~/.cache/hypxrland-worktrees/hyprshare-headers. Creates a detached worktree of ~/code/Hyprland,
# checks out submodules, configures it the way pkgbuilds/hypxrland/PKGBUILD does (Release,
# WITH_OPENXR, NO_HYPRPM, no tests, the package's GIT_* env so src/version.h is identical), and
# generates only the protocol headers — no compositor build is needed for the plugin.
#
# hyprwayland-scanner is not installed on this host; it is extracted from the pacman cache into
# ~/.cache/hyprshare-tools (no root, nothing installed). glaze is taken at the package's pin.

set -euo pipefail

REPO="${HYPRLAND_REPO:-$HOME/code/Hyprland}"
COMMIT="${1:-$(/usr/lib/hypxrland/hyprctl version -j | jq -r .commit)}"
TREE="${2:-$HOME/.cache/hypxrland-worktrees/hyprshare-headers}"
TOOLS="$HOME/.cache/hyprshare-tools"
GLAZE_COMMIT=b518eec7a22e56ffa238b072c07f47efa7cea97f
GLAZE="$TOOLS/glaze-${GLAZE_COMMIT:0:8}"

if [[ ! -d "$TREE" ]]; then
    git -C "$REPO" worktree add --detach "$TREE" "$COMMIT"
    git -C "$TREE" submodule update --init
fi
[[ "$(git -C "$TREE" rev-parse HEAD)" == "$(git -C "$REPO" rev-parse "$COMMIT")" ]] || { echo "$TREE is not at $COMMIT"; exit 1; }

mkdir -p "$TOOLS"
if [[ ! -x "$TOOLS/usr/bin/hyprwayland-scanner" ]]; then
    PKG="$(ls /var/cache/pacman/pkg/hyprwayland-scanner-*.pkg.tar.zst | sort -V | tail -1)"
    bsdtar -xf "$PKG" -C "$TOOLS" usr
fi
if [[ ! -d "$GLAZE" ]]; then
    SRC="${GLAZE_SRC:-$REPO/build-debug/_deps/glaze-src}"
    git -C "$SRC" archive --prefix="$(basename "$GLAZE")/" "$GLAZE_COMMIT" | tar -x -C "$TOOLS"
fi

# The package's version stamp (PKGBUILD build()). Only GIT_COMMIT_HASH enters the plugin ABI string
# (__hyprland_api_get_client_hash); the rest is cosmetic — override GIT_TAG / GIT_COMMITS to match a
# PKGBUILD exactly. GIT_DIR points nowhere so CMake's own git probing fails and these win, as in
# the tarball build.
GIT_COMMIT_HASH="$(git -C "$TREE" rev-parse HEAD)"
GIT_COMMIT_DATE="${GIT_COMMIT_DATE:-$(git -C "$TREE" show -s --format=%cs HEAD)}"
export GIT_COMMIT_HASH GIT_COMMIT_DATE
export GIT_BRANCH=hypxrland
export GIT_COMMIT_MESSAGE="packaged HypXRland snapshot ${GIT_COMMIT_HASH:0:10}"
export GIT_DIRTY=clean
export GIT_TAG="${GIT_TAG:-unknown}"
export GIT_COMMITS="${GIT_COMMITS:-0}"
export GIT_DIR=/nonexistent-hyprshare
export PATH="$TOOLS/usr/bin:$PATH"

cmake -S "$TREE" -B "$TREE/build-pkg" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_SKIP_RPATH=ON \
    -DFETCHCONTENT_SOURCE_DIR_GLAZE="$GLAZE" \
    -DNO_HYPRPM=ON \
    -DNO_TESTS=ON \
    -DBUILD_TESTING=OFF \
    -DWITH_OPENXR=ON \
    -DUSE_TRACY=OFF \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -Dhyprwayland-scanner_DIR="$TOOLS/usr/lib/cmake/hyprwayland-scanner"

mapfile -t PROTOS < <(ninja -C "$TREE/build-pkg" -t targets all | grep -oE '^[^:]+' | grep -E '(^|/)protocols/[^/]*\.(hpp|cpp|h|c)$' | grep -v CMakeFiles | sort -u)
ninja -C "$TREE/build-pkg" -j8 "${PROTOS[@]}"

echo "headers ready: $TREE (version.h hash: $(sed -n 's/^#define GIT_COMMIT_HASH *"\(.*\)"/\1/p' "$TREE/src/version.h"))"
