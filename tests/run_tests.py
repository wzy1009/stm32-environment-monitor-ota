"""Run host assertions against production C; use a caller-provided Windows TCC."""
from pathlib import Path
import argparse
import subprocess
import tempfile
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--tcc', required=True, type=Path)
parser.add_argument('--ota-only', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
build_root = root / 'build'
build_root.mkdir(exist_ok=True)

def run(command):
    subprocess.run([str(x) for x in command], cwd=root, check=True)

with tempfile.TemporaryDirectory(prefix='tests-', dir=build_root) as temp:
    build = Path(temp)
    headers = build / 'include'
    headers.mkdir()
    for source in (root / 'firmware/Core/Inc').glob('*.h'):
        (headers / source.name).write_text(source.read_text(encoding='utf-8-sig'), encoding='utf-8')
    def suite(name, sources, definitions=(), cloud=False):
        files = []
        for relative in sources:
            source = root / relative
            target = build / source.name
            target.write_text(source.read_text(encoding='utf-8-sig'), encoding='utf-8')
            files.append(target)
        command = [args.tcc, '-Wall', '-Werror', *definitions]
        if cloud:
            command += ['-I' + str(root / 'tests/stubs')]
        command += ['-I' + str(root / 'tests/ota_stubs'), '-I' + str(headers)]
        executable = build / (name + '.exe')
        run(command + [*files, '-o', executable])
        return executable
    def core(names):
        return ['firmware/Core/Src/' + name + '.c' for name in names.split()]
    if not args.ota_only:
        run([suite('local', ['tests/test_local_monitor.c', *core('monitor_state dht11')])])
        run([suite('cloud', ['tests/test_cloud.c', *core('cloud_task cloud_mqtt esp_stream monitor_state')],
                   ['-DENV_OTA_APP'], cloud=True)])
    ota = suite('ota', ['tests/test_ota.c', *core('ota_notice ota_download ota_meta ota_image http_image utils_md5')])
    for scenario in range(10):
        run([ota, str(scenario)])
    boot = suite('boot', ['tests/test_boot.c', 'firmware/Boot/boot_install.c', 'firmware/Boot/boot_flash.c',
                         *core('ota_meta ota_image utils_md5')], ['-DENV_BOOT'])
    run([boot])
    run([sys.executable, root / 'tests/test_package.py'])
print('PASS: selected production-code assertions; physical hardware remains untested')
