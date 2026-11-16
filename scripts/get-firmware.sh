#!/bin/sh
# Downloads the Realtek RTL8761BU firmware from the official linux-firmware repository.
# (Realtek's firmware is redistributable, but it isn't ours to ship; this fetches it from the source.)
set -e
cd "$(dirname "$0")/.."
mkdir -p fw
BASE=https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/rtl_bt
for f in rtl8761bu_fw.bin rtl8761bu_config.bin; do
  if [ ! -s "fw/$f" ]; then
    echo "downloading $f"
    curl -fsSL -o "fw/$f" "$BASE/$f"
  fi
done
head -c 8 fw/rtl8761bu_fw.bin | grep -q Realtech || { echo "fw/rtl8761bu_fw.bin doesn't look like Realtek firmware"; exit 1; }
echo "firmware ready in fw/"
