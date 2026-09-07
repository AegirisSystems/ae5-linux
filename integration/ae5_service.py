#!/usr/bin/python3
"""Host-scoped AE-5 Direct service with explicit output and mixer preservation."""
from pathlib import Path
import argparse,fcntl,hashlib,json,os,re,shutil,signal,socket,subprocess,sys,time,urllib.request,urllib.error
import dac_settings

INSTALL=Path('/usr/lib/ae5-direct')
RUNTIME=Path('/run/ae5-direct')
STATE=RUNTIME/'state.json'
PARAMS=Path('/sys/module/snd_hda_codec_ca0132/parameters')
UNITS=['pipeweaver.service','wireplumber.service','pipewire-pulse.service','pipewire-pulse.socket','pipewire.service','pipewire.socket']
CONTROLS=['AE-5: Headphone Gain','Output Select','Master Playback Volume','Master Playback Switch',
          'Front Playback Volume','Front Playback Switch','What U Hear Capture Volume','What U Hear Capture Switch']
STOP=False

def log(event,**data): print(json.dumps(dict(event=event,**data)),flush=True)
def config(): return json.loads((INSTALL/'installation.json').read_text())

def restore_dac_preference(card):
    """Queue the panel's device-scoped volume before Direct playback opens."""
    c=config()
    expected=dict(pci=c['pci'],vendor=c['vendor_id'],subsystem=c['subsystem_id'])
    try:
        values=dac_settings.load(dac_settings.preference_path(c['home']),expected)
    except (OSError,ValueError) as exc:
        log('dac-preference-ignored',reason=str(exc));return
    if values is None:return
    before=command(['amixer','-c',str(card),'cget','name='+dac_settings.CONTROL],check=False)
    if before['exit']:
        log('dac-preference-ignored',reason='Loaded driver has no Direct DAC volume control');return
    command(['amixer','-c',str(card),'cset','name='+dac_settings.CONTROL,','.join(values)])
    actual=command(['amixer','-c',str(card),'cget','name='+dac_settings.CONTROL])['stdout']
    if mixer_value(actual)!=','.join(values):raise RuntimeError('Saved DAC volume readback differs')
    log('dac-preference-restored',values=values)
