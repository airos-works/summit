#!/bin/sh
# noblank.sh: keep the screen blanker away while tests run (input_server restarts it when idle).
while true; do kill $(ps | grep screen_blanker | grep -v grep | awk '{print $(NF-3)}') 2>/dev/null; sleep 3; done
