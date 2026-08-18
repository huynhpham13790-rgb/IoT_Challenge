#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
make_ota.py - Boc mot file .gbl thanh file .ota chuan Zigbee.

TAI SAO CAN SCRIPT NAY
  Simplicity Commander chi tao duoc file .gbl (dinh dang cua Gecko Bootloader).
  Zigbee2MQTT lai can file .ota - tuc la file .gbl DOI THEM 62 byte "mu" o dau.
  62 byte do la thu duy nhat Z2M doc de biet "ban nay cua hang nao, loai gi,
  version may" ma khong can mo phan than.

CAU TRUC FILE .OTA  (tong = 56 + 6 + kich thuoc GBL)

  --- HEADER OTA, 56 byte ---
  offset  kieu      gia tri
    0     u32       magic            0x0BEEF11E  (nhan dien file OTA)
    4     u16       headerVersion    0x0100
    6     u16       headerLength     56
    8     u16       fieldControl     0x0000      (khong dung truong tuy chon)
   10     u16       manufacturerCode 0x1049      <-- phai khop voi firmware
   12     u16       imageType        0x0000      <-- phai khop voi firmware
   14     u32       fileVersion      3           <-- so version cua ban nay
   18     u16       stackVersion     0x0002      (Zigbee Pro)
   20     char[32]  headerString     mo ta dang text, padding bang \\0
   52     u32       totalImageSize   tong kich thuoc CA FILE .ota

  --- SUB-ELEMENT, 6 byte ---
   56     u16       tagId            0x0000      (0 = "upgrade image")
   58     u32       length           kich thuoc file .gbl

  --- THAN ---
   62     bytes     toan bo file .gbl, KHONG sua doi gi

CACH DUNG
  python make_ota.py <file.gbl> <version> [file-ra.ota]

VI DU
  python make_ota.py base\\smart-iv-monitor-v3.gbl 3

LUU Y
  manufacturerCode va imageType phai khop voi cau hinh firmware:
    config/ota-client-policy-config.h -> IMAGE_TYPE_ID
    config/zcl/zcl_config.zap         -> manufacturer code 0x1049
  Sai mot trong hai, client tra ve INVALID_FIELD va bo qua file.
"""

import struct
import sys
import os

# ---- Phai khop voi firmware. Doi o day neu doi ben firmware. ----
MANUFACTURER_CODE = 0x1049
IMAGE_TYPE        = 0x0000

OTA_MAGIC       = 0x0BEEF11E
HEADER_VERSION  = 0x0100
HEADER_LENGTH   = 56
FIELD_CONTROL   = 0x0000
STACK_VERSION   = 0x0002   # Zigbee Pro
TAG_UPGRADE_IMAGE = 0x0000

# Tag dau tien cua mot file GBL hop le (tren dia: EB 17 A6 03).
GBL_HEADER_TAG    = 0x03A617EB


def build_ota(gbl_bytes, file_version, header_string):
    """Tra ve noi dung file .ota hoan chinh."""

    # headerString la truong co dinh 32 byte, thieu thi dem \0.
    name = header_string.encode("utf-8")[:32]
    name = name.ljust(32, b"\x00")

    total_size = HEADER_LENGTH + 6 + len(gbl_bytes)

    header = struct.pack(
        "<IHHHHHIH",          # little-endian: u32 u16 u16 u16 u16 u16 u32 u16
        OTA_MAGIC,
        HEADER_VERSION,
        HEADER_LENGTH,
        FIELD_CONTROL,
        MANUFACTURER_CODE,
        IMAGE_TYPE,
        file_version,
        STACK_VERSION,
    )
    header += name                          # offset 20..51
    header += struct.pack("<I", total_size)  # offset 52..55

    assert len(header) == HEADER_LENGTH, "Header phai dung 56 byte"

    sub_element = struct.pack("<HI", TAG_UPGRADE_IMAGE, len(gbl_bytes))

    return header + sub_element + gbl_bytes


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1

    gbl_path = sys.argv[1]
    version  = int(sys.argv[2])

    if len(sys.argv) >= 4:
        ota_path = sys.argv[3]
    else:
        # smart-iv-monitor-v3.gbl -> smart-iv-monitor-v3.ota
        ota_path = os.path.splitext(gbl_path)[0] + ".ota"

    if not os.path.isfile(gbl_path):
        print("LOI: khong tim thay %s" % gbl_path)
        return 1

    gbl = open(gbl_path, "rb").read()

    # Kiem tra so bo: file GBL bat dau bang tag header, magic 0x03A617EB
    # (tren dia la 4 byte: EB 17 A6 03).
    if len(gbl) < 4 or struct.unpack_from("<I", gbl, 0)[0] != GBL_HEADER_TAG:
        print("CANH BAO: %s khong bat dau bang magic GBL (0x03A617EB)." % gbl_path)
        print("          Ban co chac day la file .gbl chu khong phai .s37 / .bin?")
        return 1

    header_string = "FPT xG26 OTA client v%d" % version
    ota = build_ota(gbl, version, header_string)

    open(ota_path, "wb").write(ota)

    print("Da tao: %s" % ota_path)
    print("  manufacturerCode : 0x%04X" % MANUFACTURER_CODE)
    print("  imageType        : 0x%04X" % IMAGE_TYPE)
    print("  fileVersion      : %d" % version)
    print("  headerString     : %s" % header_string)
    print("  kich thuoc GBL   : %d byte" % len(gbl))
    print("  tong file .ota   : %d byte  (= 56 + 6 + %d)" % (len(ota), len(gbl)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
