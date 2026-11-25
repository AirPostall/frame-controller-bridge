"""Embed a verified ESP32-S3 application in the offline controller setup page."""
import argparse
import base64
import hashlib
import json
import re
import struct
from pathlib import Path

MARKER = b"FRAMEBRIDGE-PAIR\0"
ADDRESS = re.compile(rb"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}\0")


def inspect(image):
    if len(image) < 56 or image[0] != 0xE9 or image[23] != 1 or int.from_bytes(image[12:14], "little") != 9:
        raise ValueError("Expected a hashed ESP32-S3 app image")
    offset = 24
    checksum = 0xEF
    records = []
    for _ in range(image[1]):
        _, length = struct.unpack_from("<II", image, offset)
        start = offset + 8
        offset = start + length
        if offset > len(image):
            raise ValueError("Invalid ESP segment length")
        payload = image[start:offset]
        for value in payload:
            checksum ^= value
        for match in re.finditer(MARKER, payload):
            record = match.end()
            if (ADDRESS.fullmatch(payload[record:record+18]) and
                    ADDRESS.fullmatch(payload[record+18:record+36]) and
                    payload[record+36:record+37] in (b"\x00", b"\x01", b"\x02")):
                records.append(start + record)

    checksum_at = (offset // 16 + 1) * 16 - 1
    if len(records) != 1 or checksum_at + 33 != len(image):
        raise ValueError("Invalid pairing record or image layout")
    if image[checksum_at] != checksum or hashlib.sha256(image[:-32]).digest() != image[-32:]:
        raise ValueError("Invalid application checksum or SHA-256")
    return {"record": records[0], "checksum": checksum_at, "defaultMode": image[records[0]+36]}


def setup_page(image):
    layout = inspect(image)
    record = layout["record"]

    def form_value(offset):
        address = image[offset:offset+17].decode()
        return "" if address == "00:00:00:00:00:00" else address

    template = Path(__file__).with_name("setup-template.html").read_text(encoding="utf-8")
    return (template.replace("@@IMAGE@@", base64.b64encode(image).decode())
            .replace("@@LAYOUT@@", json.dumps(layout))
            .replace("@@LEFT@@", form_value(record))
            .replace("@@RIGHT@@", form_value(record+18)))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("application", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.write_text(setup_page(args.application.read_bytes()), encoding="utf-8")
