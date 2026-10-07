#!/usr/bin/env python3
"""Build a clean scratch copy with public credentials only, never touching shared .pio."""
import hashlib
import os
from pathlib import Path
import shutil
import sys
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]
SCRATCH=Path(os.environ.get('TMPDIR',str(Path.home()/'.hermes/cache/scratch')))

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    target=Path(tempfile.mkdtemp(prefix='epaper-public-build-',dir=SCRATCH))
    excluded={'secrets.h','.pio','.git','__pycache__','.pytest_cache','.vscode'}
    def ignore(directory,names):
        return [name for name in names if name in excluded]
    for directory in ('firmware','managed_components'):
        if (ROOT/directory).exists(): shutil.copytree(ROOT/directory,target/directory,ignore=ignore)
    for name in ('CMakeLists.txt','platformio.ini','dependencies.lock','sdkconfig.waveshare_epaper_154_v2'):
        if (ROOT/name).exists(): shutil.copy2(ROOT/name,target/name)
    # secrets.h has been excluded everywhere, including the public fixture's conventional name.
    # This is the ONLY secrets file ever read: the explicitly public compile fixture.
    shutil.copy2(ROOT/'firmware/tests/build_fixture/secrets.h',target/'firmware/include/secrets.h')
    sources={str(p.relative_to(ROOT)):digest(p) for folder in ('firmware/src','firmware/include')
        for p in (ROOT/folder).rglob('*') if p.is_file() and p.name!='secrets.h'}
    assert all(digest(target/name)==sha for name,sha in sources.items()),'Scratch copy differs from production source'
    (target/'source-hashes.txt').write_text('\n'.join(f'{sha}  {name}' for name,sha in sorted(sources.items()))+'\n')
    print(f'Public fixture scratch: {target}',flush=True)
    env=os.environ.copy()
    env.pop('PLATFORMIO_BUILD_FLAGS',None)
    subprocess.run(['pio','run','-e','waveshare_epaper_154_v2','-t','clean'],cwd=target,env=env,check=True)
    subprocess.run(['pio','run','-e','waveshare_epaper_154_v2'],cwd=target,env=env,check=True)
    subprocess.run([sys.executable,'firmware/tests/test_web_build_assets.py'],cwd=target,env=env,check=True)
    assert all(digest(ROOT/name)==sha and digest(target/name)==sha for name,sha in sources.items()),'Source changed during build: rerun before reporting latest-source verification'
    print(f'PASS clean public fixture build; {len(sources)} production source hashes and all embedded web asset bytes verified. No flash. {target}',flush=True)

if __name__=='__main__': main()
