#!/usr/bin/env bash
#
# Package the xemu libretro core into RetroArch-style release archives.
#
# Mirrors the layout of the upstream danprice142 alpha release: a single zip
# per platform, containing the core binary and xemu_libretro.info at the top
# level (no directory prefix), which is what RetroArch's "Download core" and
# manual-install flows expect.
#
# Usage (env-configurable, sensible defaults for this tree):
#   DLL=build/xemu_libretro.dll SO=build-linux/xemu_libretro.so \
#   VERSION=v0.1.0-emuvr-preview OUTDIR=../dist \
#   scripts/package-libretro-release.sh
#
# Set STRIP=0 to skip stripping (mirrors Dan's unstripped release).
# Set KEEP_DEBUG=1 to also emit a *-debug zip holding the unstripped binary.
#
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

DLL="${DLL:-$repo_root/build/xemu_libretro.dll}"
SO="${SO:-$repo_root/build-linux/xemu_libretro.so}"
INFO="${INFO:-$repo_root/xemu_libretro.info}"
OUTDIR="${OUTDIR:-$repo_root/../dist}"
VERSION="${VERSION:-dev}"
STRIP="${STRIP:-1}"
KEEP_DEBUG="${KEEP_DEBUG:-0}"

# Strip-tool candidates. The Windows DLL is PE/COFF and needs a mingw strip;
# names differ between a host toolchain and the MXE build container, so try a
# list and fall back gracefully.
so_strip="${SO_STRIP:-strip}"
dll_strip_candidates=("${DLL_STRIP:-}" \
  x86_64-w64-mingw32-strip x86_64-w64-mingw32.static-strip)

command -v zip >/dev/null || { echo "error: 'zip' not found" >&2; exit 1; }
[ -f "$INFO" ] || { echo "error: info file not found: $INFO" >&2; exit 1; }

mkdir -p "$OUTDIR"
OUTDIR="$(cd "$OUTDIR" && pwd)"   # zip runs from a staging dir; must be absolute
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

find_dll_strip() {
  local c
  for c in "${dll_strip_candidates[@]}"; do
    [ -n "$c" ] && command -v "$c" >/dev/null 2>&1 && { echo "$c"; return 0; }
  done
  return 1
}

# package <src-binary> <name-in-zip> <zip-basename> <strip-cmd-or-empty>
package() {
  local src="$1" member="$2" base="$3" strip_cmd="$4"
  [ -f "$src" ] || { echo "skip: $src not present"; return 0; }

  local stage="$work/$base"
  rm -rf "$stage"; mkdir -p "$stage"
  cp "$src" "$stage/$member"
  cp "$INFO" "$stage/xemu_libretro.info"

  if [ "$STRIP" = "1" ] && [ -n "$strip_cmd" ]; then
    "$strip_cmd" --strip-all "$stage/$member"
  fi

  local zip_path="$OUTDIR/${base}.zip"
  rm -f "$zip_path"
  ( cd "$stage" && zip -q -j "$zip_path" "$member" xemu_libretro.info )
  printf '  %-34s %s\n' "$(basename "$zip_path")" \
    "$(du -h "$zip_path" | cut -f1) ($(du -h "$stage/$member" | cut -f1) core, unpacked)"

  if [ "$KEEP_DEBUG" = "1" ]; then
    local dbg="$OUTDIR/${base}-debug.zip"
    rm -f "$dbg"
    ( cd "$(dirname "$src")" && zip -q -j "$dbg" "$(basename "$src")" )
    printf '  %-34s %s\n' "$(basename "$dbg")" \
      "$(du -h "$dbg" | cut -f1) (unstripped symbols)"
  fi
}

echo "Packaging xemu libretro core  version=$VERSION  strip=$STRIP  -> $OUTDIR"

dll_strip=""
if [ "$STRIP" = "1" ]; then
  dll_strip="$(find_dll_strip || true)"
  [ -f "$DLL" ] && [ -z "$dll_strip" ] && \
    echo "  warning: no mingw strip found; DLL will ship unstripped" >&2
fi

package "$DLL" xemu_libretro.dll xemu_libretro-win64        "$dll_strip"
package "$SO"  xemu_libretro.so  xemu_libretro-linux-x86_64 "$so_strip"

echo "Done."
