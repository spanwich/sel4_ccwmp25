#!/bin/bash
# RAM-only boot of a Microkit raw image via fatload + go 0x88000000.
set -e
IMG=${1:?usage: $0 IMAGE_NAME_ON_PART5}
S=mp255
HERE=/home/iamfo470/phd/camkes-vm-examples/projects/sel4_ccwmp25/deploy
pane() { tmux capture-pane -p -J -t "$S" -S -300; }
"$HERE/reset.sh" >/dev/null
for _ in $(seq 160); do tmux send-keys -t "$S" ' '; sleep 0.05; done
MARK="mk$RANDOM$RANDOM"
tmux send-keys -t "$S" C-u "echo $MARK" Enter
ok=
for _ in $(seq 20); do pane | grep -qx "$MARK *" && { ok=1; break; }; sleep 0.1; done
[ -n "$ok" ] || { echo "PROMPT-NOT-LIVE"; exit 1; }
tmux send-keys -t "$S" C-u "fatload mmc 0:5 0x88000000 $IMG && go 0x88000000" Enter
echo "boot command sent for $IMG"