def digest(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def command(args,timeout=12,check=True):
    p=subprocess.run(args,capture_output=True,text=True,timeout=timeout)
    if check and p.returncode: raise RuntimeError(f'{args[0]} exit {p.returncode}: {p.stderr.strip()}')
    return dict(exit=p.returncode,stdout=p.stdout,stderr=p.stderr)
def user_command(args,timeout=12,check=True):
    c=config()
    env=dict(os.environ,HOME=c['home'],USER=c['user'],LOGNAME=c['user'],
        XDG_RUNTIME_DIR=f"/run/user/{c['uid']}",DBUS_SESSION_BUS_ADDRESS=f"unix:path=/run/user/{c['uid']}/bus")
    for key in ('PIPEWIRE_REMOTE','PIPEWIRE_RUNTIME_DIR','PIPEWIRE_CONFIG_DIR','PIPEWIRE_CONFIG_NAME','PIPEWIRE_CONFIG_PREFIX'):
        env.pop(key,None)
    p=subprocess.run(args,user=c['uid'],group=c['uid'],extra_groups=os.getgrouplist(c['user'],c['uid']),
        env=env,capture_output=True,text=True,timeout=timeout)
    if check and p.returncode: raise RuntimeError(f'{args[0]} exit {p.returncode}: {p.stderr.strip()}')
    return dict(exit=p.returncode,stdout=p.stdout,stderr=p.stderr)
def ctl(*args,check=True): return user_command(['systemctl','--user',*args],check=check)
def user_available(): return Path(f"/run/user/{config()['uid']}/bus").exists()
def units():
    raw=ctl('show',*UNITS,'-p','Id','-p','ActiveState','-p','MainPID')['stdout']
    out={}
    for block in raw.strip().split('\n\n'):
        d=dict(line.split('=',1) for line in block.splitlines() if '=' in line)
        if 'Id' in d: out[d['Id']]=d
    if set(out)!=set(UNITS): raise RuntimeError('Unexpected audio unit inventory')
    return out
def wait_units(active,timeout):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        u=units()
        if all(v['ActiveState'] in (('active',) if active else ('inactive','failed')) for v in u.values()): return u
        time.sleep(.4)
    raise RuntimeError('Audio service transition timed out: '+json.dumps(u))
def state(): return json.loads(STATE.read_text())
def save(s):
    RUNTIME.mkdir(exist_ok=True,mode=0o755)
    tmp=STATE.with_suffix('.tmp'); tmp.write_text(json.dumps(s,indent=2)); tmp.chmod(0o600); os.replace(tmp,STATE)
def pcm(card): return Path(f'/proc/asound/card{card}/pcm0p/sub0/hw_params').read_text()
def mixer(card): return {name:command(['amixer','-c',str(card),'cget','name='+name])['stdout'] for name in CONTROLS}

def mixer_value(text):
    match=re.search(r'^\s*: values=([0-9,onf-]+)\s*$',text,re.M)
    if not match: raise RuntimeError('Unexpected ALSA mixer value')
    return match[1]

def restore_mixer(card,expected):
    """Restore mixer state after a lifecycle transition; never change gain."""
    current=mixer(card)
    gain='AE-5: Headphone Gain'
    if current[gain]!=expected[gain]: raise RuntimeError('Headphone gain changed; not overwritten')
    for name in CONTROLS:
        if name==gain: continue
        current=mixer(card)
        if current[name]!=expected[name] or name in ('Front Playback Switch','Master Playback Switch'):
            command(['amixer','-c',str(card),'cset','name='+name,mixer_value(expected[name])])
    if mixer(card)!=expected: raise RuntimeError('Hardware controls could not be preserved')
    verify_front_mute(card,expected)
    log('mixer-preserved',gain_unchanged=True)

def verify_front_mute(card,expected):
    """Report the actual converter mute separately from cached mixer values."""
    must_be_on=(mixer_value(expected['Master Playback Switch'])=='on' and
                mixer_value(expected['Front Playback Switch'])=='on,on')
    codec=next(Path(f'/proc/asound/card{card}').glob('codec#*'),None)
    if codec is None: raise RuntimeError('AE-5 codec readback unavailable')
    text=codec.read_text()
    block=re.search(r'Node 0x02 .*?(?=\nNode |\Z)',text,re.S)
    amps=re.search(r'Amp-Out vals:\s*\[(0x[0-9a-f]+) (0x[0-9a-f]+)\]',block[0] if block else '')
    if not amps: raise RuntimeError('Front DAC amplifier readback unavailable')
    if must_be_on and any(int(v,16)&0x80 for v in amps.groups()):
        raise RuntimeError('Front DAC remains muted despite mixer state; no audible-output claim')

def pcm_matches(actual,expected):
    """Preserve rate/format/channels while allowing normal buffer negotiation."""
    fields=('format','channels','rate')
    def parsed(text):
        return {line.split(':',1)[0]:line.split(':',1)[1].strip().split(' ')[0]
            for line in text.splitlines() if ':' in line and line.split(':',1)[0] in fields}
    a,b=parsed(actual),parsed(expected)
    return a==b and (len(a)==3 or actual==expected)

def prepare_output(card):
    """Select this deployment's dedicated headphone output with the PCM closed."""
    c=config(); before=mixer(card)
    if pcm(card).strip()!='closed': raise RuntimeError('Output preparation requires a closed PCM')
    if 'output_select' in c:
        value=str(c['output_select'])
        if value not in ('0','1'): raise RuntimeError('Invalid configured output selector')
        if mixer_value(before['Output Select'])!=value:
            command(['amixer','-c',str(card),'cset','name=Output Select',value])
    if c.get('unmute_front_on_start') and mixer_value(before['Front Playback Switch'])!='on,on':
        command(['amixer','-c',str(card),'cset','name=Front Playback Switch','on,on'])
    after=mixer(card)
    for name in CONTROLS:
        if name not in ('Output Select','Front Playback Switch') and before[name]!=after[name]:
            raise RuntimeError('Unrelated mixer setting changed during output preparation: '+name)
    return after
def native(): return json.loads((PARAMS/'status').read_text())
def attached(): return (PARAMS/'status').exists() and bool(native()['ready'])
def native_command(value):
    with (PARAMS/'command').open('w') as f: f.write(value+'\n')
def notify(value):
    address=os.environ.get('NOTIFY_SOCKET')
    if not address: return
    if address.startswith('@'): address='\0'+address[1:]
    with socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM) as sock: sock.connect(address); sock.sendall(value.encode())
