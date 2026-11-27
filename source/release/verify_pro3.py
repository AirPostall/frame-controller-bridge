"""Verify an ESP32-S3 release's USB and optional Bluetooth Pro 3 inputs against this Mac's mappings.

Use the ESP-IDF Python environment (pyelftools) and supply the build's ELF and app bin.
This is an offline consistency check. It requires no connected controller.
"""

import argparse
from collections import defaultdict
from pathlib import Path
import plistlib
import re
import struct

from elftools.elf.elffile import ELFFile
from make_setup import inspect


def require(condition, message):
    if not condition:
        raise ValueError(message)


def flash_symbols(elf_path, image, names):
    """Read ELF symbol contents, then independently compare at each ESP segment load address."""
    segments = []
    pos = 24
    for _ in range(image[1]):
        addr, length = struct.unpack_from('<II', image, pos)
        pos += 8
        segments.append((addr, memoryview(image)[pos:pos + length]))
        pos += length

    values = {}
    with elf_path.open('rb') as stream:
        elf = ELFFile(stream)
        table = elf.get_section_by_name('.symtab')
        for name in names:
            symbols = table.get_symbol_by_name(name) or []
            require(len(symbols) == 1, f'Expected one ELF symbol named {name}')
            symbol = symbols[0]
            section = elf.get_section(symbol['st_shndx'])
            addr, size = symbol['st_value'], symbol['st_size']
            start = addr - section['sh_addr']
            data = section.data()[start:start + size]
            matches = [seg[addr-base:addr-base+size] for base, seg in segments
                       if base <= addr and addr + size <= base + len(seg)]
            require(size and len(matches) == 1 and matches[0] == data,
                    f'{name} does not match the application image at 0x{addr:08x}')
            values[name] = data
    return values


def input_fields(report):
    """Expand HID short items from the FrameBridge descriptor into input bit offsets."""
    fields = defaultdict(list)
    offsets = defaultdict(int)
    feature_bits = defaultdict(int)
    state, usages, globals_stack = {}, [], []
    minimum = None
    pos = 0
    while pos < len(report):
        prefix = report[pos]
        size = (0, 1, 2, 4)[prefix & 3]
        kind, tag = (prefix >> 2) & 3, prefix >> 4
        require(prefix != 0xfe and pos + size + 1 <= len(report), 'Unsupported or truncated HID item')
        value = int.from_bytes(report[pos + 1:pos + 1 + size], 'little')
        pos += size + 1
        if kind == 1:  # Global item
            if tag == 10:
                globals_stack.append(state.copy())
            elif tag == 11:
                state = globals_stack.pop()
            else:
                state[tag] = value
        elif kind == 2:  # Local usage, usage minimum or usage maximum
            if tag == 0:
                usages.append(value)
            elif tag == 1:
                minimum = value
            elif tag == 2:
                require(minimum is not None, 'Usage maximum without minimum')
                usages.extend(range(minimum, value + 1))
        elif kind == 0:
            if tag == 8:  # Input
                page, width, count = state[0], state[7], state[9]
                report_id = state.get(8, 0)
                if not value & 1 and page in (1, 2, 9):  # Standard, nonconstant input fields
                    require(value & 2 and len(usages) == count,
                            'Expected a separate HID usage for each variable input')
                    for i, usage in enumerate(usages):
                        field = (page, usage, report_id, offsets[report_id] + i*width, width)
                        if page == 9:
                            fields[1].append(field)
                        elif (page == 1 and 0x30 <= usage <= 0x38) or page == 2:
                            fields[2].append(field)
                        elif (page, usage) == (1, 0x39):
                            fields[3].append(field)
                offsets[report_id] += width * count
            elif tag == 11:  # Feature reports have independent offsets from input reports.
                feature_bits[state.get(8, 0)] += state[7] * state[9]
            usages.clear()
            minimum = None
    require(not globals_stack, 'Unbalanced HID global state')
    require(dict(feature_bits) == {6: 104}, 'Expected the 13-byte Pro 3 sensor capability report (ID 6)')
    return fields, offsets


