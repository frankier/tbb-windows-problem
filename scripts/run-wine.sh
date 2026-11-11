#!/usr/bin/env bash
# Run every scenario under Wine and report what happened. Crashes are expected,
# so this script does not use "set -e".
#
#   scripts/run-wine.sh [dist-directory]
#
# Environment: WINEPREFIX (optional), WINE (default: wine).

set -uo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
DIST=${1:-"$ROOT/build-wine/dist"}
WINE=${WINE:-wine}

if [[ ! -f "$DIST/host.exe" ]]; then
    echo "error: $DIST/host.exe not found; run scripts/build-wine.sh first" >&2
    exit 1
fi

# Do not open Wine's crash dialog for the expected crashes.
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:+$WINEDLLOVERRIDES;}winedbg.exe=d"

run() {
    local out rc
    printf '\n==== host.exe %s ====\n' "$*"
    # Pointer and register noise from a Wine crash is not useful here.
    out=$("$WINE" "$DIST/host.exe" "$@" 2>/dev/null)
    rc=$?
    printf '%s\n' "$out"
    if grep -q 'trigger thread: calling into the unloaded module' <<<"$out" &&
       ! grep -q 'scenario finished' <<<"$out"; then
        # Wine reports the Windows exit code modulo 256, so 0xC0000005 is 5.
        printf '[result] access violation 0xC0000005\n'
    elif [[ $rc -eq 0 ]]; then
        printf '[result] exited normally\n'
    else
        printf '[result] exit status %s\n' "$rc"
    fi
}

run close
run close --exit
run finalize
run finalize --exit

printf '\nExpected against oneTBB f1e7e5c: only "finalize" crashes.\n'
printf 'Against the fixed revision 30337af every scenario exits normally.\n'
printf 'See README.md for other revisions and for the --control diagnostic.\n'
