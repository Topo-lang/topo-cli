#!/bin/sh
# run-init-smoke.sh — end-to-end scaffold smoke for topo-init.
#
# Per language: copy the fixture host project under test/smoke/fixtures/
# to a scratch directory, run topo-init against it, then assert the
# generated project is check-clean (topo-check exit 0) and buildable
# (topo-build exit 0) when the language's backend tool is on PATH.
#
# topo-init / topo-check / topo-build and the per-language backend tools
# are resolved from PATH. Environment knobs:
#   TOPO_INIT_SMOKE_CHECK_ONLY=1     skip the build half entirely
#   TOPO_INIT_SMOKE_REQUIRE_BUILD=1  a missing backend tool fails the run
#                                    (default: loud check-only fallback)
#   TOPO_INIT_SMOKE_WORKDIR=<dir>    scratch root (default: ${TMPDIR:-/tmp})
#
# The scratch project directory is named <lang>smoke — for rust this must
# stay equal to the fixture Cargo.toml package name (the wrapper
# namespace topo-init derives from the directory has to match the
# crate's symbol-form name).
#
# Usage: run-init-smoke.sh [lang ...]   (default: all five)

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
fixtures="${script_dir}/fixtures"

langs="${*:-cpp rust java python typescript}"
work="${TOPO_INIT_SMOKE_WORKDIR:-${TMPDIR:-/tmp}}/topo-init-smoke-$$"

backend_tool() {
    case "$1" in
    cpp) echo topo-build-llvm-cpp ;;
    rust) echo topo-build-llvm-rust ;;
    java) echo topo-build-jvm-java ;;
    python) echo topo-build-python ;;
    typescript) echo topo-build-typescript ;;
    esac
}

fail=0
build_skipped=""

for lang in $langs; do
    case "$lang" in
    cpp | rust | java | python | typescript) ;;
    *)
        echo "FAIL: unknown language '${lang}'"
        fail=1
        continue
        ;;
    esac

    proj="${work}/${lang}smoke"
    rm -rf "$proj"
    mkdir -p "$proj"
    cp -R "${fixtures}/${lang}/." "$proj"

    echo "=== ${lang}: topo-init ==="
    if ! topo-init --project "$proj" --language "$lang"; then
        echo "FAIL: ${lang} — topo-init exited non-zero"
        fail=1
        continue
    fi

    echo "=== ${lang}: topo-check ==="
    if ! topo-check --project "$proj"; then
        echo "FAIL: ${lang} — generated project is not check-clean"
        fail=1
        continue
    fi

    if [ "${TOPO_INIT_SMOKE_CHECK_ONLY:-0}" = "1" ]; then
        echo "${lang}: check-only mode — build half skipped by request"
        continue
    fi

    tool=$(backend_tool "$lang")
    if ! command -v "$tool" >/dev/null 2>&1; then
        if [ "${TOPO_INIT_SMOKE_REQUIRE_BUILD:-0}" = "1" ]; then
            echo "FAIL: ${lang} — backend tool '${tool}' not on PATH and a build was required"
            fail=1
        else
            echo "BUILD SKIPPED: ${lang} — backend tool '${tool}' not on PATH;"
            echo "  this language was verified CHECK-ONLY, which is NOT full verification"
            build_skipped="${build_skipped} ${lang}"
        fi
        continue
    fi

    echo "=== ${lang}: topo-build ==="
    if ! (cd "$proj" && topo-build); then
        echo "FAIL: ${lang} — generated project does not build"
        fail=1
        continue
    fi
done

rm -rf "$work"

if [ -n "$build_skipped" ]; then
    echo "WARNING: build half skipped for:${build_skipped}"
fi
if [ "$fail" -ne 0 ]; then
    echo "init smoke FAILED"
    exit 1
fi
echo "init smoke OK: ${langs}"
