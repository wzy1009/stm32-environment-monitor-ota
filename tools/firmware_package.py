"""Merge initial HEX files or create a validated MQTT upgrade notification."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct
from urllib.parse import urlsplit

APP_BASE, APP_LIMIT = 0x08008000, 0x08080000

def notice(binary, version, url):
    if not 8 <= len(binary) <= APP_LIMIT - APP_BASE:
        raise ValueError('BIN must be 8..491520 bytes')
    stack, reset = struct.unpack_from('<II', binary)
    if not (0x20000000 < stack <= 0x20010000 and stack % 8 == 0 and reset & 1
            and APP_BASE + 8 <= (reset & ~1) < APP_BASE + len(binary)):
        raise ValueError('BIN vectors do not target the application partition')
    if not re.fullmatch(r'[A-Za-z0-9._-]{1,31}', version):
        raise ValueError('version must use 1..31 letters, digits, dot, underscore or dash')
    parsed = urlsplit(url)
    host = parsed.hostname or ''
    raw_port = parsed.netloc.partition(':')[2]
    if (not url.startswith('http://') or not host or len(host) > 63 or parsed.username
            or parsed.password or parsed.fragment or not parsed.path or len(url) >= 192
            or any(ord(c) <= 32 or ord(c) > 126 or c in '\\"' for c in url)
            or any(not re.fullmatch(r'[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?', label)
                   for label in host.split('.'))
            or re.search(r'%(?![A-Fa-f0-9]{2})', url)
            or ':' in parsed.netloc and not re.fullmatch(r'[1-9][0-9]{0,4}', raw_port)
            or parsed.port is not None and not 1 <= parsed.port <= 65535):
        raise ValueError('URL must be a direct HTTP path accepted by this firmware')
    # Reject a version label which was not embedded in this actual build.
    if version.encode('ascii') + b'\0' not in binary:
        raise ValueError('version does not match an embedded firmware version string')
    value = dict(cmd='ota', version=version, url=url, size=len(binary), md5=hashlib.md5(binary).hexdigest())
    text = json.dumps(value, separators=(',', ':'))
    if len(text.encode('ascii')) > 360:
        raise ValueError('notification exceeds the firmware 360-byte limit')
    return text

def read_hex(path):
    data, base, ended = {}, 0, False
    for line in path.read_text(encoding='ascii').splitlines():
        if not line:
            continue
        if ended or not line.startswith(':'):
            raise ValueError('unexpected HEX content or data after EOF')
        row = bytes.fromhex(line[1:])
        if len(row) < 5 or len(row) != row[0] + 5 or sum(row) & 255:
            raise ValueError('invalid HEX length/checksum')
        count, offset, kind = row[0], int.from_bytes(row[1:3], 'big'), row[3]
        payload = row[4:-1]
        if kind == 0:
            if offset + count > 65536:
                raise ValueError('HEX data record crosses an address segment')
            for i, value in enumerate(payload):
                address = base + offset + i
                if address in data:
                    raise ValueError('overlapping HEX data')
                data[address] = value
        elif kind == 1 and count == 0 and offset == 0:
            ended = True
        elif kind == 4 and count == 2 and offset == 0:
            base = int.from_bytes(payload, 'big') << 16
        elif kind == 5 and count == 4 and offset == 0:
            pass  # Cortex-M boots from the vector table, not this debug start record.
        else:
            raise ValueError('unsupported HEX record')
    if not ended or not data:
        raise ValueError('HEX is empty or has no EOF record')
    return data

def record(kind, offset, payload=b''):
    row = bytes([len(payload)]) + offset.to_bytes(2, 'big') + bytes([kind]) + payload
    return ':' + (row + bytes([-sum(row) & 255])).hex().upper()

def merge(boot, app):
    segments = [(read_hex(boot), 0x08000000, APP_BASE), (read_hex(app), APP_BASE, APP_LIMIT)]
    combined = {}
    for data, low, high in segments:
        if min(data) < low or max(data) >= high:
            raise ValueError('HEX contains data outside its assigned partition')
        combined.update(data)
    for address, low, high in [(0x08000000, 0x08000008, APP_BASE), (APP_BASE, APP_BASE + 8, APP_LIMIT)]:
        try:
            stack, entry = struct.unpack('<II', bytes(combined[address + i] for i in range(8)))
        except KeyError as error:
            raise ValueError('HEX is missing its vector table') from error
        if not (0x20000000 < stack <= 0x20010000 and stack % 8 == 0 and entry & 1
                and low <= (entry & ~1) < high and (entry & ~1) in combined):
            raise ValueError('HEX vector does not match its partition')
    if bytes(combined.get(0x08007800 + i, 255) for i in range(8)) != bytes.fromhex('5AA5A55A3CC3C33C'):
        raise ValueError('Boot first-setup marker is missing')
    output, base, addresses = [], None, sorted(combined)
    i = 0
    while i < len(addresses):
        address = addresses[i]
        if address >> 16 != base:
            base = address >> 16
            output.append(record(4, 0, base.to_bytes(2, 'big')))
        payload = bytearray([combined[address]])
        i += 1
        while (i < len(addresses) and len(payload) < 16 and addresses[i] == address + len(payload)
               and addresses[i] >> 16 == base):
            payload.append(combined[addresses[i]])
            i += 1
        output.append(record(0, address & 65535, payload))
    output.append(record(1, 0))
    return '\n'.join(output) + '\n'

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    tasks = parser.add_subparsers(dest='task', required=True)
    command = tasks.add_parser('notice')
    command.add_argument('--bin', required=True, type=Path, dest='binary')
    command.add_argument('--version', required=True)
    command.add_argument('--url', required=True)
    command.add_argument('--out', required=True, type=Path)
    command = tasks.add_parser('merge')
    command.add_argument('--boot', required=True, type=Path)
    command.add_argument('--app', required=True, type=Path)
    command.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    try:
        text = (notice(args.binary.read_bytes(), args.version, args.url) if args.task == 'notice'
                else merge(args.boot, args.app))
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text.rstrip() + '\n', encoding='utf-8')
    except (ValueError, OSError) as error:
        parser.error(str(error))
    print(args.out)

if __name__ == '__main__':
    main()
