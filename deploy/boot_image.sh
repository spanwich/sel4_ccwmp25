#!/bin/bash
# Fresh boot of a named seL4 image staged on eMMC partition 5 (see
# stage_ums.sh): NRST reset, stop autoboot, fatload + bootm. Writes nothing.
# Usage: deploy/boot_image.sh sel4-everparse.bin
set -e
IMG=${1:?usage: $0 IMAGE_NAME_ON_PARTITION_5}
TMUX_SESSION=${TMUX_SESSION:-mp255}
HERE=$(cd "$(dirname "$0")" && pwd)
console_has() { tmux capture-pane -p -t "$TMUX_SESSION" -S -15 | grep -q "$1"; }

for attempt in 1 2 3; do
    "$HERE/reset.sh" >/dev/null
    for _ in $(seq 160); do tmux send-keys -t "$TMUX_SESSION" ' '; sleep 0.05; done
    tmux send-keys -t "$TMUX_SESSION" C-u Enter
    sleep 0.5
    console_has '^=> *$' || { echo "attempt $attempt: U-Boot prompt not seen"; continue; }
    tmux clear-history -t "$TMUX_SESSION"
    tmux send-keys -t "$TMUX_SESSION" C-u \
        "fatload mmc 0:5 0x90000000 $IMG && fatload mmc 0:5 0xb0000000 ccmp25-dvk.dtb && bootm 0x90000000 - 0xb0000000" Enter
    for _ in $(seq 40); do
        if tmux capture-pane -p -t "$TMUX_SESSION" -S -80 | grep -q "Starting kernel"; then
            echo "booted $IMG (attempt $attempt)"
            exit 0
        fi
        sleep 0.25
    done
    echo "attempt $attempt: no 'Starting kernel'"
done
exit 1
