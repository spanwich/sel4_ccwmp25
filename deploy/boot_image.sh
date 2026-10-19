#!/bin/bash
# Fresh boot of a named seL4 image staged on eMMC partition 5 (see
# stage_ums.sh): NRST reset, stop autoboot, fatload + bootm. Writes nothing.
# A random echo marker proves the U-Boot prompt is live before loading (a
# stale '=>' on the visible screen is otherwise indistinguishable).
# Usage: deploy/boot_image.sh sel4-everparse.bin
set -e
IMG=${1:?usage: $0 IMAGE_NAME_ON_PARTITION_5}
TMUX_SESSION=${TMUX_SESSION:-mp255}
HERE=$(cd "$(dirname "$0")" && pwd)
pane() { tmux capture-pane -p -J -t "$TMUX_SESSION" -S -200; }

for attempt in 1 2 3; do
    "$HERE/reset.sh" >/dev/null
    for _ in $(seq 160); do tmux send-keys -t "$TMUX_SESSION" ' '; sleep 0.05; done
    MARK="mk$RANDOM$RANDOM"
    tmux send-keys -t "$TMUX_SESSION" C-u "echo $MARK" Enter
    ok=
    for _ in $(seq 20); do
        if pane | grep -qx "$MARK *"; then ok=1; break; fi
        sleep 0.1
    done
    [ -n "$ok" ] || { echo "attempt $attempt: U-Boot prompt not live"; continue; }
    tmux send-keys -t "$TMUX_SESSION" C-u \
        "fatload mmc 0:5 0x90000000 $IMG && fatload mmc 0:5 0xb0000000 ccmp25-dvk.dtb && bootm 0x90000000 - 0xb0000000" Enter
    for _ in $(seq 60); do
        if pane | sed -n "/^$MARK *\$/,\$p" | grep -q "Starting kernel"; then
            echo "booted $IMG (attempt $attempt)"
            exit 0
        fi
        sleep 0.25
    done
    echo "attempt $attempt: no 'Starting kernel' after load"
done
exit 1