def discover():
    c=config(); matches=[]
    for path in Path('/sys/bus/hdaudio/devices').glob('hdaudioC*D*'):
        if c['pci'] not in path.resolve().parts: continue
        if (path/'vendor_id').read_text().strip().lower()!=c['vendor_id']: continue
        if (path/'subsystem_id').read_text().strip().lower()!=c['subsystem_id']: continue
        m=re.fullmatch(r'hdaudioC(\d+)D(\d+)',path.name)
        if m: matches.append(dict(codec=path.name,card=int(m[1])))
    if len(matches)!=1: raise RuntimeError('Configured AE-5 codec is absent or ambiguous')
    return matches[0]
def supported():
    c=config()
    if Path('/proc/sys/kernel/osrelease').read_text().strip()!=c['kernel']: return False
    if digest(c['module_path'])!=c['module_sha256']: return False
    if digest(c['stock_module'])!=c['stock_module_sha256']: return False
    return True
def stop_signal(*_):
    global STOP
    STOP=True
def check_stop():
    if STOP: raise InterruptedError('Service stop requested')

def copied_config(destination):
    """Only these two services see the runtime copy of the normal user config."""
    home=Path(config()['home'])/'.config'
    for tool in ('pipewire','wireplumber'):
        origin=home/tool; target=destination/tool; target.mkdir(parents=True,exist_ok=True)
        if not origin.exists(): continue
        for source in origin.rglob('*'):
            if source.is_symlink(): raise RuntimeError('Review audio config symlink before copying: '+str(source))
            relative=source.relative_to(origin); dest=target/relative
            if source.is_dir(): dest.mkdir(exist_ok=True)
            elif source.is_file():
                if source.stat().st_size>2*1024*1024: raise RuntimeError('Unexpected large audio config: '+str(source))
                dest.write_bytes(source.read_bytes()); dest.chmod(0o644)
    for p in destination.rglob('*'):
        if p.is_dir(): p.chmod(0o755)

def prepare_runtime(s):
    c=config(); root=RUNTIME/'config'
    if root.exists() or root.is_symlink():
        if root.is_symlink() or root.resolve().parent!=RUNTIME.resolve(): raise RuntimeError('Unexpected runtime config path')
        shutil.rmtree(root)
    root.mkdir(mode=0o755)
    copied_config(root)
    p=root/'pipewire/pipewire.conf.d/99-ae5-direct.conf'; p.parent.mkdir(parents=True,exist_ok=True)
    text=(INSTALL/'pipewire-direct.conf').read_text().replace('@CARD@',str(s['card'])).replace('@SINK@',c['sink'])
    p.write_text(text); p.chmod(0o644)
    p=root/'wireplumber/wireplumber.conf.d/99-ae5-direct.conf'; p.parent.mkdir(parents=True,exist_ok=True)
    p.write_text((INSTALL/'wireplumber-direct.conf').read_text().replace('@PCI@',c['pci'].replace(':','_'))); p.chmod(0o644)
    runtime_user=Path(f"/run/user/{c['uid']}/systemd/user")
    content='[Service]\nEnvironment=XDG_CONFIG_HOME=/run/ae5-direct/config\n'
    s['dropins']={str(runtime_user/(unit+'.d/90-ae5-direct.conf')):content for unit in ('pipewire.service','wireplumber.service')}
    save(s)
    for name,data in s['dropins'].items():
        path=Path(name)
        if path.exists() or path.is_symlink(): raise RuntimeError('Unexpected existing Direct service drop-in: '+name)
        path.parent.mkdir(parents=True,exist_ok=True)
        with path.open('x') as f: f.write(data)
        path.chmod(0o644)
    ctl('daemon-reload')

def remove_dropins(s):
    for name,data in s.get('dropins',{}).items():
        p=Path(name)
        if not p.exists() and not p.is_symlink(): continue
        if p.is_symlink() or not p.is_file() or p.read_text()!=data: raise RuntimeError('Direct drop-in was changed: '+name)
        p.unlink()
    if user_available(): ctl('daemon-reload')

def graph_health(s):
    n=native()
    if any(n.get(k) for k in ('prepare_error','cleanup_error','clock_error')): raise RuntimeError('Native Direct error: '+json.dumps(n))
    if not n['enabled']: raise RuntimeError('Direct callback integration is no longer enabled')
    if 'rate: 384000 ' not in pcm(s['card']): return False
    g=json.loads(user_command(['pw-dump'],timeout=8)['stdout'])
    nodes={o.get('info',{}).get('props',{}).get('node.name'):o for o in g if o['type'].endswith(':Node')}
    sink=nodes.get(config()['sink']); system=nodes.get('pipeweaver_system')
    if not sink or not system: return False
    edges={}
    for o in g:
        if o['type'].endswith(':Link') and o['info']['state']=='active':
            edges.setdefault(o['info']['output-node-id'],set()).add(o['info']['input-node-id'])
    seen=set(); todo=[system['id']]
    while todo:
        here=todo.pop()
        if here in seen: continue
        seen.add(here); todo.extend(edges.get(here,set())-seen)
    return bool(sink['id'] in seen and n['dma_running'] and n['dma_format']==0x1843 and n['wire_format']==0x1843 and
        n['clocks']==[0x80,7,0xc7,0x1f103,0x1f103,0x31002,0x2000f] and n['path_verified'] and n['route_verified'])

