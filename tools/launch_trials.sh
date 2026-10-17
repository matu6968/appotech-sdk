#!/bin/bash
# Repeatedly: SPI-booted SDK -> `debug bootrom` -> `sdk launch MANIFEST` from the ROM loader.
# A trial succeeds only if the SDK that answers has the manifest's build id AND reports
# boot_source "usb" (the SPI-booted image answering after a crashed launch reports "spi").
# After each trial the camera is handed back to the loader and left to idle-reset into its
# SPI image, so no power cycle is needed. Usage: launch_trials.sh MANIFEST [TRIALS]
# With RING_DIR set, a crashed launch's SRAM ring (0x2800) is saved there as ring-N.bin; decode with tools/decode_ring.py.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export PYTHONPATH="$ROOT/apxtool/src:$ROOT/jl-uboot-tool"
MANIFEST="$1"; TRIALS="${2:-5}"
APX=(python3 -m apxtool)
want=$(grep -o '"build_id": "[0-9a-f]*"' "$MANIFEST" | cut -d'"' -f4)

port() { for d in /sys/bus/usb/devices/*/idVendor; do [ "$(cat "$d")" = 1908 ] && basename "$(dirname "$d")"; done; }
loader() {
  for _ in $(seq 1 80); do
    for s in /sys/class/scsi_generic/sg*; do
      case "$(cat "$s/device/model" 2>/dev/null)" in *Video050Loader*) echo "/dev/$(basename "$s")"; return;; esac
    done; sleep 0.1
  done
}
field() { "${APX[@]}" debug --port "$1" info 2>/dev/null | grep -o "\"$2\": \"[^\"]*\"" | cut -d'"' -f4; }
# Wait until an SDK answers; echo its port.
wait_sdk() {
  for _ in $(seq 1 "${1:-60}"); do
    p=$(port); [ -n "$p" ] && [ -n "$(field "$p" build_id)" ] && { echo "$p"; return; }
    sleep 2
  done
}

ok=0; crash=0; hang=0; fail=0
for n in $(seq 1 "$TRIALS"); do
  p=$(wait_sdk)
  if [ -z "$p" ]; then echo "trial $n: camera did not come back (needs a power cycle)"; exit 3; fi
  start=$(field "$p" boot_source)
  if [ "$start" != spi ]; then
    "${APX[@]}" debug --port "$p" bootrom >/dev/null 2>&1; sleep 14; p=$(wait_sdk)
  fi
  if ! "${APX[@]}" debug --port "$p" bootrom >/dev/null 2>&1; then echo "trial $n: debug bootrom failed"; fail=$((fail+1)); continue; fi
  sg=$(loader)
  "${APX[@]}" --device "$sg" sdk launch "$MANIFEST" --reclaim-jpeg-ram >/dev/null 2>&1
  p=$(wait_sdk 25)
  if [ -z "$p" ]; then
    echo "trial $n: HANG (nothing answers on USB; needs a power cycle)"; hang=$((hang+1))
    echo "manifest $want: $ok ok / $crash crashed-to-SPI / $hang hung, stopped after $n of $TRIALS"; exit 3
  fi
  got=$(field "$p" build_id); src=$(field "$p" boot_source)
  if [ "$got" = "$want" ] && [ "$src" = usb ]; then
    ok=$((ok+1)); echo "trial $n: OK (usb launch running)"
    [ -n "${RING_DIR:-}" ] && "${APX[@]}" debug --port "$p" read 0x2800 0x650 --output "$RING_DIR/ring-ok-$n.bin" >/dev/null 2>&1
  else
    crash=$((crash+1)); echo "trial $n: CRASHED (SPI image answering: build=$got source=$src)"
    [ -n "${RING_DIR:-}" ] && "${APX[@]}" debug --port "$p" read 0x2800 0x650 --output "$RING_DIR/ring-$n.bin" >/dev/null 2>&1
  fi
  "${APX[@]}" debug --port "$p" bootrom >/dev/null 2>&1; sleep 14
done
echo "manifest $want: $ok ok / $crash crashed-to-SPI / $fail other failures of $TRIALS"
