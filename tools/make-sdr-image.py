#!/usr/bin/env python3
"""Build the SDR-mode modem image from the phone's own stock image.

No firmware is distributed with this project. The SDR mode needs a patched copy of the
modem image that the phone already runs, so this tool applies the patch **on the stock
image you provide** and verifies both ends by sha256:

    stock image  22c1909dbf9f0455c1a93f8a8e0a141245cc9b2d979c44d0c8da75f5a3424896
    SDR image    5a625327a06fcacf12f611419437bc85aa54c339911a8d3427262fea0643b4d3

The second one is the image the loader knows as profile ``sdr-8bit``: 8 bits per
component, the scanner's capture copied to the AP ring and fixed tuning at 2,115.0 MHz
(band 1).

Usage
-----
    python3 tools/make-sdr-image.py /path/to/stock-modem.img
    python3 tools/make-sdr-image.py /path/to/stock-modem.img -o modem-sdr-8bit.img
    python3 tools/make-sdr-image.py --check /path/to/stock-modem.img

Then, on the phone (from the repository root)::

    scp modem-sdr-8bit.img tools/flash_modem.sh furios@<phone>:/home/furios/
    scp /path/to/stock-modem.img furios@<phone>:/home/furios/modem-E6.img   # rescue copy
    sudo sdr-load --image /home/furios/modem-sdr-8bit.img     # writes and verifies; no reboot
    sudo systemctl reboot                                     # the bootloader reads it
    sudo sdr-load --resume                                    # enters and activates the mode
    # optional: sudo sdr-load --activate starts another sweep

The stock image is the modem partition of your own phone (``md1img_a``). It is the
same image the vendor package or a backup contains; the tool refuses anything whose
sha256 is not the one above, so a different firmware version cannot be patched by
accident.
"""

from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path

IMAGE_SIZE = 63018064
STOCK_SHA256 = "22c1909dbf9f0455c1a93f8a8e0a141245cc9b2d979c44d0c8da75f5a3424896"
SDR_SHA256 = "5a625327a06fcacf12f611419437bc85aa54c339911a8d3427262fea0643b4d3"
PROFILE = "sdr-8bit"