def check_apple_mapping(bundle, transport, identity, fields):
    with (bundle / 'Info.plist').open('rb') as f:
        devices = plistlib.load(f)['Devices']
    entry = next(d for d in devices if d['Identifier'] == 'com.8BitDo.Pro3.' + transport)
    ids = dict(zip(('VendorID', 'ProductID', 'VersionNumber'), identity))
    require(entry['IOPropertyMatch'] == ids, f'Apple {transport} identity mismatch: {ids}')
    require(entry['Personalities'][0].endswith('/WithBackButtons.plist'), 'Expanded personality order changed')
    with (bundle / entry['Personalities'][0]).open('rb') as f:
        personality = plistlib.load(f)
    require('IOPropertyMatchingPredicate' not in personality,
            'Apple has added an expanded-profile matching predicate that needs review')
    model = personality['Model']
    bindings = {}
    for element in model['Driver']['Elements']:
        match = re.fullmatch(r'UsageType == (\d+) AND UsageTypeIndex == (\d+)', element['Predicate'])
        require(match, f"Unrecognized Apple element predicate: {element['Predicate']}")
        kind, index = map(int, match.groups())
        require(index < len(fields[kind]), f"Descriptor lacks the input for {element['Identifier']}")
        bindings[element['Identifier']] = fields[kind][index]
    require(len(set(bindings.values())) == len(bindings), 'Apple element predicates overlap')
    # Check that the actual HID usages at Apple's ordinal positions are the ones
    # produced by FrameBridge. Constant padding or a missing intermediate control
    # must not silently shift the remaining controls onto the wrong mapping.
    expected_usages = {
        'button.a': (9, 1), 'button.b': (9, 2), 'button.c': (9, 4), 'button.d': (9, 5),
        'button.l': (9, 7), 'button.r': (9, 8), 'button.select': (9, 11), 'button.start': (9, 12),
        'button.logo': (9, 13), 'thumbstick.left.button': (9, 14), 'thumbstick.right.button': (9, 15),
        'button.pl': (9, 6), 'button.pr': (9, 3), 'button.l4': (9, 17), 'button.r4': (9, 18),
        'thumbstick.left.x': (1, 0x30), 'thumbstick.left.y': (1, 0x31),
        'thumbstick.right.x': (1, 0x32), 'thumbstick.right.y': (1, 0x35),
        'button.r2': (2, 0xc4), 'button.l2': (2, 0xc5), 'dpad': (1, 0x39),
    }
    for identifier, usage in expected_usages.items():
        require(bindings[identifier][:2] == usage, f'HID usage mismatch for {identifier}')
    extras = ('button.pl', 'button.pr', 'button.l4', 'button.r4')
    physical = {e['Identifier']: e for e in model['PhysicalInput']['Elements']}
    events = {e['ExtendedIndex']: e['SourceExpression'] for e in model['Driver']['Input']['GamepadEventFields']}
    for control in (*extras, 'button.l2', 'button.r2'):
        field = bindings[control]
        element = physical[control]
        analog = control in ('button.l2', 'button.r2')
        event = events[element['PressedValueSource']]['InputExpression']
        require(event['ElementIdentifier'] == control and element['Analog'] == analog,
                f'Apple input routing changed for {control}')
        require((field[-1] >= 8) if analog else (field[-1] == 1), f'Unexpected precision for {control}')
    return ids, bindings


