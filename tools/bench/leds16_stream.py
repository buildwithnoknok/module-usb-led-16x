#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 noknok (Christopher Houben)
# SPDX-License-Identifier: MIT
#
# leds16_stream.py - DEV-78 fast-host test for the LEDs 16x, run on a Linux PC / Pi with the
# module on a plain USB port (cdc-acm, pyserial). Sends one colour (0x11) once, then ONLY
# 0x03 brightness, sweeping 43 -> 8 -> 43, as fast as the host allows (default 5 ms gap,
# ~200/s, 20x the Pico). Linux may pack several commands into one USB packet - that is
# wanted: it exercises the parser with commands split and joined at any byte.
# Every second it sends 0xB1 GET_VERSION and checks the 4-byte reply: if the parser ever
# fell out of step, the 0xB1 would be swallowed as a value byte and the reply would be
# missing or wrong. A person watches the ring for glitches (one LED off-colour, ring flash).
#
#   python3 leds16_stream.py [--seconds 300] [--gap 0.005] [--serial CDAB...]
# Exit 0 = every version check answered correctly.
import argparse
import glob
import os
import sys
import time

import serial

VID, PID = "1209", "4e4e"
WARM = bytes((0x11, 160, 80, 0, 95))
SWEEP = list(range(43, 7, -1)) + list(range(8, 44))


def find_tty(serial_no=None):
    for tty in sorted(glob.glob("/sys/class/tty/ttyACM*")):
        usbdev = os.path.dirname(os.path.realpath(os.path.join(tty, "device")))
        try:
            v = open(os.path.join(usbdev, "idVendor")).read().strip()
            p = open(os.path.join(usbdev, "idProduct")).read().strip()
            s = open(os.path.join(usbdev, "serial")).read().strip()
        except OSError:
            continue
        if v == VID and p == PID and (serial_no is None or s.upper() == serial_no.upper()):
            return "/dev/" + os.path.basename(tty), s
    return None, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=300)
    ap.add_argument("--gap", type=float, default=0.005)
    ap.add_argument("--serial")
    a = ap.parse_args()
    path, sn = find_tty(a.serial)
    if not path:
        print("FAIL no LEDs 16x (1209:4e4e) on a ttyACM")
        return 1
    port = serial.Serial(path, timeout=0.5)
    port.reset_input_buffer()

    def version():
        port.write(b"\xB1")
        r = port.read(4)
        return tuple(r) if len(r) == 4 else None

    v0 = version()
    print("module", sn, "on", path, "fw", v0)
    port.write(bytes((0x03, 43)) + WARM)
    sent = checks = bad = 0
    t0 = last = time.monotonic()
    i = 0
    while time.monotonic() - t0 < a.seconds:
        port.write(bytes((0x03, SWEEP[i % len(SWEEP)])))
        i += 1
        sent += 1
        if a.gap:
            time.sleep(a.gap)
        if time.monotonic() - last >= 1:
            last = time.monotonic()
            checks += 1
            v = version()
            if v != v0:
                bad += 1
                print("  %.0f s: version check got %s" % (last - t0, v))
            if checks % 60 == 0:
                print("  %.0f s  sent %d (%.0f/s)  version checks %d bad %d" % (last - t0, sent, sent / (last - t0), checks, bad))
    dur = time.monotonic() - t0
    port.write(b"\x00")
    print("sent 0x03 x%d in %.0f s = %.0f/s; version checks %d, bad %d" % (sent, dur, sent / dur, checks, bad))
    print("RESULT", "CLEAN" if bad == 0 else "DESYNC")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
