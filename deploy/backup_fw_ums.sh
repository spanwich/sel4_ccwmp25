#!/bin/bash
# READ-ONLY firmware inventory of the CCMP25-DVK eMMC from U-Boot.
# Writes nothing to the board. Records the partition table and FWU metadata,
# and CRC32s of TF-A (boot1/boot2) and the FIP (fip-a/fip-b), to compare
# against a known-good image set (crc32 of a host file: python3 zlib.crc32).
# (A raw dd over ums needs root on this host, so U-Boot does the hashing.)
# Usage: deploy/backup_fw_ums.sh OUTDIR [TFA_BYTES_HEX FIP_BYTES_HEX]
set -e

OUT=${1:?usage: $0 OUTDIR}
TMUX_SESSION=${TMUX_SESSION:-mp255}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT"

console_has() { tmux capture-pane -p -t "$TMUX_SESSION" -S -15 | grep -q "$1"; }
ucmd() { # run a U-Boot command and append its output to $OUT/uboot.log
    tmux clear-history -t "$TMUX_SESSION"
    tmux send-keys -t "$TMUX_SESSION" C-u "$1" Enter
    sleep "${2:-1}"
    { echo "=> $1"; tmux capture-pane -p -t "$TMUX_SESSION" -S -200 | sed '/^$/d'; } >> "$OUT/uboot.log"
}

echo "[1/4] reset + stop autoboot"
"$HERE/reset.sh"
for _ in $(seq 160); do
    tmux send-keys -t "$TMUX_SESSION" ' '
    sleep 0.05
done
tmux send-keys -t "$TMUX_SESSION" C-u Enter
sleep 0.5
console_has '^=> *$' || { echo "U-Boot prompt not seen (check J6 wiring?)"; exit 1; }

echo "[2/4] U-Boot inventory (read-only commands)"
: > "$OUT/uboot.log"
ucmd "version"
ucmd "mmc dev 0"
ucmd "mmc info"
ucmd "part list mmc 0"
ucmd "fwu_mdata_read" 2
ucmd "printenv boot_targets bootcmd forced_update"

echo "[3/4] CRC32 of boot1/boot2 (TF-A) and fip-a/fip-b"
TFA=${2:-0x2ec01}   # tf-a-ccmp25-dvk-optee-emmc.stm32 (DEY 5.0-r3)
FIP=${3:-0x2c7490}  # fip-ccmp25-dvk-optee-emmc.bin (DEY 5.0-r3)
blocks() { printf '0x%x' $(( ($1 + 511) / 512 )); }
for hw in 1 2; do
    ucmd "mmc dev 0 $hw"
    ucmd "mmc read 0x90000000 0 $(blocks $TFA)"
    ucmd "crc32 0x90000000 $TFA"
done
ucmd "mmc dev 0 0"
ucmd "mmc read 0x90000000 0x1800 $(blocks $FIP)"   # fip-a
ucmd "crc32 0x90000000 $FIP"
ucmd "mmc read 0x90000000 0x3000 $(blocks $FIP)"   # fip-b
ucmd "crc32 0x90000000 $FIP"
grep "==>" "$OUT/uboot.log" | sort -u

echo "[4/4] reboot"
"$HERE/reset.sh"
