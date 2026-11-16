#!/bin/sh
# Installs btkbd. Run from the repo with: sudo make install
#  - /usr/local/btkbd/btkbd          root LaunchDaemon: owns the USB dongle, starts at boot, restarts if it stops
#  - /usr/local/btkbd/btkbd-inject   LaunchAgent in the login window AND in each user session: types the keys
set -e
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
[ -x build/btkbd ] && [ -x build/btkbd-inject ] || { echo "build first: make"; exit 1; }
[ -s fw/rtl8761bu_fw.bin ] || sh scripts/get-firmware.sh

D=/Library/LaunchDaemons/local.btkbd.plist
A=/Library/LaunchAgents/local.btkbd-inject.plist
U=$(stat -f %u /dev/console)

launchctl unload "$D" 2>/dev/null || true
mkdir -p /usr/local/btkbd
install -m 755 -o root -g wheel build/btkbd build/btkbd-inject /usr/local/btkbd/
install -m 644 -o root -g wheel fw/rtl8761bu_fw.bin fw/rtl8761bu_config.bin /usr/local/btkbd/
install -m 644 -o root -g wheel launchd/local.btkbd.plist "$D"
install -m 644 -o root -g wheel launchd/local.btkbd-inject.plist "$A"

# (re)start the key helper in the current user's session; at the login window launchd starts it by itself
if [ "$U" != 0 ]; then
  launchctl bootout gui/$U "$A" 2>/dev/null || true
  launchctl bootstrap gui/$U "$A"
fi

if [ -f /usr/local/btkbd/bond.txt ]; then
  launchctl load -w "$D"
  echo "installed and running. Press a key on the keyboard to wake it."
else
  echo "installed. Now pair: put the keyboard in pairing mode and run"
  echo "    sudo /usr/local/btkbd/btkbd pair"
  echo "then: sudo launchctl load -w $D"
fi
