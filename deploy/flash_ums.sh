#!/bin/bash
# Flash sel4.bin to the CCMP25-DVK eMMC (partition 5) over USB-C via U-Boot ums,
# then reboot into it. Console must be open in tmux session $TMUX_SESSION.
# Usage: deploy/flash_ums.sh path/to/sel4.bin
set -e

IMAGE=${1:?usage: $0 path/to/sel4.bin}
TMUX_SESSION=${TMUX_SESSION:-mp255}
HERE=$(cd "$(dirname "$0")" && pwd)

console_has() { tmux capture-pane -p -t "$TMUX_SESSION" -S -15 | grep -q "$1"; }

echo "[1/5] reset + stop autoboot"
"$HERE/reset.sh"
for _ in $(seq 160); do
    tmux send-keys -t "$TMUX_SESSION" ' '
    sleep 0.05
done
tmux send-keys -t "$TMUX_SESSION" C-u Enter
sleep 0.5
console_has '^=> *$' || { echo "U-Boot prompt not seen (check J6 wiring?)"; exit 1; }

echo "[2/5] ums 0 mmc 0"
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
sleep 2 # let the desktop automount settle

echo "[3/5] copy $IMAGE -> $PART"
MNT=$(lsblk -no MOUNTPOINT "$PART")
if [ -z "$MNT" ]; then
    udisksctl mount -b "$PART" >/dev/null
    MNT=$(lsblk -no MOUNTPOINT "$PART")
fi
cp "$IMAGE" "$MNT/sel4.bin"
sync
cmp "$IMAGE" "$MNT/sel4.bin"

echo "[4/5] unmount + end ums"
for p in $(lsblk -lnpo NAME,MOUNTPOINT "$DEV" | awk 'NF==2 {print $1}'); do
    udisksctl unmount -b "$p" >/dev/null
done
tmux send-keys -t "$TMUX_SESSION" C-c

echo "[5/5] reboot"
sleep 1
"$HERE/reset.sh"