def usb_inputs(configuration, report_size):
    pos, packets = 0, []
    require(len(configuration) == struct.unpack_from('<H', configuration, 2)[0],
            'USB configuration length mismatch')
    found_hid = False
    while pos < len(configuration):
        size, kind = configuration[pos:pos + 2]
        require(size >= 2 and pos + size <= len(configuration), 'Invalid USB configuration descriptor')
        d = configuration[pos:pos + size]
        if kind == 0x21:
            found_hid = len(d) == 9 and d[6] == 0x22 and int.from_bytes(d[7:9], 'little') == report_size
        elif kind == 5 and d[2] & 0x80 and d[3] & 3 == 3:
            packets.append(int.from_bytes(d[4:6], 'little'))
        pos += size
    require(found_hid and len(packets) == 1, 'Expected HID report size and one interrupt IN endpoint')
    return packets[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    parser.add_argument('app', type=Path)
    parser.add_argument('--apple-bundle', type=Path)
    parser.add_argument('--bluetooth', action='store_true', help='also verify the BLE HID report and PnP ID')
    args = parser.parse_args()
    bundle = args.apple_bundle
    if not bundle:
        assets = Path('/System/Library/AssetsV2/com_apple_MobileAsset_GameController_DB1')
        bundles = list(assets.glob('*.asset/AssetData/GameControllers-Custom.bundle'))
        require(len(bundles) == 1, 'Select one installed GameControllers-Custom.bundle with --apple-bundle')
        bundle = bundles[0]

    image = args.app.read_bytes()
    layout = inspect(image)
    require(image[layout['record'] + 36] == 2, 'Factory image must default to the wired Pro 3 layout')
    names = ('pro3_device', 'pro3_configuration', 'pro3_report_descriptor', 'pro3_feature')
    symbols = flash_symbols(args.elf, image, names)
    device, config, report, feature = (symbols[name] for name in names)
    require(len(feature) == 13 and feature[12] == 0xaa, 'Pro 3 timestamp capability missing')
    require(device[:2] == bytes((18, 1)) and len(device) == 18, 'Invalid USB device descriptor')
    packet_size = usb_inputs(config, len(report))
    fields, offsets = input_fields(report)
    require(dict(offsets) == {4: 248}, 'Expected 31-byte wired enhanced input (ID 4)')
    for report_id, bits in offsets.items():
        require((bits + 7)//8 + bool(report_id) <= packet_size, 'Input report exceeds the interrupt endpoint')
    ids, bindings = check_apple_mapping(bundle, 'USB', struct.unpack_from('<HHH', device, 8), fields)
    print(f"Apple Pro 3 USB match: {ids['VendorID']:04x}:{ids['ProductID']:04x}, revision {ids['VersionNumber']:04x}")
    print(f"{len(bindings)} Apple predicates resolved. Report IDs and payload lengths (bits): {dict(offsets)}")
    for control in ('button.pl', 'button.pr', 'button.l4', 'button.r4', 'button.l2', 'button.r2'):
        page, usage, report_id, bit, width = bindings[control]
        print(f'{control:10}  usage {page:02x}/{usage:02x}  report {report_id}, bit {bit}, width {width}')
    if args.bluetooth:
        ble = flash_symbols(args.elf, image, ('ble_pad_pnp_id', 'ble_pad_report_map'))
        source, vendor, product, revision = struct.unpack('<BHHH', ble['ble_pad_pnp_id'])
        require(source == 2, 'Expected USB-IF vendor namespace in the BLE Device Information Service')
        ble_fields, ble_offsets = input_fields(ble['ble_pad_report_map'])
        require(dict(ble_offsets) == {4: 80, 1: 248},
                'Expected Bluetooth ten-byte controls (ID 4) and 31-byte enhanced input (ID 1)')
        _, ble_bindings = check_apple_mapping(bundle, 'Bluetooth', (vendor, product, revision), ble_fields)
        require(ble_bindings == bindings, 'USB and Bluetooth Pro 3 controls disagree')
        print(f'Apple Bluetooth match: {vendor:04x}:{product:04x}, revision {revision:04x}; '
              f'{len(ble_bindings)} controls resolved in the ten-byte report.')


if __name__ == '__main__':
    main()
