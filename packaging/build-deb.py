#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build an unsigned exact-kernel package in an isolated checkout; never install."""
from pathlib import Path
import argparse,hashlib,json,shutil,subprocess,os

ROOT=Path(__file__).resolve().parent.parent
def run(args):subprocess.run(args,check=True)

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--kernel',default='7.0.0-31-generic')
    ap.add_argument('--out',type=Path,default=ROOT/'dist')
    a=ap.parse_args()
    if a.kernel!='7.0.0-31-generic':ap.error('Only the recovered kernel is qualified for compilation in this release.')
    a.out=a.out.resolve();a.out.mkdir(parents=True,exist_ok=True)
    headers=Path('/lib/modules')/a.kernel/'build'
    run(['make','-C',str(headers),'M='+str(ROOT/'driver'),'-j2','modules'])
    # Unique stage directory avoids deleting existing files or an earlier artifact.
    import tempfile
    stage=Path(tempfile.mkdtemp(prefix='ae5-deb-',dir=a.out))
    lib=stage/'usr/lib/ae5-direct';lib.mkdir(parents=True)
    for p in (ROOT/'integration').iterdir():
        if p.is_file() and p.suffix in ('.py','.conf','.service'):shutil.copy2(p,lib/p.name)
    shutil.copy2(ROOT/'aegaudio/dac_settings.py',lib/'dac_settings.py')
    unit=(lib/'ae5-direct.service').read_text().replace('/usr/local/lib/ae5-direct','/usr/lib/ae5-direct')
    unit=unit.replace('user@1000.service','user@@UID@.service')
    (lib/'ae5-direct.service.in').write_text(unit);(lib/'ae5-direct.service').unlink()
    sleep=(lib/'ae5-direct-sleep.service').read_text().replace('/usr/local/lib/ae5-direct','/usr/lib/ae5-direct')
    target=stage/'usr/lib/systemd/system';target.mkdir(parents=True)
    (target/'ae5-direct-sleep.service').write_text(sleep)
    source=stage/'usr/src/ae5-direct-0.17';source.mkdir(parents=True)
    for p in (ROOT/'driver').iterdir():
        if p.suffix in ('.c','.h') or p.name=='Makefile':
            if p.name.endswith('.mod.c'):continue
            shutil.copy2(p,source/p.name)
    mods=stage/'lib/modules'/a.kernel/'updates/ae5';mods.mkdir(parents=True)
    module=ROOT/'driver/snd-hda-codec-ca0132.ko';shutil.copy2(module,mods/module.name)
    metadata=stage/'DEBIAN';metadata.mkdir()
    (metadata/'control').write_text('Package: ae5-direct-driver\nVersion: 0.17~preview1-1\nArchitecture: amd64\nMaintainer: Aegiris Systems\nSection: sound\nPriority: optional\nDepends: python3, alsa-utils, pipewire, pipewire-pulse, wireplumber, kmod\nDescription: Experimental AE-5 Direct 384 kHz and live DAC volume for kernel '+a.kernel+'\n Does not activate audio on installation. PipeWeaver is required for the\n recovered integration and must be installed separately.\n')
    (metadata/'postinst').write_text('#!/bin/sh\nset -eu\nif [ "$1" = configure ]; then\n depmod '+a.kernel+'\n systemctl daemon-reload || true\n echo "Installed only. Configure explicitly; no audio service was started."\nfi\n')
    (metadata/'prerm').write_text('#!/bin/sh\nset -eu\nif [ "$1" = remove ] || [ "$1" = deconfigure ]; then\n if systemctl is-active --quiet ae5-direct.service; then\n  echo "Stop Direct mode explicitly before removal." >&2\n  exit 1\n fi\nfi\n')
    for p in metadata.iterdir():p.chmod(0o755 if p.name in ('postinst','prerm') else 0o644)
    docs=stage/'usr/share/doc/ae5-direct-driver';docs.mkdir(parents=True)
    for p in (ROOT/'docs').glob('*.md'):shutil.copy2(p,docs/p.name)
    tests=docs/'tests';tests.mkdir()
    for p in (ROOT/'tests').iterdir():
        if p.is_file():shutil.copy2(p,tests/p.name)
    if (ROOT/'LICENSE').exists():shutil.copy2(ROOT/'LICENSE',docs/'copyright')
    for p in stage.rglob('*'):
        if p.is_dir():p.chmod(0o755)
        elif p.parent!=metadata:p.chmod(0o644)
    package=a.out/'ae5-direct-driver_0.17~preview1-1_amd64.deb'
    run(['dpkg-deb','--root-owner-group','--build',str(stage),str(package)])
    manifest={'kernel':a.kernel,'package':package.name,'package_sha256':hashlib.sha256(package.read_bytes()).hexdigest(),
              'module_sha256':hashlib.sha256(module.read_bytes()).hexdigest(),
              'module_signed':False,'installed':False,'playback_tests_run':False,
              'srcversion':subprocess.check_output(['modinfo','-F','srcversion',str(module)],text=True).strip(),
              'source_sha256':{p.relative_to(ROOT).as_posix():hashlib.sha256(p.read_bytes()).hexdigest()
                  for d in ('driver','integration') for p in (ROOT/d).iterdir()
                  if p.suffix in ('.c','.h','.py','.conf','.service') and not p.name.endswith('.mod.c')},
              'dac_settings_sha256':hashlib.sha256((ROOT/'aegaudio/dac_settings.py').read_bytes()).hexdigest()}
    (a.out/'build-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(json.dumps(manifest))

if __name__=='__main__':main()
