#!/bin/sh
# flash_modem.sh - write a md1img image into the phone's modem partition and verify the
# read-back.
#
#   ./flash_modem.sh /home/furios/modem-sdr-8bit.img
#   ./flash_modem.sh /home/furios/modem-E6.img        # back to stock
#
# The partition is only read by the bootloader (LK) at boot, so a reboot is needed after
# writing. If the modem does not come back, run this script again with the stock image and
# reboot: the Linux system does not depend on md1img.
#
# sdr-load runs this script (as root); it can also be used by hand. The PART= line is what
# sdr-load reads to know which partition to hash and verify.
set -eu

if [ $# -lt 1 ]; then
    echo "usage: $0 <md1img image>" >&2
    exit 1
fi
IMG=$1
PART=/dev/disk/by-partlabel/md1img_a

[ -f "$IMG" ] || { echo "missing $IMG"; exit 1; }
[ -e "$PART" ] || { echo "missing $PART"; exit 1; }

SIZE=$(stat -c %s "$IMG")
WANT=$(sha256sum "$IMG" | cut -d' ' -f1)
echo "image  : $IMG ($SIZE bytes)"
echo "sha256 : $WANT"
echo "target : $PART"

sudo dd if="$IMG" of="$PART" bs=1M conv=fsync 2>&1 | tail -1
sync

GOT=$(sudo head -c "$SIZE" "$PART" | sha256sum | cut -d' ' -f1)
echo "readback: $GOT"
if [ "$GOT" != "$WANT" ]; then
    echo "FAILED: the read-back does not match"
    exit 1
fi
echo "OK: partition written and verified. Reboot with: sudo systemctl reboot"
