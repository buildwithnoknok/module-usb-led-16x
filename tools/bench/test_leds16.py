#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# test_leds16.py - bench test for the noknok LEDs 16x (fw 2.0.0) straight from the Pi4
# over the module's USB serial port (no Pico needed).
#   python3 test_leds16.py            -> identity + version + visual sequence
#   python3 test_leds16.py off        -> all off
# Each visual step holds for a few seconds so it can be checked by eye.
import glob, sys, time, serial

ports = [p for p in glob.glob("/dev/serial/by-id/*") if "WCH" not in p and "Pico" not in p]
if not ports:
    sys.exit("No LED module found in /dev/serial/by-id (is it plugged into the Pi?)")
print("port:", ports[0])
s = serial.Serial(ports[0], 115200, timeout=0.5)

def tx(*b):
    s.write(bytes(b)); s.flush(); time.sleep(0.02)

def ask(cmd, n):
    s.reset_input_buffer(); s.write(bytes([cmd])); s.flush()
    return list(s.read(n))

print("identity 0xF0 ->", [hex(x) for x in ask(0xF0, 3)], "(expect 4e 4e 06 = LEDs 16x, fw >= 2.2.0)")
v = ask(0xB1, 4)
print("version  0xB1 ->", v, "(protocol, major, minor, patch)")

if len(sys.argv) > 1 and sys.argv[1] == "off":
    tx(0x00); sys.exit(0)

def step(label, *cmd, hold=3):
    print(" ", label); tx(*cmd); time.sleep(hold)

tx(0x03, 60)                                   # global brightness ~25% for the eye test
step("all RED",              0x11, 255, 0, 0, 0)
step("all GREEN",            0x11, 0, 255, 0, 0)
step("all BLUE",             0x11, 0, 0, 255, 0)
step("all WHITE die only",   0x11, 0, 0, 0, 255)
step("all RGB white (mix)",  0x11, 255, 255, 255, 0)
tx(0x00)
print("  one LED at a time, red -> walks 0..15 (check order + no skipped LED)")
for i in range(16):
    tx(0x12, i, 255, 0, 0, 0); time.sleep(0.25); tx(0x12, i, 0, 0, 0, 0)
frame = []
for i in range(16):                            # 0x14: per-LED RGBW frame, 4 colour groups
    frame += [[255,0,0,0],[0,255,0,0],[0,0,255,0],[0,0,0,255]][i // 4]
step("0x14 frame: 4 red, 4 green, 4 blue, 4 white", 0x14, *frame)
step("legacy 0x01 RGB (cyan)", 0x01, 0, 255, 255)
step("rainbow preset",       0x20, 1, 30, 0, 0, 0, hold=5)
step("breathe warm white",   0x21, 2, 20, 0, 0, 0, 255, hold=5)
tx(0x03, 255)
step("current cap: ALL channels full (should be capped ~450 mA)", 0x11, 255, 255, 255, 255, hold=4)
tx(0x00)
print("done - LEDs off (LED rail switched off)")
