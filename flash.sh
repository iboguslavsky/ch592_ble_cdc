#!/usr/bin/env bash
set -e
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
BIN="$DIR/build/ch592_ble_cdc.bin"
[ -f "$BIN" ] || { echo "Building first..."; make -C "$DIR"; }
echo "Put CH592 into ISP mode (bridge Pad 2 [BOOT/PB22] to Pad 1 [GND] while connecting USB), then flashing..."
wchisp flash -V "$BIN"
echo "Done! Disconnect BOOT bridge, and open terminal (e.g. picocom -b 115200 /dev/ttyACM0)."
