#!/usr/bin/env python3
"""Exercise IDF FatFs, not POSIX mkdir, against a disposable RAM FAT32 disk."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
IDF = Path(os.environ.get('IDF_PATH', str(Path.home() / '.platformio/packages/framework-espidf')))
SOURCE = IDF / 'components/fatfs/src'


def config(path):
    return dict(re.findall(r'^(CONFIG_\w+)=(.+)$', path.read_text(), re.M))


def run():
    defaults = config(ROOT / 'firmware/sdkconfig.defaults')
    actual = config(ROOT / 'sdkconfig.waveshare_epaper_154_v2')
    # Fail for both fresh builds and existing PlatformIO configuration.
    for name, values in [('defaults', defaults), ('active', actual)]:
        assert values.get('CONFIG_FATFS_LFN_HEAP') == 'y', f'{name}: heap LFN required'
        assert values.get('CONFIG_FATFS_LFN_NONE') != 'y', f'{name}: 8.3-only paths break recording'
        assert int(values['CONFIG_FATFS_MAX_LFN']) >= 64, f'{name}: note filenames need long names'
    assert (SOURCE / 'ff.c').is_file(), f'Install the ESP-IDF PlatformIO package: {SOURCE}'
    with tempfile.TemporaryDirectory(prefix='fatfs-paths-', dir=os.environ.get('TMPDIR')) as tmp:
        temp = Path(tmp)
        (temp / 'freertos').mkdir()
        (temp / 'freertos/FreeRTOS.h').write_text('#define portTICK_PERIOD_MS 1\n')
        (temp / 'freertos/semphr.h').write_text('/* Single-threaded host adapter. */\n')
        paths = dict(re.findall(r'constexpr char (\w+)\[\] = "([^"]+)";', (ROOT / 'firmware/include/project_config.h').read_text()))
        mount = paths['SD_MOUNT_POINT']
        (temp / 'paths.h').write_text('\n'.join(f'#define {name} "0:{value[len(mount):]}"' for name, value in paths.items() if name != 'SD_MOUNT_POINT') + '\n')
        header = '\n'.join(f'#define {key} {1 if value == "y" else value}' for key, value in actual.items() if key.startswith(('CONFIG_FATFS_', 'CONFIG_WL_')))
        # Disabled boolean symbols used as C expressions by ffconf.h.
        header += '\n#define CONFIG_FATFS_USE_FASTSEEK 0\n#define CONFIG_FATFS_USE_LABEL 0\n#define CONFIG_FATFS_USE_DYN_BUFFERS 0\n'
        for mode in ('heap', 'none'):
            selected = header if mode == 'heap' else re.sub(r'^#define CONFIG_FATFS_LFN_HEAP .+$', '', header, flags=re.M) + '\n#define CONFIG_FATFS_LFN_NONE 1\n'
            (temp / 'sdkconfig.h').write_text(selected)
            exe = temp / mode
            command = [os.environ.get('CC', 'cc'), '-std=c11', '-I' + str(temp), '-I' + str(SOURCE), str(ROOT / 'firmware/tests/fatfs_paths_fixture.c'), str(SOURCE / 'ff.c'), str(SOURCE / 'ffunicode.c'), '-o', str(exe)]
            subprocess.run(command, check=True)
            subprocess.run([str(exe), mode], check=True)
    print('FatFs paths PASS: active/default LFN and real FAT32 create/write/read/remount; 8.3-only failure reproduced')


if __name__ == '__main__':
    run()
