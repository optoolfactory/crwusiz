#!/usr/bin/bash

cd /data/openpilot && scons -c;
rm /data/openpilot/.sconsign.dblite;
rm /data/openpilot/prebuilt;
rm -rf /tmp/scons_cache;

# Allows you to restart OpenPilot without rebooting the Comma 3
tmux kill-session -t comma;
rm -f /tmp/safe_staging_overlay.lock;
tmux new -s comma -d "/data/openpilot/launch_openpilot.sh"