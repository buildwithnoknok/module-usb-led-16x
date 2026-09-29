#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# step_leds16.py N - set ONE visual test step on the noknok LEDs 16x and leave it
# showing (the module holds the frame / keeps animating until the next command).
#   python3 step_leds16.py 1 .. 12      (0 = all off)
import glob, sys, time, serial

port = [p for p in glob.glob("/dev/serial/by-id/*") if "noknok_LEDs" in p][0]
s = serial.Serial(port, 115200, timeout=0.5)
def tx(*b): s.write(bytes(b)); s.flush(); time.sleep(0.02)

n = int(sys.argv[1])
BR = 60                                   # ~25 % brightness for the eye test
if n == 0:  tx(0x00)
if n == 1:  tx(0x03, BR); tx(0x11, 255, 0, 0, 0)
if n == 2:  tx(0x03, BR); tx(0x11, 0, 255, 0, 0)
if n == 3:  tx(0x03, BR); tx(0x11, 0, 0, 255, 0)
if n == 4:  tx(0x03, BR); tx(0x11, 0, 0, 0, 255)
if n == 5:  tx(0x03, BR); tx(0x11, 255, 255, 255, 0)
if n == 6:                                # walk: one red LED, 0..15, 1 s each
    tx(0x03, BR); tx(0x00)
    for i in range(16):
        tx(0x12, i, 255, 0, 0, 0); time.sleep(1.0); tx(0x12, i, 0, 0, 0, 0)
if n == 7:
    f = []
    for i in range(16): f += [[255,0,0,0],[0,255,0,0],[0,0,255,0],[0,0,0,255]][i // 4]
    tx(0x03, BR); tx(0x14, *f)
if n == 8:  tx(0x03, BR); tx(0x01, 0, 255, 255)
if n == 9:  tx(0x03, BR); tx(0x20, 1, 30, 0, 0, 0)
if n == 10: tx(0x03, BR); tx(0x21, 2, 20, 0, 0, 0, 255)
if n == 11: tx(0x03, 255); tx(0x11, 255, 255, 255, 255)
if n == 12: tx(0x03, 255); tx(0x11, 0, 0, 0, 255)      # white die alone, full
if n == 13: tx(0x03, BR); tx(0x14, *([0,0,0,255]*8 + [255,255,255,0]*8))  # 0-7 W die, 8-15 RGB mix
if n == 14: tx(0x03, 255); tx(0x11, 0, 0, 0, 10)      # white die only, very dim
if n == 15: tx(0x03, 255); tx(0x11, 0, 10, 0, 0)      # green die only, very dim
if n == 16: tx(0x03, 255); tx(0x14, *([0,0,0,10]*8 + [0,10,0,0]*8))  # 0-7 dim W, 8-15 dim G
print("step", n, "sent")
