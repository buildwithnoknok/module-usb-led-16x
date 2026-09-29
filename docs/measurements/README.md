# Bench measurements — noknok LEDs 16x (rev 2.0)

Logged from the Pi4 bench with `tools/bench/status_leds16.py log <seconds> <step> <file>`
(firmware `0x30 GET_STATUS`, every 5 s). Columns:
`t_s, temp_c (chip die, 0.1 °C), vbus_mv, budget_ma, est_ma, therm_pct, vbus_pct, rail, therm_cut`.

Board powered from a Pi4 USB-A port through a KWS-X1 meter (standard USB, CC2 ≈ 0.60 V →
450 mA LED budget). Room temperature ≈ 22–25 °C.

| File | What | Result |
|---|---|---|
| `2026-09-29_heat_open_air_fw2.1.0.csv` | All LEDs, all channels at 100 % requested (capped at 450 mA ≈ 1.8 W total), board **LED side up, in open air**, 11.5 min | 27.7 → 57 °C in ~7 min, then creeping +0.3 °C/min (58.4 °C at 11.5 min). fw 2.1.0 had a bug: the derating budget was computed but never applied to a static frame (`est_ma` stays 449) — so this is the **undimmed** curve. |
| `2026-09-29_thermal_cut_resume_testbuild.csv` | TEST build with low thresholds (dim 35→45 °C, cut 50 °C, resume < 42 °C) + the 2.1.1 fix, started hot | Starts at 54.7 °C → LED rail **off**; cools; **back on by itself at 41.4 °C** at 52 %; then dims step by step as it warms and holds ~43 °C at ~190 mA. Whole chain derate → cut → resume verified. |

Earlier observation (not logged): the same full-power setting with the board **LED side
down on cardboard** got "really hot" to the touch within some minutes — a closed housing
will behave in between. **Next measurement: inside the real 3D-printed housing.** The
thermal thresholds (55 / 70 / 75 °C) are set from that.
