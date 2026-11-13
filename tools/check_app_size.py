#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fail if a built app image uses more than a given share of its app slot.

Reads the app binary named in <build>/project_description.json and the
partition table the build generated (<build>/partition_table/partition-table.bin),
and compares the image against the SMALLEST app partition in that table: an OTA
image has to fit whichever slot is inactive.

    tools/check_app_size.py build.hp6 0.85

No ESP-IDF needed; the binary partition table is parsed directly.
"""
import json
import os
import struct
import sys

ENTRY = struct.Struct("<HBBII16sI")  # magic, type, subtype, offset, size, label, flags
MAGIC = 0x50AA
TYPE_APP = 0x00


def app_partitions(table_path):
    with open(table_path, "rb") as f:
        data = f.read()
    parts = []
    for off in range(0, len(data) - ENTRY.size + 1, ENTRY.size):
        magic, ptype, _sub, offset, size, label, _flags = ENTRY.unpack_from(data, off)
        if magic != MAGIC:
            break  # 0xEBEB MD5 marker or 0xFFFF padding ends the entries
        if ptype == TYPE_APP:
            parts.append((label.rstrip(b"\0").decode(), offset, size))
    return parts


def main():
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} <build-dir> <max-fraction, e.g. 0.85>")
    build, limit = sys.argv[1], float(sys.argv[2])

    with open(os.path.join(build, "project_description.json")) as f:
        desc = json.load(f)
    app_bin = os.path.join(build, desc["app_bin"])
    app_size = os.path.getsize(app_bin)

    parts = app_partitions(os.path.join(build, "partition_table", "partition-table.bin"))
    if not parts:
        sys.exit("error: no app partitions in the generated partition table")
    label, _off, slot = min(parts, key=lambda p: p[2])

    used = app_size / slot
    print(f"{desc['app_bin']}: {app_size} B in {label} ({slot} B): "
          f"{used:.1%} used, limit {limit:.0%}, {slot - app_size} B free")
    if used > limit:
        sys.exit(f"error: app image is {used:.1%} of its {slot} B slot, over the "
                 f"{limit:.0%} limit. Trim the image or revisit the limit.")


if __name__ == "__main__":
    main()
