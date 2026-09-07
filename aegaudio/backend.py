# SPDX-License-Identifier: GPL-2.0-or-later
"""Typed ALSA controls. Importing and reading never writes audio state."""
from pathlib import Path
import json
import re
import subprocess

HEAD = re.compile(r"^numid=(\d+),iface=(\w+),name='(.*?)'(.*)$", re.M)
PROTECTED = {'AE-5: Headphone Gain', 'Output Select',
             'HP/Speaker Auto Detect Playback Switch', 'Surround Channel Config'}

def run(args):
    p = subprocess.run(args, capture_output=True, text=True, timeout=20)
    if p.returncode:
        raise RuntimeError(p.stderr.strip() or p.stdout.strip() or f'{args[0]} failed')
    return p.stdout

def cards():
    result = []
    path = Path('/proc/asound/cards')
    if not path.exists():
        return result
    for m in re.finditer(r'^\s*(\d+)\s+\[(\S+)\s*\]:\s*(.*?) - (.+)$', path.read_text(), re.M):
        index = int(m[1]); base = Path(f'/proc/asound/card{index}')
        codec = '\n'.join(p.read_text(errors='replace') for p in base.glob('codec#*'))
        vendor = re.search(r'Vendor Id: (0x[0-9a-f]+)', codec)
        subsystem = re.search(r'Subsystem Id: (0x[0-9a-f]+)', codec)
        sysdev = Path(f'/sys/class/sound/card{index}/device').resolve()
        result.append(dict(index=index, id=m[2], name=m[4], device_path=str(sysdev),
                           vendor=vendor[1] if vendor else '',
                           subsystem=subsystem[1] if subsystem else ''))
    return result

def parse_controls(text):
    matches = list(HEAD.finditer(text)); result = []
    for i, m in enumerate(matches):
        block = text[m.end():matches[i+1].start() if i+1 < len(matches) else len(text)]
        # PCM maps and IEC958 byte arrays are information, not generic slider controls.
        meta = re.search(r'; type=([^\n]+)', block)
        if not meta:
            continue
        fields = dict(part.split('=',1) for part in ('type='+meta[1]).split(',') if '=' in part)
        vals = re.search(r': values=([^\n]+)', block)
        idx = re.search(r',index=(\d+)', m[4]); dev = re.search(r',device=(\d+)', m[4])
        c = dict(numid=int(m[1]), iface=m[2], name=m[3], index=int(idx[1]) if idx else 0,
                 device=int(dev[1]) if dev else 0, type=fields['type'],
                 writable='w' in fields.get('access',''), count=int(fields.get('values','1')),
                 values=vals[1].strip().split(',') if vals else [],
                 items=re.findall(r"; Item #\d+ '(.*)'",block))
        for k in ('min','max','step'):
            if k in fields:c[k]=int(fields[k])
        db = re.search(r'dBscale-min=([\d.-]+)dB,step=([\d.-]+)dB',block)
        if db:c['db_min'],c['db_step']=map(float,db.groups())
        c['key'] = json.dumps([c[k] for k in ('iface','name','index','device')])
        result.append(c)
    return result

def controls(card):
    return parse_controls(run(['amixer','-c',str(card['index']),'contents']))

def current_card(selected):
    found = [c for c in cards() if c['device_path']==selected['device_path'] and
             c['vendor']==selected['vendor'] and c['subsystem']==selected['subsystem']]
    if len(found)!=1:
        raise RuntimeError('Selected device changed or disconnected. Refresh the device list.')
    return found[0]

def validate(c, values):
    if not c['writable'] or c['iface']!='MIXER':
        raise ValueError('This control is read-only in AegAudio.')
    if len(values)!=c['count']:
        raise ValueError('Channel count does not match the control.')
    out=[]
    for value in values:
        if c['type']=='BOOLEAN':
            if value not in ('on','off'):raise ValueError('Expected on or off.')
            out.append(value)
        elif c['type'] in ('INTEGER','ENUMERATED'):
            v=int(value)
            lo,hi=(0,len(c['items'])-1) if c['type']=='ENUMERATED' else (c['min'],c['max'])
            if not lo<=v<=hi:raise ValueError('Value outside the ALSA control range.')
            out.append(str(v))
        else:raise ValueError('This control type is not writable in AegAudio.')
    return out

