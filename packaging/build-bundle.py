#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build preview2 from committed sources and the unchanged preview1 driver.

No installation, audio-device access or service operation is performed.
Requires Git, pip, setuptools and wheel on the build machine.
"""
from pathlib import Path, PurePosixPath
import argparse
import hashlib
import io
import json
import os
import shutil
import subprocess
import tarfile
import tempfile
import zipfile
import sys

ROOT = Path(__file__).resolve().parent.parent
TAG = 'v0.16.0-preview2'
DRIVER_COMMIT = '0ffc2dd786f8fd25ff3fc4d8e8660d996ed5be53'
DRIVER_SHA = '94bd5dc6638ba85f6c0070ec93865e76aec5223b0b3407fa618ee84e33d3964f'
DRIVER_MANIFEST_SHA = 'a7b66309a4ca161060b3e09e1b6242c95ce27ed18113f9da44d97435ffadce57'
HIDDEN = {'creationflags': 0x08000000} if os.name == 'nt' else {}


def run(*args):
    return subprocess.check_output(args, cwd=ROOT, **HIDDEN)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--driver-package', type=Path, required=True)
    parser.add_argument('--driver-manifest', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True, help='New or empty artifact directory')
    args = parser.parse_args()
    driver = args.driver_package.resolve()
    driver_manifest = args.driver_manifest.resolve()
    if sha(driver) != DRIVER_SHA or sha(driver_manifest) != DRIVER_MANIFEST_SHA:
        parser.error('Expected the exact published preview1 package and build-manifest.json.')
    if run('git', 'status', '--porcelain', '--untracked-files=no').strip():
        parser.error('Commit tracked source changes before bundling.')
    commit = run('git', 'rev-parse', 'HEAD').decode().strip()
    delta = run('git', 'diff', DRIVER_COMMIT, commit, '--', 'driver', 'integration', 'packaging/build-deb.py')
    if delta:
        parser.error('Driver/build/integration source changed; cannot reuse the old binary.')
    out = args.out.resolve()
    if out.exists() and any(out.iterdir()):
        parser.error('Output must be a new or empty directory; existing artifacts are not overwritten.')
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='ae5-bundle-', dir=out.parent) as temp:
        source = Path(temp) / ('ae5-linux-' + TAG)
        source.mkdir()
        raw = run('git', 'archive', '--format=tar', commit)
        with tarfile.open(fileobj=io.BytesIO(raw), mode='r:') as archive:
            for item in archive:
                name = PurePosixPath(item.name)
                if name.is_absolute() or '..' in name.parts or not (item.isdir() or item.isfile()):
                    raise RuntimeError('Unexpected source archive entry: ' + item.name)
                target = source.joinpath(*name.parts)
                if item.isdir():
                    target.mkdir(parents=True, exist_ok=True)
                else:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(archive.extractfile(item).read())
                    target.chmod(item.mode & 0o777)
        # This uses the isolated committed tree, not mutable/untracked workspace files.
        subprocess.run([sys.executable, '-m', 'pip', 'wheel', '--no-deps',
                        '--no-build-isolation', str(source), '--wheel-dir', str(out)],
                       check=True, **HIDDEN)
        wheel = out / 'aegaudio-0.2.0-py3-none-any.whl'
        if not wheel.exists():
            raise RuntimeError('Expected UI version 0.2.0.')
        with zipfile.ZipFile(wheel) as z:
            for path in (source / 'aegaudio').rglob('*'):
                if path.is_file() and '__pycache__' not in path.parts:
                    if z.read(path.relative_to(source).as_posix()) != path.read_bytes():
                        raise RuntimeError('Wheel source mismatch: ' + str(path))
            entry = z.read('aegaudio-0.2.0.dist-info/entry_points.txt').decode()
            if 'aegaudio-web = aegaudio.web:main' not in entry:
                raise RuntimeError('Browser entry point missing.')
        # Do not bundle generated build directories or wheel metadata in the source tree.
        for name in ('build', 'aegaudio.egg-info'):
            generated = source / name
            if generated.exists():
                if generated.is_symlink() or not generated.resolve().is_relative_to(source.resolve()):
                    raise RuntimeError('Build output escaped the temporary source directory.')
                shutil.rmtree(generated)
        packages = source / 'packages'
        packages.mkdir()
        shutil.copy2(driver, out / 'ae5-direct-driver_0.16-1_amd64.deb')
        shutil.copy2(driver_manifest, out / 'driver-build-manifest.json')
        for name in ('ae5-direct-driver_0.16-1_amd64.deb', wheel.name, 'driver-build-manifest.json'):
            shutil.copy2(out / name, packages / name)
        manifest = {
            'release': TAG, 'source_commit': commit,
            'driver_source_commit': DRIVER_COMMIT,
            'driver_reused_without_changes': True,
            'driver_sha256': DRIVER_SHA, 'ui_version': '0.2.0',
            'ui_wheel_sha256': sha(wheel),
            'kernel': '7.0.0-31-generic', 'architecture': 'amd64',
            'module_signed': False, 'driver_integration_source_matches_preview1': True,
            'wheel_source_and_assets_match_commit': True,
            'fresh_installation_tested': False, 'audio_tests_run_for_bundle': False,
            'reboot_suspend_qualified': False,
            'not_bundled': ['kernel headers', 'distribution dependencies', 'PySide6',
                            'ALSA firmware', 'PipeWeaver', 'private configurations', 'signing keys'],
        }
        text = json.dumps(manifest, indent=2) + '\n'
        (out / 'bundle-manifest.json').write_text(text, encoding='utf-8')
        (source / 'bundle-manifest.json').write_text(text, encoding='utf-8')
        files = sorted(p for p in source.rglob('*') if p.is_file())
        sums = ''.join(sha(p) + '  ' + p.relative_to(source).as_posix() + '\n' for p in files)
        (source / 'SHA256SUMS').write_text(sums, encoding='utf-8')
        bundle = out / (source.name + '-bundle.tar.gz')
        with tarfile.open(bundle, 'w:gz') as archive:
            archive.add(source, arcname=source.name)
        # Independently check every archived file against the internal manifest.
        with tarfile.open(bundle, 'r:gz') as archive:
            for line in sums.splitlines():
                expected, name = line.split('  ', 1)
                data = archive.extractfile(source.name + '/' + name).read()
                if hashlib.sha256(data).hexdigest() != expected:
                    raise RuntimeError('Bundled file differs: ' + name)
        assets = sorted(p for p in out.iterdir() if p.is_file())
        (out / 'SHA256SUMS').write_text(''.join(sha(p) + '  ' + p.name + '\n' for p in assets), encoding='utf-8')
        print(json.dumps({'bundle': str(bundle), 'source_commit': commit,
                          'files_verified_in_bundle': len(files), 'assets': len(assets) + 1,
                          'hardware_access': False}, indent=2))


if __name__ == '__main__':
    main()
