#!/bin/bash
# Builds and runs virglrenderer/tests/neptune/test_resource_update.c: RESOURCE_UPDATE commands through the render
# server's own decoder and dispatch, linked against the objects scripts/build-server.sh built (build/server, or the
# meson build folder given as the first argument). No D3D library or guest is needed.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$HERE/build/server}"
[ -f "$BUILD/src/libvirgl.a" ] || { echo "No $BUILD/src/libvirgl.a: run scripts/build-server.sh first." >&2; exit 1; }
SRC="$HERE/virglrenderer/src"
OUT="$BUILD/tests-neptune"
mkdir -p "$OUT"
clang -std=gnu11 -O1 -Wall -Wextra -Werror -Wno-unused-parameter -imacros "$BUILD/config.h" -DHAVE_CONFIG_H=1 \
    -I"$BUILD/src" -I"$BUILD" -I"$SRC" -I"$SRC/neptune" -I"$SRC/gallium/include" -I"$SRC/gallium/auxiliary" \
    -I"$SRC/gallium/auxiliary/util" -I"$SRC/mesa" -I"$SRC/mesa/compat" -I"$SRC/mesa/pipe" -I"$SRC/mesa/util" \
    -I"$SRC/drm" -I"$SRC/drm/drm-uapi" -I"$HERE/virglrenderer" \
    -o "$OUT/test_resource_update" "$HERE/virglrenderer/tests/neptune/test_resource_update.c" \
    "$BUILD/src/libvirgl.a" "$BUILD/src/mesa/libmesa.a" -lm
NPT_LOG=1 "$OUT/test_resource_update"