def control_health():
    try:
        with urllib.request.urlopen('http://127.0.0.1:14565/api/get-devices',timeout=2) as response:
            if response.status!=200: return False
            raw=response.read(2*1024*1024+1)
        if len(raw)>2*1024*1024: return False
        data=json.loads(raw)
        return isinstance(data,dict) and isinstance(data.get('config'),dict) and isinstance(data.get('audio'),dict) and isinstance(data['audio'].get('profile'),dict)
    except (OSError,ValueError,urllib.error.URLError): return False

def health(s): return graph_health(s) and control_health()

def wait_graph(s,timeout=150):
    begun=time.monotonic(); deadline=begun+timeout; repaired=False
    while time.monotonic()<deadline:
        check_stop()
        try:
            if health(s): return
        except subprocess.TimeoutExpired:
            log('graph-startup-wait',reason='PipeWire connection timed out')
        if not repaired and time.monotonic()-begun>=8:
            ctl('restart','--no-block','pipeweaver.service')
            repaired=True; s['recoveries']=s.get('recoveries',0)+1; save(s)
            log('startup-reconnect-requested',reason='Audio graph or PipeWeaver control API not ready')
        time.sleep(.5)
    raise RuntimeError('384 kHz graph or PipeWeaver controls did not connect: '+json.dumps(native()))

def activate():
    if not os.environ.get('INVOCATION_ID'): raise RuntimeError('Start through ae5-direct.service')
    if not supported(): raise RuntimeError('Unsupported kernel or changed module; ordinary audio retained')
    if STATE.exists() and state().get('phase') not in ('stopped',): cleanup()
    if list(Path('/sys/module').glob('ae5_*')): raise RuntimeError('Another AE-5 module is already loaded')
    device=discover(); card=device['card']; c=config()
    if Path('/sys/module/snd_hda_codec_ca0132/srcversion').read_text().strip()!=c['native_srcversion']: raise RuntimeError('Running codec differs from the installed package')
    deadline=time.monotonic()+180
    notify('STATUS=Waiting for the ordinary user audio services')
    while time.monotonic()<deadline:
        check_stop()
        try:
            if all(v['ActiveState']=='active' for v in units().values()) and 'rate: 96000 ' in pcm(card): break
        except (FileNotFoundError,RuntimeError,subprocess.TimeoutExpired): pass
        time.sleep(1)
    else: raise RuntimeError('Ordinary user audio was not ready; left unchanged')
    s=dict(device,phase='prepared',boot_id=Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
        original_pcm=pcm(card),original_mixer=mixer(card),dropins={},started=time.time(),recoveries=0)
    save(s)
    for name,value in dict(managed_mode='1',playback_uid=str(c['uid']),codec_name=s['codec']).items():
        (PARAMS/name).write_text(value+'\n')
    native_command('attach')
    s['phase']='loaded'; save(s); check_stop()
    notify('STATUS=Activating the verified 384 kHz Direct path')
    ctl('stop','--no-block',*UNITS); wait_units(False,105); check_stop()
    if pcm(card).strip()!='closed': raise RuntimeError('Audio PCM did not close')
    if mixer(card)!=s['original_mixer']: raise RuntimeError('Hardware controls changed during activation')
    s['direct_mixer']=prepare_output(card); save(s)
    native_command('check'); native_command('enable')
    restore_dac_preference(card)
    s['phase']='enabled'; save(s)
    prepare_runtime(s); check_stop()
    ctl('start','--no-block',*UNITS); wait_units(True,35)
    wait_graph(s)
    verify_front_mute(card,s['direct_mixer'])
    if mixer(card)!=s['direct_mixer']: raise RuntimeError('Hardware controls changed during Direct activation')
    s['phase']='running'; s['ready_at']=time.time(); save(s)
    log('ready',rate=384000,card=card,codec=s['codec'],gain_unchanged=True)
    notify('READY=1\nSTATUS=AE-5 Direct stereo 384 kHz; existing PipeWeaver volume and profile')
    bad_since=None; repaired=False
    while not STOP:
        time.sleep(2)
        if STOP: break
        try: healthy=health(s)
        except (subprocess.TimeoutExpired,RuntimeError) as e:
            log('health-check',detail=str(e)); healthy=False
        if healthy:
            bad_since=None; repaired=False; continue
        bad_since=bad_since or time.monotonic()
        if not repaired and time.monotonic()-bad_since>=8:
            u=units(); missing=[k for k,v in u.items() if v['ActiveState'] in ('inactive','failed')]
            if missing: ctl('start','--no-block',*missing)
            else: ctl('restart','--no-block','pipeweaver.service')
            repaired=True; s['recoveries']+=1; save(s)
            log('reconnect-requested',missing=missing)
        if time.monotonic()-bad_since>=125: raise RuntimeError('Audio graph failed to recover; restoring ordinary audio')
    log('stop-requested')

