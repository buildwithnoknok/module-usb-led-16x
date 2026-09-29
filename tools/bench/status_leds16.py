#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# status_leds16.py - read the LEDs 16x power governor (fw >= 2.1.0) from the Pi4.
#   python3 status_leds16.py                  -> one GET_STATUS (0x30) + GET_SENSOR_RAW (0x3F)
#   python3 status_leds16.py log 1200 5 f.csv -> log every 5 s for 1200 s into f.csv
#                                                (heat-up curve; prints each line too)
import glob, struct, sys, time, serial

port = [p for p in glob.glob("/dev/serial/by-id/*") if "noknok_LEDs" in p][0]
s = serial.Serial(port, 115200, timeout=0.5)

def ask(cmd, n):
    s.reset_input_buffer(); s.write(bytes([cmd])); s.flush()
    r = s.read(n)
    if len(r) != n:
        raise RuntimeError("0x%02X: expected %d bytes, got %d" % (cmd, n, len(r)))
    return r

def status():
    r = ask(0x30, 16)
    ver, t, vbus, cc1, cc2, budget, est = struct.unpack_from("<BhHHHHH", r, 0)
    flags, therm, vfac = r[13], r[14], r[15]
    return dict(temp_c=t / 10.0, vbus_mv=vbus, cc1_mv=cc1, cc2_mv=cc2, budget_ma=budget,
                est_ma=est, rail=bool(flags & 1), therm_cut=bool(flags & 2),
                vbus_cut=bool(flags & 4), factory_cal=bool(flags & 8),
                therm_pct=therm, vbus_pct=vfac)

def raw():
    r = ask(0x3F, 18)
    cal = struct.unpack_from("<I", r, 0)[0]
    vdda, ts, vref, vb, c1, c2, refmv = struct.unpack_from("<7H", r, 4)
    return dict(cal_word="0x%08X" % cal, cal_mv=cal & 0xFFFF, cal_c=cal >> 16, vdda_mv=vdda,
                raw_ts=ts, raw_vref=vref, raw_vbus=vb, raw_cc1=c1, raw_cc2=c2, ts_ref_mv_used=refmv)

if len(sys.argv) > 1 and sys.argv[1] == "log":
    dur, step = float(sys.argv[2]), float(sys.argv[3])
    out = open(sys.argv[4], "w")
    out.write("t_s,temp_c,vbus_mv,budget_ma,est_ma,therm_pct,vbus_pct,rail,therm_cut\n")
    t0 = time.time()
    while time.time() - t0 < dur:
        st = status()
        line = "%.0f,%.1f,%d,%d,%d,%d,%d,%d,%d" % (time.time() - t0, st["temp_c"], st["vbus_mv"],
               st["budget_ma"], st["est_ma"], st["therm_pct"], st["vbus_pct"], st["rail"], st["therm_cut"])
        out.write(line + "\n"); out.flush(); print(line, flush=True)
        time.sleep(step)
else:
    print("version", list(ask(0xB1, 4)))
    for k, v in status().items(): print("  %-12s %s" % (k, v))
    print("raw:")
    for k, v in raw().items(): print("  %-12s %s" % (k, v))
