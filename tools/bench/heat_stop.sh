#!/bin/sh
# SPDX-FileCopyrightText: 2026 noknok (Christopher Houben)
# SPDX-License-Identifier: MIT
#
# heat_stop.sh - stop the background governor logger and switch the LEDs off.
pkill -f "status_leds16.py log" && echo "logger stopped" || echo "no logger running"
sleep 1
cd /home/noknok/dev/ch32fun/noknok_leds16 && python3 step_leds16.py 0