def set_control(selected, old, values, allow_protected=False):
    card=current_card(selected)
    matches=[c for c in controls(card) if c['key']==old['key']]
    if len(matches)!=1:raise RuntimeError('Control no longer resolves uniquely.')
    c=matches[0]
    if c['name'] in PROTECTED and not allow_protected:
        raise ValueError('Output/gain changes require an explicit selection outside profiles.')
    processing=(c['name'].startswith(('FX:','EQ Band','Enable InFX','Enable OutFX','VoiceFX')))
    if (c['name'] in PROTECTED or processing) and state(card)['direct']:
        raise ValueError('Direct playback became active. Stop Direct mode before changing this control.')
    if c['values']!=old['values']:
        raise RuntimeError('Control changed externally. Refresh before applying your change.')
    values=validate(c,values)
    run(['amixer','-c',str(card['index']),'cset',f"numid={c['numid']}",','.join(values)])
    readback=[x for x in controls(card) if x['key']==c['key']][0]
    if readback['values']!=values:raise RuntimeError('ALSA readback differs from the requested value.')
    return readback

def state(card):
    base=Path(f"/proc/asound/card{card['index']}")
    streams={str(p.relative_to(base)):p.read_text().strip() for p in base.glob('pcm*/sub*/hw_params')}
    codec='\n'.join(p.read_text(errors='replace') for p in base.glob('codec#*'))
    dacs=[]
    for b in re.split(r'\n(?=Node 0x)',codec):
        head=re.match(r'Node (0x[0-9a-f]+) \[Audio Output\]',b)
        amp=re.search(r'Amp-Out vals:\s*\[(.*?)\]',b)
        if head and amp:
            values=amp[1].split();dacs.append(dict(node=head[1],amps=values,
                muted=[bool(int(v,16)&128) for v in values]))
    # PCM state is accessible to the seat user; no elevation or active probe.
    direct=any('rate: 384000 ' in s and 'format: S32_LE' in s for s in streams.values())
    return dict(streams=streams,dacs=dacs,direct=direct,
                evidence='PCM format and amplifier registers; audibility is not measured.')

def profile(card, all_controls):
    return dict(schema=1, vendor=card['vendor'],subsystem=card['subsystem'],
                controls=[{k:c[k] for k in ('key','name','type','values')} for c in all_controls
                          if c['iface']=='MIXER' and c['writable'] and c['type'] in ('BOOLEAN','INTEGER','ENUMERATED')
                          and c['name'] not in PROTECTED])

def profile_plan(card, data):
    if not isinstance(data,dict) or data.get('schema')!=1:
        raise ValueError('Unsupported profile format.')
    if data.get('vendor')!=card['vendor'] or data.get('subsystem')!=card['subsystem']:
        raise ValueError('Profile was saved for a different codec/subsystem.')
    rows=data.get('controls')
    if not isinstance(rows,list) or len(rows)>256:raise ValueError('Invalid profile controls.')
    available={c['key']:c for c in controls(current_card(card))};plan=[];seen=set()
    for row in rows:
        c=available.get(row['key'])
        if not c or c['key'] in seen or c['name'] in PROTECTED:
            raise ValueError('Unknown, duplicate, or protected control in profile.')
        seen.add(c['key']);values=validate(c,row['values'])
        if c['values']!=values:plan.append((c,values))
    # Kernel EQ preset writes DSP state; apply individual bands after the preset.
    return sorted(plan,key=lambda x: (x[0]['name'].startswith('EQ Band'),x[0]['name']))

def apply_profile(card, plan):
    applied=[]
    try:
        for old,values in plan:
            readback=set_control(card,old,values);applied.append((old,readback))
    except Exception as exc:
        failures=[]
        for old,current in reversed(applied):
            try:set_control(card,current,old['values'])
            except Exception as rollback:failures.append(str(rollback))
        raise RuntimeError(f'Profile stopped: {exc}. Rollback errors: {failures or "none"}') from exc
