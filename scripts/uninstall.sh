#!/bin/sh
# Removes btkbd completely (including the saved pairing). Run with sudo.
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
D=/Library/LaunchDaemons/local.btkbd.plist
A=/Library/LaunchAgents/local.btkbd-inject.plist
U=$(stat -f %u /dev/console)
launchctl unload "$D" 2>/dev/null
[ "$U" != 0 ] && launchctl bootout gui/$U "$A" 2>/dev/null
rm -f "$D" "$A" /var/run/btkbd.sock /tmp/btkbd-inject.log
rm -rf /usr/local/btkbd
echo "btkbd removed"
