"""Run production C with host hardware substitutes; TCC is supplied by the caller."""
from pathlib import Path
import argparse
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--tcc', required=True, type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]

def run(command):
    subprocess.run([str(x) for x in command], cwd=root, check=True)

# ARMCC accepts UTF-8 BOM, the portable TinyCC build does not.
with tempfile.TemporaryDirectory(prefix='ota-tests-', dir=root) as temp:
    build = Path(temp)
    headers = build / 'include'
    headers.mkdir()
    for source in (root / 'firmware/Core/Inc').glob('*.h'):
        (headers / source.name).write_text(source.read_text(encoding='utf-8-sig'), encoding='utf-8')
    def suite(name, sources, stubs=None, definitions=()):
        files = []
        for relative in sources:
            source = root / relative
            target = build / source.name
            target.write_text(source.read_text(encoding='utf-8-sig'), encoding='utf-8')
            files.append(target)
        command = [args.tcc, '-Wall', '-Werror', *definitions]
        if stubs:
            command += ['-I' + str(root / stubs)]
        executable = build / (name + '.exe')
        run(command + ['-I' + str(headers), *files, '-o', executable])
        return executable

    local = suite('local', ['tests/test_local_monitor.c', 'firmware/Core/Src/monitor_state.c', 'firmware/Core/Src/dht11.c'])
    run([local])
    cloud = suite('cloud', ['tests/test_cloud.c', *['firmware/Core/Src/' + x + '.c' for x in ['cloud_task','cloud_mqtt','esp_stream','monitor_state']]], 'tests/stubs', ['-DENV_OTA_APP'])
    run([cloud])
    ota = suite('ota', ['tests/test_ota.c', *['firmware/Core/Src/' + x + '.c' for x in ['ota_notice','ota_download','ota_meta','ota_image','http_image','utils_md5']]], 'tests/ota_stubs')
    for scenario in range(8):
        run([ota, str(scenario)])
print('PASS: local, cloud and OTA regression suites')
