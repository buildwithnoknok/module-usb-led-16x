#!/bin/sh
# SPDX-FileCopyrightText: 2026 noknok (Christopher Houben)
# SPDX-License-Identifier: MIT
#
# heat_run.sh - full power on the LEDs 16x, then log the governor for 20 min (background).
cd "$(dirname "$0")"
python3 step_leds16.py 11
nohup python3 status_leds16.py log 1200 5 ${1:-$HOME/heat_open_air.csv} > /tmp/heat_run.out 2>&1 &
echo started