def cleanup():
    if not STATE.exists():
        if attached(): raise RuntimeError('Attached Direct path without an ownership record; manual inspection required')
        return
    s=state()
    if s['phase']=='stopped' and not attached(): return
    s['phase']='stopping'; save(s)
    before=mixer(s['card'])
    # Preserve the user's latest levels and mutes, but restore the output
    # selection saved before Direct mode changed the hardware path.
    before['Output Select']=s['original_mixer']['Output Select']
    if user_available():
        ctl('stop','--no-block',*UNITS); wait_units(False,105)
    if pcm(s['card']).strip()!='closed': raise RuntimeError('PCM still open; retained module and configuration')
    if attached():
        n=native()
        for _ in range(2):
            if not n['needs_restore']: break
            try: native_command('recover')
            except OSError: pass
            n=native()
        if n['needs_restore']: raise RuntimeError('Native path restore failed; retained module and configuration')
        if n['enabled']: native_command('disable')
        n=native()
        if n['enabled'] or n['needs_restore']: raise RuntimeError('Direct disable did not complete')
    remove_dropins(s)
    if attached(): native_command('detach')
    shutting_down=command(['systemctl','is-system-running'],check=False)['stdout'].strip() in ('stopping','offline')
    if user_available() and not shutting_down:
        ctl('start','--no-block',*UNITS); wait_units(True,35)
        deadline=time.monotonic()+20
        while time.monotonic()<deadline:
            if pcm_matches(pcm(s['card']),s['original_pcm']): break
            time.sleep(.5)
        else: raise RuntimeError('Original hardware PCM was not restored')
    restore_mixer(s['card'],before)
    s.update(phase='stopped',stopped_at=time.time(),controls_preserved=True,dropins_removed=True,direct_detached=not attached())
    save(s); log('restored',rate=96000,controls_preserved=True,direct_detached=True)

def status():
    out={'installed':True,'supported':supported(),'service':command(['systemctl','show','ae5-direct.service','-p','ActiveState','-p','SubState','-p','Result'],check=False)['stdout']}
    if STATE.exists():
        s=state(); out.update(phase=s['phase'],card=s['card'],pcm=pcm(s['card']),recoveries=s.get('recoveries',0))
    out['native']=native() if (PARAMS/'status').exists() else None
    print(json.dumps(out,indent=2))

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action',choices=('run','cleanup','status','supported','prepare-sleep','resume'))
    args=parser.parse_args()
    if args.action=='supported': return 0 if supported() else 1
    if args.action=='status': status(); return 0
    if os.geteuid()!=0: raise RuntimeError('Root is required for driver lifecycle management')
    if args.action in ('prepare-sleep','resume'):
        marker=Path('/run/ae5-direct-resume')
        if args.action=='prepare-sleep':
            active=command(['systemctl','is-active','ae5-direct.service'],check=False)['stdout'].strip()=='active'
            if active:
                marker.write_text('resume\n'); marker.chmod(0o600)
                command(['systemctl','stop','ae5-direct.service'],timeout=180)
            if attached(): raise RuntimeError('AE-5 integration must be restored before system sleep')
        elif marker.exists():
            marker.unlink(); command(['systemctl','start','--no-block','ae5-direct.service'])
        return 0
    with open('/run/lock/ae5-direct.lock','a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX)
        if args.action=='run':
            signal.signal(signal.SIGTERM,stop_signal); signal.signal(signal.SIGINT,stop_signal)
            try: activate()
            except InterruptedError: pass
        else: cleanup()
    return 0

if __name__=='__main__':
    try: sys.exit(main())
    except Exception as exc:
        log('error',detail=str(exc)); sys.exit(1)
