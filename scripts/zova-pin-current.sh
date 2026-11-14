#!/usr/bin/env bash
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
ROOT=$(cd "$ROOT" && pwd -P)
SOURCE_ROOT=${ZOVA_SOURCE_ROOT:-${ZOVA_ROOT:-"$ROOT/../zova"}}
PIN_ROOT=${CBM_ZOVA_PIN_ROOT:-"$ROOT/.zova-sdk"}
REFRESH=0

if [[ "${1:-}" == "--refresh" ]]; then
  REFRESH=1
  shift
fi
if [[ "$#" -ne 0 ]]; then
  echo "usage: $0 [--refresh]" >&2
  exit 2
fi

SOURCE_ROOT=$(cd "$SOURCE_ROOT" && pwd -P)
SOURCE_HEADER="$SOURCE_ROOT/include/zova.h"
SOURCE_LIBRARY="$SOURCE_ROOT/zig-out/lib/libzova_c.a"
SOURCE_ROOT_ZIG="$SOURCE_ROOT/src/root.zig"
SOURCE_VERSION_ZIG="$SOURCE_ROOT/src/version.zig"

for path in "$SOURCE_HEADER" "$SOURCE_LIBRARY" "$SOURCE_ROOT_ZIG" "$SOURCE_VERSION_ZIG"; do
  if [[ ! -f "$path" ]]; then
    echo "error: current Zova SDK artifact is missing: $path" >&2
    echo "Build Zova explicitly once, then rerun this command." >&2
    exit 1
  fi
done

format_version=$(sed -n 's/^pub const format_version = "\([^"]*\)";$/\1/p' "$SOURCE_VERSION_ZIG")
abi_version=$(sed -n 's/^pub const abi_version_string = "\([^"]*\)";$/\1/p' "$SOURCE_VERSION_ZIG")
if [[ "$format_version" != "13" ]]; then
  echo "error: current Zova SDK must use format version 13, found: ${format_version:-missing}" >&2
  exit 1
fi
if [[ -z "$abi_version" ]]; then
  echo "error: current Zova SDK does not declare abi_version_string" >&2
  exit 1
fi
for symbol in zova_graph_edge_delete_many zova_vector_delete_many zova_graph_build_fresh_keyed \
              zova_graph_build_fresh_prepared_keyed \
              zova_graph_build_fresh_prepared_keyed_with_payloads \
              zova_graph_edge_payload_get_many zova_graph_edge_payload_replace_many \
              zova_fresh_build_begin; do
  if ! grep -Fq "zova_status $symbol(" "$SOURCE_HEADER"; then
    echo "error: current Zova SDK header is missing required symbol: $symbol" >&2
    exit 1
  fi
done

source_commit=$(git -C "$SOURCE_ROOT" rev-parse HEAD 2>/dev/null || true)
if [[ -z "$source_commit" ]]; then
  echo "error: current Zova SDK source is not at an identifiable git commit" >&2
  exit 1
fi
if ! git -C "$SOURCE_ROOT" diff --quiet -- include src build.zig ||
   ! git -C "$SOURCE_ROOT" diff --cached --quiet -- include src build.zig; then
  echo "error: current Zova SDK has tracked source changes not represented by $source_commit" >&2
  exit 1
fi

if [[ -e "$PIN_ROOT" && "$REFRESH" -ne 1 ]]; then
  echo "error: pinned Zova SDK already exists: $PIN_ROOT" >&2
  echo "Use --refresh only when you intentionally want a newer Zova snapshot." >&2
  exit 1
fi

PARENT=$(dirname "$PIN_ROOT")
mkdir -p "$PARENT"
TMP=$(mktemp -d "$PARENT/.zova-sdk.tmp.XXXXXX")
BACKUP=""
cleanup() {
  rm -rf "$TMP"
  if [[ -n "$BACKUP" && -d "$BACKUP" && ! -e "$PIN_ROOT" ]]; then
    mv "$BACKUP" "$PIN_ROOT"
  fi
}
trap cleanup EXIT INT TERM

mkdir -p "$TMP/include" "$TMP/zig-out/lib"
cp "$SOURCE_HEADER" "$TMP/include/zova.h"
cp "$SOURCE_LIBRARY" "$TMP/zig-out/lib/libzova_c.a"
cp -R "$SOURCE_ROOT/src" "$TMP/src"

header_hash=$(shasum -a 256 "$TMP/include/zova.h" | awk '{print $1}')
library_hash=$(shasum -a 256 "$TMP/zig-out/lib/libzova_c.a" | awk '{print $1}')
root_hash=$(shasum -a 256 "$TMP/src/root.zig" | awk '{print $1}')
source_tree_hash=$(
  cd "$TMP/src"
  find . -type f -print | LC_ALL=C sort | while IFS= read -r path; do
    printf '%s  ' "$path"
    shasum -a 256 "$path" | awk '{print $1}'
  done | shasum -a 256 | awk '{print $1}'
)
cat > "$TMP/manifest.txt" <<EOF
format=cbm-zova-sdk-v1
source_root=$SOURCE_ROOT
source_commit=$source_commit
format_version=$format_version
abi_version=$abi_version
graph_edge_delete_many_symbol=zova_graph_edge_delete_many
vector_delete_many_symbol=zova_vector_delete_many
graph_build_fresh_keyed_symbol=zova_graph_build_fresh_keyed
graph_build_fresh_prepared_keyed_symbol=zova_graph_build_fresh_prepared_keyed
graph_build_fresh_prepared_keyed_with_payloads_symbol=zova_graph_build_fresh_prepared_keyed_with_payloads
graph_edge_payload_get_many_symbol=zova_graph_edge_payload_get_many
graph_edge_payload_replace_many_symbol=zova_graph_edge_payload_replace_many
fresh_build_begin_symbol=zova_fresh_build_begin
pinned_at_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)
header_sha256=$header_hash
library_sha256=$library_hash
root_zig_sha256=$root_hash
source_tree_sha256=$source_tree_hash
EOF

if [[ -e "$PIN_ROOT" ]]; then
  BACKUP="$PARENT/.zova-sdk.previous.$$"
  mv "$PIN_ROOT" "$BACKUP"
fi
mv "$TMP" "$PIN_ROOT"
TMP=""
if [[ -n "$BACKUP" ]]; then
  rm -rf "$BACKUP"
  BACKUP=""
fi
trap - EXIT INT TERM

echo "PINNED ZOVA SDK: $PIN_ROOT"
echo "LIBRARY SHA256: $library_hash"
