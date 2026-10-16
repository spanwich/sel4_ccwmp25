#!/bin/bash
# Reset the CCMP25-DVK by pulsing NRST (J6 pin 2) via the Nucleo-F767ZI.
# Firmware: tools/nucleo_reset/. Watch the console on /dev/ttyACM0.
set -e

NUCLEO=${NUCLEO_TTY:-/dev/serial/by-id/usb-STMicroelectronics_STM32_STLink_066FFF555567894967085545-if02}

python3 - "$NUCLEO" <<'EOF'
import sys, time, serial
s = serial.Serial(sys.argv[1], 115200, timeout=1)
time.sleep(0.2)
s.reset_input_buffer()
s.write(b'r')
reply = s.read(16)
if b'RESET' not in reply:
    sys.exit(f"no RESET ack from Nucleo (got {reply!r})")
print("CCMP25 reset pulsed")
EOF
