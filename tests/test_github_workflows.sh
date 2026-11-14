#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd -P)
WORKFLOWS="$ROOT/.github/workflows"

expected=$(printf '%s\n' _build.yml ci.yml release.yml)
actual=$(find "$WORKFLOWS" -maxdepth 1 -type f -name '*.yml' -exec basename {} \; | sort)
if [[ "$actual" != "$expected" ]]; then
  echo "error: unexpected GitHub workflow set" >&2
  diff -u <(printf '%s\n' "$expected") <(printf '%s\n' "$actual") || true
  exit 1
fi

for path in \
  "$ROOT/.github/actions/setup-zova/action.yml" \
  "$WORKFLOWS/ci.yml" \
  "$WORKFLOWS/_build.yml" \
  "$WORKFLOWS/release.yml"; do
  [[ -f "$path" ]] || { echo "error: missing $path" >&2; exit 1; }
done

grep -q 'repository: ata-sesli/zova' "$ROOT/.github/actions/setup-zova/action.yml"
grep -q 'ref: 23a75306d6c9ff44a6bdf24c8120b302439ce726' \
  "$ROOT/.github/actions/setup-zova/action.yml"
grep -q 'version: 0.17.0' "$ROOT/.github/actions/setup-zova/action.yml"
grep -q '^  cpu:' "$ROOT/.github/actions/setup-zova/action.yml"
grep -Fq '${{ inputs.cpu }}' "$ROOT/.github/actions/setup-zova/action.yml"
grep -q 'BUILD_ARGS=(c-abi' "$ROOT/.github/actions/setup-zova/action.yml"
grep -q 'BUILD_ARGS+=(-Dcpu="$CPU")' "$ROOT/.github/actions/setup-zova/action.yml"
grep -q 'actions/cache@55cc8345863c7cc4c66a329aec7e433d2d1c52a9' \
  "$ROOT/.github/actions/setup-zova/action.yml"
grep -q "steps.zova-cache.outputs.cache-hit != 'true'" \
  "$ROOT/.github/actions/setup-zova/action.yml"
grep -q 'cygpath -m' "$ROOT/.github/actions/setup-zova/action.yml"
grep -q 'zova_c.lib' "$ROOT/.github/actions/setup-zova/action.yml"

grep -q 'pull_request:' "$WORKFLOWS/ci.yml"
grep -q 'push:' "$WORKFLOWS/ci.yml"
grep -q 'tests/test_zova_build_workflow.sh' "$WORKFLOWS/ci.yml"
grep -q 'scripts/check-build-sources.sh' "$WORKFLOWS/ci.yml"
grep -q '^  sdk:' "$WORKFLOWS/ci.yml"
grep -q '^  build:' "$WORKFLOWS/ci.yml"
grep -q '^  test-build:' "$WORKFLOWS/ci.yml"
grep -q '^  test:' "$WORKFLOWS/ci.yml"
grep -q 'scripts/ci-test-shard.sh' "$WORKFLOWS/ci.yml"
grep -q 'scripts/smoke-test.sh' "$WORKFLOWS/ci.yml"
test_build_job=$(sed -n '/^  test-build:/,/^  test:/p' "$WORKFLOWS/ci.yml")
[[ $(grep -c 'cpu: baseline' <<<"$test_build_job") -eq 1 ]] || {
  echo "error: the shared test runner must use one baseline-CPU Zova build" >&2
  exit 1
}
[[ $(grep -c 'CBM_ZOVA_TEST_CPU: baseline' <<<"$test_build_job") -eq 1 ]] || {
  echo "error: the shared CBM test runner must target the baseline CPU" >&2
  exit 1
}
grep -q 'key: cbm-tests-.*-cpu-baseline-' <<<"$test_build_job"
# Zova bundles compiler_rt's memcpy/memset, so a host-CPU Zova archive restored
# on an older runner (or shipped to a user) dies with SIGILL at startup.
sdk_job=$(sed -n '/^  sdk:/,/^  build:/p' "$WORKFLOWS/ci.yml")
build_job=$(sed -n '/^  build:/,/^  test-build:/p' "$WORKFLOWS/ci.yml")
grep -q 'cpu: baseline' <<<"$sdk_job" || {
  echo "error: the sdk job must warm the baseline-CPU Zova cache" >&2
  exit 1
}
grep -q 'cpu: baseline' <<<"$build_job" || {
  echo "error: the production build must link a baseline-CPU Zova build" >&2
  exit 1
}
grep -q 'key: cbm-prod-.*-cpu-baseline-' <<<"$build_job"
# CI must also build through scripts/build.sh and Makefile.cbm, the path the
# release jobs use; build.zig alone hid a Makefile-only include error.
release_build_job=$(sed -n '/^  release-build:/,$p' "$WORKFLOWS/ci.yml")
grep -q 'scripts/build.sh' <<<"$release_build_job" || {
  echo "error: CI must build the release path (scripts/build.sh)" >&2
  exit 1
}
grep -q 'cpu: baseline' <<<"$release_build_job"
grep -q 'scripts/smoke-test.sh' <<<"$release_build_job"
# Release jobs that build Zova for the runner's own platform; zova-windows
# cross-builds with an explicit -Dtarget, which already means a baseline CPU.
for job in unix linux-portable; do
  job_text=$(sed -n "/^  $job:/,/^  [a-z-]*:\$/p" "$WORKFLOWS/_build.yml")
  grep -q 'cpu: baseline' <<<"$job_text" || {
    echo "error: release job $job must build Zova for the baseline CPU" >&2
    exit 1
  }
