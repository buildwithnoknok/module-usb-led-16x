#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# stress_leds16.py - USB receive-path stress test for the noknok LEDs 16x.
# Blasts N back-to-back 65-byte 0x14 RGBW frames (each crosses a 64-byte USB
# packet boundary) with NO pauses, then checks the parser is still in sync by
# asking for the version. A lost/misparsed byte shows up as a wrong or missing
# reply (and usually as dark LEDs, because stray 0x00 bytes mean ALL OFF).
# Leaves the last frame showing: a slow-rotating dim rainbow-ish pattern.
import glob, sys, time, serial

N = int(sys.argv[1]) if len(sys.argv) > 1 else 300
port = [p for p in glob.glob("/dev/serial/by-id/*") if "noknok_LEDs" in p][0]
s = serial.Serial(port, 115200, timeout=1.0)
s.write(bytes([0x03, 60])); s.flush(); time.sleep(0.05)

cols = [[255,0,0,0],[0,255,0,0],[0,0,255,0],[0,0,0,255]]
blob = bytearray()
for k in range(N):
    frame = bytearray([0x14])
    for i in range(16):
        frame += bytes(cols[(i // 4 + k) % 4])      # 4-LED colour blocks, shifted per frame
    blob += frame
t0 = time.time(); s.write(blob); s.flush(); dt = time.time() - t0
time.sleep(0.2)
s.reset_input_buffer(); s.write(bytes([0xB1])); s.flush()
v = list(s.read(4))
print("sent %d frames (%d bytes) in %.2f s" % (N, len(blob), dt))
print("version after blast:", v)
last = (N - 1) % 4
print("last frame: LEDs 0-3 =", ["red","green","blue","white"][last],
      "then the next colours in order red->green->blue->white")
print("PASS" if len(v) == 4 and v[0] == 1 and v[1] == 2 else "FAIL - parser out of sync / reply lost")
