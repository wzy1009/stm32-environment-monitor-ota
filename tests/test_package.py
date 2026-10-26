from pathlib import Path
import importlib.util
import json
import struct
import tempfile

root = Path(__file__).resolve().parents[1]
(root / 'build').mkdir(exist_ok=True)
spec = importlib.util.spec_from_file_location('firmware_package', root / 'tools/firmware_package.py')
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)

def rejected(function, *args):
    try:
        function(*args)
    except ValueError:
        return
    raise AssertionError('invalid firmware input accepted')

binary = struct.pack('<II', 0x20010000, 0x08008009) + b'0.4.1\0' + b'x' * 100
value = json.loads(package.notice(binary, '0.4.1', 'http://192.168.1.2:8080/new.bin?rev=1'))
assert value['size'] == len(binary) and value['version'] == '0.4.1'
rejected(package.notice, binary, '0.4.2', 'http://host/a')
for url in ['https://host/a', 'http://host:080/a', 'http://host:0/a', 'http://host/a\r\nX:y',
            'http://user:password@host/a', 'http://host/a#fragment', 'http://host/a%ZZ']:
    rejected(package.notice, binary, '0.4.1', url)
rejected(package.notice, struct.pack('<II', 0x20010000, 0x08000009) + binary[8:], '0.4.1', 'http://host/a')

with tempfile.TemporaryDirectory(dir=root / 'build', prefix='package-test-') as directory:
    directory = Path(directory)
    boot, app, combined = [directory / name for name in ['boot.hex', 'app.hex', 'initial.hex']]
    def make(path, records):
        path.write_text('\n'.join(records + [package.record(1, 0)]) + '\n', encoding='ascii')
    make(boot, [package.record(4, 0, bytes.fromhex('0800')),
                package.record(0, 0, struct.pack('<II', 0x20010000, 0x08000101)),
                package.record(0, 0x100, b'\x00\xbf'),
                package.record(0, 0x7800, bytes.fromhex('5AA5A55A3CC3C33C'))])
    make(app, [package.record(4, 0, bytes.fromhex('0800')), package.record(0, 0x8000, binary[:16])])
    combined.write_text(package.merge(boot, app), encoding='ascii')
    assert package.read_hex(combined) == {**package.read_hex(boot), **package.read_hex(app)}
    make(app, [package.record(4, 0, bytes.fromhex('0800')), package.record(0, 0, binary[:16])])
    rejected(package.merge, boot, app)
    app.write_text(':00000001FE\n', encoding='ascii')
    rejected(package.read_hex, app)
print('PASS package: BIN version/vector/URL validation and checked initial HEX merge')
