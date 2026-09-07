# SPDX-License-Identifier: GPL-2.0-or-later
"""One device's explicit DAC volume preference, separate from effect profiles."""
import json
import os
import tempfile
from pathlib import Path

CONTROL = 'AE-5: Direct DAC Playback Volume'

def preference_path(home):
    return Path(home)/'.config/aegaudio/direct-dac.json'

def identity(card):
    return dict(pci=Path(card['device_path']).name, vendor=card['vendor'], subsystem=card['subsystem'])

def validate(data, expected):
    if not isinstance(data,dict) or data.get('schema')!=1 or data.get('device')!=expected:
        raise ValueError('DAC volume preference does not match this AE-5.')
    values=data.get('values')
    if not isinstance(values,list) or len(values)!=2 or any(type(v) is not int or not 0<=v<=255 for v in values):
        raise ValueError('DAC volume preference must contain two values from 0 to 255.')
    return [str(v) for v in values]

def load(path, expected):
    path=Path(path)
    if not path.exists():return None
    if path.is_symlink() or not path.is_file() or path.stat().st_size>4096:
        raise ValueError('DAC volume preference is not a small regular file.')
    return validate(json.loads(path.read_text()),expected)

def save(path, expected, values):
    data=dict(schema=1,device=expected,values=[int(v) for v in values])
    validate(data,expected)
    path=Path(path);path.parent.mkdir(parents=True,exist_ok=True)
    if path.is_symlink():raise ValueError('DAC volume preference cannot be a symlink.')
    fd,name=tempfile.mkstemp(prefix='.direct-dac-',dir=path.parent)
    try:
        with os.fdopen(fd,'w') as f:
            json.dump(data,f);f.write('\n');f.flush();os.fsync(f.fileno())
        os.replace(name,path)
    finally:
        if os.path.exists(name):os.unlink(name)
