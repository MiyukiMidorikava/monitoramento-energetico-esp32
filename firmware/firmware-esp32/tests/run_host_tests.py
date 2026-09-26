"""Compile real firmware C with simulated peripherals and the ESP-IDF cJSON library."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--idf-path', default=os.environ.get('IDF_PATH'))
parser.add_argument('--zig', help='Optional path to zig (uses zig cc)')
parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
args = parser.parse_args()
if not args.idf_path:
    parser.error('Set IDF_PATH or pass --idf-path')
root = Path(__file__).resolve().parents[1]
cjson = Path(args.idf_path) / 'components/json/cJSON'
headers = ['esp_err.h', 'esp_wifi.h', 'esp_timer.h', 'esp_log.h', 'sdkconfig.h', 'nvs.h', 'nvs_flash.h', 'esp_attr.h',
           'esp_adc/adc_continuous.h', 'soc/soc_caps.h',
           'driver/i2c_master.h', 'ssd1306.h', 'freertos/FreeRTOS.h',
           'esp_app_desc.h', 'esp_image_format.h', 'esp_http_client.h', 'esp_crt_bundle.h', 'esp_ota_ops.h', 'esp_system.h',
           'esp_task_wdt.h', 'mbedtls/pk.h', 'mbedtls/sha256.h',
           'freertos/task.h', 'freertos/semphr.h', 'freertos/event_groups.h', 'freertos/queue.h']
with tempfile.TemporaryDirectory(prefix='energia-dia2-') as work:
    work = Path(work)
    for header in headers:
        path = work / header
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "host_platform.h"\n', encoding='utf-8')
    binary = work / ('tests.exe' if os.name == 'nt' else 'tests')
    command = [args.zig, 'cc'] if args.zig else [args.cc]
    command += ['-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-UNDEBUG',
                '-I' + str(work), '-I' + str(root / 'tests'),
                '-I' + str(root / 'main'), '-I' + str(cjson),
                str(root / 'tests/test_eletrica.c'), str(cjson / 'cJSON.c'),
                '-lm', '-o', str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)

    ota_binary = work / ('test_ota.exe' if os.name == 'nt' else 'test_ota')
    ota_command = [args.zig, 'cc'] if args.zig else [args.cc]
    ota_command += ['-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-UNDEBUG',
                    '-I' + str(work), '-I' + str(root / 'tests'),
                    '-I' + str(root / 'main'), str(root / 'tests/test_ota.c'),
                    '-o', str(ota_binary)]
    subprocess.run(ota_command, check=True)
    subprocess.run([str(ota_binary)], check=True)

    http_binary = work / ('test_http.exe' if os.name == 'nt' else 'test_http')
    http_command = [args.zig, 'cc'] if args.zig else [args.cc]
    http_command += ['-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-UNDEBUG',
                     '-I' + str(work), '-I' + str(root / 'tests'),
                     '-I' + str(root / 'main'), str(root / 'tests/test_http.c'),
                     '-o', str(http_binary)]
    subprocess.run(http_command, check=True)
    subprocess.run([str(http_binary)], check=True)
