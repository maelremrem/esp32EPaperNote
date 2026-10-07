#!/usr/bin/env python3
"""Verify the actual PlatformIO build embeds the current web assets, not stale data."""
from pathlib import Path
import re
root=Path(__file__).resolve().parents[2]
for name in ('index.html','style.css','tokens.css','app.js'):
    source=(root/'firmware/src/web'/name).read_bytes()
    assembly=(root/'.pio/build/waveshare_epaper_154_v2/esp-idf/src'/('carnet_'+name+'.S')).read_text()
    embedded=bytes(int(x,16) for line in assembly.splitlines() if line.startswith('.byte') for x in re.findall(r'0x([0-9a-f]+)',line))
    assert embedded==source, f'{name}: stale embedded asset; use pio run -t clean then fixture build'
    print(f'PASS current embedded {name} ({len(source)} bytes)')