done
[[ $(grep -c '^ZIG_TARGET_FLAGS := .*-mcpu=baseline' "$ROOT/Makefile.cbm") -eq 2 ]] || {
  echo "error: Makefile.cbm Zig objects must target the baseline CPU" >&2
  exit 1
}
grep -q 'CBM_ZOVA_TEST_CPU' "$ROOT/scripts/zova-build-test-runner.sh"
grep -q -- '-Dcpu="$TEST_CPU"' "$ROOT/scripts/zova-build-test-runner.sh"
bash "$ROOT/scripts/ci-test-shard.sh" --verify

grep -q 'CBM_WITH_ZOVA=1' "$WORKFLOWS/_build.yml"
grep -q 'ZOVA_ROOT=' "$WORKFLOWS/_build.yml"
grep -q 'zova_fresh_build_begin' "$WORKFLOWS/_build.yml"
grep -q 'cbm_zova_publish_model.zig' "$ROOT/Makefile.cbm"
grep -q 'if \$WITH_ZOVA' "$ROOT/scripts/build.sh"
grep -q 'codebase-memory-mcp-linux-.*-portable.tar.gz' "$WORKFLOWS/_build.yml"
grep -q 'os: darwin' "$WORKFLOWS/_build.yml"
grep -q 'aarch64-macos.15.0' "$WORKFLOWS/_build.yml"
grep -q 'x86_64-macos.15.0' "$WORKFLOWS/_build.yml"
grep -q '^  zova-windows:' "$WORKFLOWS/_build.yml"
grep -q 'needs: zova-windows' "$WORKFLOWS/_build.yml"
grep -q 'zova-windows-${{ matrix.arch }}' "$WORKFLOWS/_build.yml"
grep -q 'Build CBM Zig support for Windows' "$WORKFLOWS/_build.yml"
grep -q 'windows-zig/libcbm_zig.a' "$WORKFLOWS/_build.yml"
grep -q 'windows-zig/cbm_zova_bridge.o' "$WORKFLOWS/_build.yml"
grep -q 'CBM_PREBUILT_ZIG_DIR: .ci/windows-zig' "$WORKFLOWS/_build.yml"
grep -q 'ZOVA_ROOT_PATH=$(cygpath -m' "$WORKFLOWS/_build.yml"
grep -q 'codebase-memory-mcp-windows-' "$WORKFLOWS/_build.yml"
grep -q -- '-fcompiler-rt' "$ROOT/Makefile.cbm"
grep -q -- '-lntdll' "$ROOT/Makefile.cbm"
grep -q 'CBM_PREBUILT_ZIG ?= 0' "$ROOT/Makefile.cbm"
grep -Fq 'ZIG_GLOBAL_CACHE_DIR="$(ZIG_GLOBAL_CACHE_DIR)"' "$ROOT/Makefile.cbm"
grep -Fq 'ZIG_LOCAL_CACHE_DIR="$(ZIG_LOCAL_CACHE_DIR)"' "$ROOT/Makefile.cbm"
grep -q 'CBM_PREBUILT_ZIG_DIR' "$ROOT/scripts/build.sh"
grep -q 'export CBM_PREBUILT_ZIG=1' "$ROOT/scripts/build.sh"

BRIDGE="$ROOT/src/zova/cbm_zova_bridge.zig"
grep -q 'extern fn cbm_regcomp' "$BRIDGE"
if grep -q '@cInclude("regex.h")' "$BRIDGE"; then
  echo "error: Zova bridge must use CBM's portable regex abstraction" >&2
  exit 1
fi
grep -q 'bridge_module.addIncludePath(b.path("src"))' "$ROOT/build.zig"
grep -q -- '-Isrc' "$ROOT/Makefile.cbm"

grep -q 'workflow_dispatch:' "$WORKFLOWS/release.yml"
grep -q 'contents: write' "$WORKFLOWS/release.yml"
grep -q 'softprops/action-gh-release@' "$WORKFLOWS/release.yml"
grep -q 'pattern: release-\*' "$WORKFLOWS/release.yml"
grep -q 'checksums.txt' "$WORKFLOWS/release.yml"

grep -q '#define cbm_lstat' "$ROOT/src/foundation/compat.h"
grep -q '#define cbm_mkdir_mode' "$ROOT/src/foundation/compat.h"
for path in \
  "$ROOT/src/store/store.zig" \
  "$ROOT/src/zova/cbm_zova_operations.zig" \
  "$ROOT/src/cli/cli.zig" \
  "$ROOT/src/cli/commands.zig"; do
  if rg -n '\blstat\(' "$path"; then
    echo "error: $path bypasses the Windows-compatible cbm_lstat wrapper" >&2
    exit 1
  fi
done
if rg -n '\bmkdir\([^,]+,' "$ROOT/src/zova/cbm_zova_operations.zig"; then
  echo "error: Zova operations bypasses the Windows-compatible mkdir wrapper" >&2
  exit 1
fi

if rg -n -i \
    'npm publish|twine upload|mcp-publisher|pypi|registry\.npmjs|virus.?total|deusdata|brew tap|deploy-pages|stale@|issue-labeler|label-actions|scorecard' \
    "$WORKFLOWS" "$ROOT/.github/actions"; then
  echo "error: publishing or inherited repository automation remains" >&2
  exit 1
fi

echo "PASS: GitHub Actions are limited to Zova-CBM CI and GitHub release artifacts"
