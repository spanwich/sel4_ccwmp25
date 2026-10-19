#!/bin/bash
# Copy several files to the CCMP25-DVK eMMC boot partition (5, vfat) in one
# U-Boot ums session, then reboot. Only seL4 images/scripts on partition 5
# are written. Usage: deploy/stage_ums.sh SRC:DESTNAME [SRC:DESTNAME ...]
set -e
TMUX_SESSION=${TMUX_SESSION:-mp255}
HERE=$(cd "$(dirname "$0")" && pwd)
console_has() { tmux capture-pane -p -t "$TMUX_SESSION" -S -15 | grep -q "$1"; }

"$HERE/reset.sh"
for _ in $(seq 160); do tmux send-keys -t "$TMUX_SESSION" ' '; sleep 0.05; done
tmux send-keys -t "$TMUX_SESSION" C-u Enter
sleep 0.5
console_has '^=> *$' || { echo "U-Boot prompt not seen"; exit 1; }
tmux send-keys -t "$TMUX_SESSION" C-u "ums 0 mmc 0" Enter
DEV=
for _ in $(seq 30); do
    DEV=$(lsblk -dnpo NAME,MODEL | awk '/UMS disk 0/ {print $1}')
    [ -n "$DEV" ] && break
    sleep 1
done
[ -n "$DEV" ] || { echo "UMS disk did not appear"; exit 1; }
PART=${DEV}5
for _ in $(seq 20); do
    [ -b "$PART" ] && udisksctl info -b "$PART" >/dev/null 2>&1 && break
    sleep 0.5
done
sleep 2
MNT=$(lsblk -no MOUNTPOINT "$PART")
[ -n "$MNT" ] || { udisksctl mount -b "$PART" >/dev/null; MNT=$(lsblk -no MOUNTPOINT "$PART"); }
for pair in "$@"; do
    cp "${pair%%:*}" "$MNT/${pair##*:}"
done
sync
for pair in "$@"; do
    cmp "${pair%%:*}" "$MNT/${pair##*:}"
    echo "staged ${pair##*:} $(sha256sum "${pair%%:*}" | cut -c1-16)"
done
for p in $(lsblk -lnpo NAME,MOUNTPOINT "$DEV" | awk 'NF==2 {print $1}'); do
    udisksctl unmount -b "$p" >/dev/null
done
tmux send-keys -t "$TMUX_SESSION" C-c
sleep 1
"$HERE/reset.sh"
