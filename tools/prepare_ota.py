"""Validate an OTA APP BIN and write its MQTT notification, without sending it."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
from urllib.parse import urlsplit

def notice(binary, version, url):
    if not 8 <= len(binary) <= 0x78000:
        raise ValueError('BIN must be 8..491520 bytes')
    stack, reset = struct.unpack_from('<II', binary)
    if not (0x20000000 < stack <= 0x20010000 and stack % 8 == 0 and reset & 1
            and 0x08008008 <= (reset & ~1) < 0x08008000 + len(binary)):
        raise ValueError('BIN vectors do not target the ENV_OTA partition')
    if not re.fullmatch(r'[A-Za-z0-9._-]{1,31}', version):
        raise ValueError('version must use 1..31 ASCII letters, digits, dot, underscore or dash')
    parsed = urlsplit(url)
    host = parsed.hostname or ''
    if (parsed.scheme != 'http' or not host or len(host) > 63 or parsed.username
            or parsed.password or parsed.fragment or not parsed.path or len(url) >= 192
            or any(ord(c) <= 32 or ord(c) > 126 or c in '\\"' for c in url)
            or any(not re.fullmatch(r'[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?', label)
                   for label in host.split('.'))
            or re.search(r'%(?![A-Fa-f0-9]{2})', url)
            or parsed.port is not None and not 1 <= parsed.port <= 65535):
        raise ValueError('URL must be a directly reachable HTTP firmware path')
    result = dict(cmd='ota', version=version, url=url, size=len(binary), md5=hashlib.md5(binary).hexdigest())
    text = json.dumps(result, separators=(',', ':'))
    if len(text.encode('ascii')) > 360:
        raise ValueError('notification exceeds the firmware 360-byte limit')
    return text

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bin', required=True, type=Path, dest='binary')
    parser.add_argument('--version', required=True)
    parser.add_argument('--url', required=True)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    try:
        text = notice(args.binary.read_bytes(), args.version, args.url)
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text + '\n', encoding='utf-8')
    except (ValueError, OSError) as error:
        parser.error(str(error))
    print(text)

if __name__ == '__main__':
    main()
