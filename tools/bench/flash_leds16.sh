#!/bin/sh
# SPDX-License-Identifier: MIT
# flash_leds16.sh - blank-board bring-up of a noknok LEDs 16x over SWD (WCH-LinkE).
# Writes bootloader+app+metadata combined image at 0x08000000, reads it back,
# compares byte-for-byte, then reboots. Board powered by its own USB-C.
# NOTE: do NOT pass -t (target power off) - minichlink segfaults on the CH32V203 write.
# Leave the LinkE's 3V3 pad unconnected/unused so the two 3V3 sources never fight.
set -e
cd "$(dirname "$0")"
IMG=${1:-noknok_leds16_combined.bin}
ML=/home/noknok/dev/ch32fun/minichlink/minichlink
LEN=$(stat -c %s "$IMG")
echo "== chip info"; $ML -i 2>&1 | tail -6
echo "== write $IMG ($LEN B)"; $ML -w "$IMG" 0x08000000 2>&1 | tail -3
echo "== read back"; : > /tmp/rb_leds16.bin; $ML -r /tmp/rb_leds16.bin 0x08000000 $LEN 2>&1 | tail -2
if cmp -s "$IMG" /tmp/rb_leds16.bin; then echo "VERIFY OK"; else echo "VERIFY FAILED"; cmp "$IMG" /tmp/rb_leds16.bin | head -3; exit 1; fi
echo "== reboot"; $ML -b 2>&1 | tail -1
