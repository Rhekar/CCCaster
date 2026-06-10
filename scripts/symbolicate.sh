#!/usr/bin/env bash
#
# Resolve Sentry/Glitchtip crash-frame addresses to function + source:line for a CCCaster build.
#
# A native crash report from lib/SentryClient carries, per frame, an absolute `instruction_addr`
# and the owning module's runtime `image_addr`. Because of ASLR the runtime load address differs
# from the binary's linked ImageBase, so we map each frame back to a file virtual address:
#
#     file_VA = <ImageBase from the binary>  +  (instruction_addr - image_addr)
#
# and feed that to addr2line. This only yields source lines for an UNSTRIPPED build (debug, or
# logging — release is stripped with -s, so keep a copy of the unstripped binary if you need this).
#
# Usage:
#   scripts/symbolicate.sh <binary> <image_addr> <instruction_addr> [<instruction_addr> ...]
#
#     binary            cccaster/hook.dll (the SAME build that crashed) or cccaster.v*.exe
#     image_addr        the frame's image_addr (module runtime base) from the event
#     instruction_addr  one or more frame instruction_addr values from the event
#
# Example (fault + a couple of caller frames, all in hook.dll):
#   scripts/symbolicate.sh cccaster/hook.dll 0x72900000 0x72901dca 0x72903f51
#
# MINGW32_BIN may point at the 32-bit binutils dir (default: /c/msys64/mingw32/bin).

set -euo pipefail

if [ "$#" -lt 3 ]; then
    sed -n '2,30p' "$0"
    exit 2
fi

BIN="$1"; IMAGE_ADDR="$2"; shift 2

MINGW32_BIN="${MINGW32_BIN:-/c/msys64/mingw32/bin}"
ADDR2LINE="$MINGW32_BIN/addr2line"
OBJDUMP="$MINGW32_BIN/objdump"
# Use the 32-bit binutils from MINGW32_BIN; only fall back to PATH if they're genuinely absent.
# A 64-bit addr2line/objdump on PATH silently returns "??" for a 32-bit DWARF binary, so the
# (extensionless) full path must be preferred — `command -v` on it fails for the .exe, so test
# the file directly instead.
[ -x "$ADDR2LINE" ] || [ -x "$ADDR2LINE.exe" ] || ADDR2LINE="addr2line"
[ -x "$OBJDUMP" ]   || [ -x "$OBJDUMP.exe" ]   || OBJDUMP="objdump"

[ -f "$BIN" ] || { echo "ERROR: binary not found: $BIN" >&2; exit 1; }

# Linked ImageBase from the PE header (printed in hex without a 0x prefix).
IMAGE_BASE=$("$OBJDUMP" -p "$BIN" | awk '/ImageBase/{print "0x"$2}')
[ -n "$IMAGE_BASE" ] || { echo "ERROR: could not read ImageBase from $BIN" >&2; exit 1; }

if ! "$OBJDUMP" -h "$BIN" | grep -q '\.debug_info'; then
    echo "WARNING: $BIN has no .debug_info (stripped?) — addresses won't resolve to source lines." >&2
fi

echo "binary     : $BIN"
echo "ImageBase  : $IMAGE_BASE"
echo "image_addr : $IMAGE_ADDR"
echo

for INSTR in "$@"; do
    RVA=$(( INSTR - IMAGE_ADDR ))
    FILE_VA=$(( IMAGE_BASE + RVA ))
    printf '%s  (rva=0x%x)\n' "$INSTR" "$RVA"
    "$ADDR2LINE" -f -C -i -e "$BIN" "$(printf '0x%x' "$FILE_VA")" | sed 's/^/    /'
    echo
done