# (offset, new bytes as hex). Only the ranges that change are stored: 1,694 bytes in
# seven runs. They carry the capture copy, the 8-bit format and the fixed tuning.
PATCH = [
    (0x03AE982, "2b2920af"),
    (0x040078A, "2629be90c017e160fefffdffecc804102629be90"),
    (0x04035E8, "122ba800"),
    (0x044C896, "212b5ecf"),
    (0x14D9564, "042ad89e"),
    (0x1523440,
     "806100368c4aac850080c06138534339cd8920000890a06138534339ac850090"
     "ac010400cc0100020d840090ac91cda9f73f0890ec876490ac850480a991ac85"
     "04902023c0013983ff202c87689020ab040008901a38806100368c4aec876480"
     "fb2bd15a806100368c4aec876480e0db351e006200368c4a8061f8a61d25ac85"
     "4c80d0850880cd89c4010890b0850890d0856080c0a90e000890c0010100d085"
     "6090aa190890b0851c906d820180bff1cc855880d0857890cc855080d0857c90"
     "cc855480d08580904000010062221010cc8558804e205072c08910000890d085"
     "6c80c991d0856c9010180890d0858880c991d085889056190890f38102c04c00"
     "5c00e221501122860080606040a71d2591a930c1089071882ac1089051840020"
     "508484901116921660e0600064a814c1089070e0600064880ac108905db2c088"
     "020108906000015d6688f8c0089009968a96468042c050842c909121d0794000"
     "0c004f2098910895cc854880d08530908061e4a61d25a011c011cc850080c0a9"
     "120008908c91a991e0010400eda9eb3f0890c011d0855890c089600008904e00"
     "0400606000378c4aa0011b00e2850080e38500904c906c90bf91a0a9ef3f0890"
     "ee0170004f840020508434904fa4a290508438904f840e6050843c904f841080"
     "508440904f841480508444904f841880508448904f841c8050844c908061f02a"
     "a028ac850480b0855090ac850220b085549009168b1670388061f8a61d25cc85"
     "4880f0850880eea90cc00890b0858c80a991b0858c90b0850c80a991b0850c90"
     "0c180890b0851480a991b0851490e5833230e0db231e2412806100398c4ab011"
     "c0011800ed850080ec850090ac918c91df91c0a9ef3f089091102063581a4090"
     "30dbe3832230e0db0411c58143c0c089140008906410a3a6003963002000df91"
     "c0a9f33f0890206100368c4a89851880a061380025b8ad85008028e35019e985"
     "5c8000030100cf810ec0d92150396060435344316784009087850490a7850890"
     "078710907594c9851c80c7851890c9852080c7851c90c9852480c7852090c985"
     "2880c7852490c9853480c7852890c9853880c7852c90c9853c80c7853090c985"
     "4080c7853490c9854480c7853890c9854880c7853c90c9854c80c7854090c985"
     "5080c7854490c9855480c7854890c9855880c7854c90c9853080c7855090c985"
     "6880c7855890c9857880c7855c90c9858480c7856090c06100398c4a6e840880"
     "678464906e840c806784689007846c9069848c8067846c900784549049842c80"
     "c001e00fc2a904c008904e10c210a70080000013688400806088040008900993"
     "658400900c91d192df9069bb089007875490c28102c0c7850c9089917494c985"
     "2c804e20d071c0896e000890ce10e99160000f006fa804000890e0116f800ec0"
     "79205039000301006060435344316784009087850490a7850890078710906000"
     "01006784149069841c80678418906984208067841c90a7008000688400806584"
     "00900c91d192df9071bb0890ce8102c0c7850c908991749489851890c9851080"
     "ca91c9851090a061380025b8ad850080c9857080c0890a000890cd21d071c985"
     "7490a9857090e9855c80ea9160000e006fa804000890e011e9855c90e0db"),
    (0x16697F8,
     "806100368c4aac850082a991ac850092a4851020ac850492a4850820ac850892"
     "a4853120ac850c92a4851860ac851092a0010200a4851010a0010300a4850810"
     "a4851860ac851492a4851020ac8518927429bbeb121e4e9a0890806100368c4a"
     "ac85b081a991ac85b091a4850080ac85b891c06101000200cda92a000890a485"
     "1060ac85bc91a0019e52a4851050a0010100a4850c10806100368c4aac85b481"
     "a991ac85b491e2831230d928256b121e0412008a1a010890806100368c4aac85"
     "2082a991ac8520920c863092b0850020ac852492d0851460cc852892f0851260"
     "ec852c92ee89e8c0089040006c00c22118105020501142002c004c843492a0a9"
     "4c00089062840c206c843892628410606c843c926000010062840c1060009e52"
     "62841050628464266c8440926088900008906284f8666c844492600001006284"
     "641660009e526284f85674180890600001006da8360008904e8001c050205011"
     "628428606c843892628464626c843c92600001006284285060009e5262846452"
     "600001006c844c923618089040004002c22118105020501162843e206c843892"
     "62843c606c843c926000010062843e1060009e5262843c50600002006c844c92"
     "806100368c4a6c84488269906c844892e2831230e683e035d428b34f"),
]


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def build(data: bytearray) -> None:
    for offset, hexs in PATCH:
        blob = bytes.fromhex(hexs)
        if offset + len(blob) > IMAGE_SIZE:
            raise SystemExit(f"patch out of range at 0x{offset:X}")
        data[offset:offset + len(blob)] = blob


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Build the SDR-mode modem image from your stock image "
                    f"(sha256 {STOCK_SHA256[:12]}...).")
    ap.add_argument("stock", help="stock modem image (md1img_a of the phone)")
    ap.add_argument("-o", "--output", default=f"modem-sdr-8bit.img",
                    help="output image (default: modem-sdr-8bit.img)")
    ap.add_argument("--check", action="store_true",
                    help="verify the stock image and report; write nothing")
    ap.add_argument("--force", action="store_true", help="overwrite the output file")
    args = ap.parse_args()

    src = Path(args.stock)
    try:
        data = bytearray(src.read_bytes())
    except OSError as e:
        print(f"error: cannot read {src}: {e}", file=sys.stderr)
        return 2

    if len(data) != IMAGE_SIZE:
        print(f"error: {src} has {len(data)} bytes; the modem image has {IMAGE_SIZE}",
              file=sys.stderr)
        return 2
    got = sha256(bytes(data))
    if got != STOCK_SHA256:
        print(f"error: {src} is not the expected stock image", file=sys.stderr)
        print(f"  expected sha256 {STOCK_SHA256}", file=sys.stderr)
        print(f"  got      sha256 {got}", file=sys.stderr)
        return 3

    print(f"stock image : {src} ({IMAGE_SIZE} bytes)")
    print(f"sha256      : {got}  OK")
    build(data)
    out_sha = sha256(bytes(data))
    if out_sha != SDR_SHA256:
        print(f"error: the result does not have the SDR image sha256", file=sys.stderr)
        print(f"  expected {SDR_SHA256}", file=sys.stderr)
        print(f"  got      {out_sha}", file=sys.stderr)
        return 4

    if args.check:
        print(f"SDR image   : {out_sha}  OK (profile {PROFILE}); nothing written")
        return 0

    out = Path(args.output)
    if out.exists() and not args.force:
        print(f"error: {out} exists; pass --force to overwrite it", file=sys.stderr)
        return 1
    out.write_bytes(bytes(data))
    print(f"SDR image   : {out} ({IMAGE_SIZE} bytes)")
    print(f"sha256      : {out_sha}  OK (profile {PROFILE})")
    print()
    print("Next, on the phone (from the repository root):")
    print(f"  scp {out} tools/flash_modem.sh furios@<phone>:/home/furios/")
    print(f"  scp {args.stock} furios@<phone>:/home/furios/modem-E6.img   # rescue copy")
    print(f"  sudo sdr-load --image /home/furios/{out.name}   # writes it; no reboot")
    print("  sudo systemctl reboot                            # the bootloader reads it")
    print("  sudo sdr-load --resume                          # enters and activates the mode")
    print("  # code 6 from --image is success (reboot needed); it disables WiFi/Bluetooth first")
    print("  # optional: sudo sdr-load --activate starts another sweep")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
