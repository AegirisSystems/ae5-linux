#!/usr/bin/python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepare a deployment explicitly. Never starts/stops audio or changes a mixer."""
from pathlib import Path
import argparse,datetime,hashlib,json,os,pwd,re,shutil,subprocess

BASE=Path('/usr/lib/ae5-direct')
KERNEL='7.0.0-31-generic'
def run(args):return subprocess.check_output(args,text=True).strip()
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--user',required=True,help='Existing desktop audio account')
    ap.add_argument('--pci',help='Select among multiple compatible cards, e.g. 0000:06:00.0')
    ap.add_argument('--apply',action='store_true',help='Write the reviewed deployment files; no live activation')
    ap.add_argument('--enable-on-boot',action='store_true',help='Also enable the Direct/sleep system units; does not start them')
    a=ap.parse_args()
    if os.geteuid()!=0:ap.error('Run through sudo; the user named by --user remains the playback owner.')
    if a.enable_on_boot and not a.apply:ap.error('--enable-on-boot requires --apply')
    account=pwd.getpwnam(a.user)
    if account.pw_uid==0:ap.error('Select a non-root desktop audio account.')
    if run(['uname','-r'])!=KERNEL:ap.error('This release supports only '+KERNEL)
    module=Path('/lib/modules')/KERNEL/'updates/ae5/snd-hda-codec-ca0132.ko'
    if not module.exists():ap.error('Install the matching driver package first.')
    candidates=[]
    for p in Path('/sys/bus/hdaudio/devices').glob('hdaudioC*D*'):
        if (p/'vendor_id').read_text().strip().lower()!='0x11020011':continue
        if (p/'subsystem_id').read_text().strip().lower()!='0x11020051':continue
        pci=[x for x in p.resolve().parts if re.fullmatch(r'[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]',x)]
        if pci and (not a.pci or pci[-1]==a.pci):candidates.append((p,pci[-1]))
    if len(candidates)!=1:ap.error('Exactly one supported AE-5 SSID 1102:0051 is required; use --pci if ambiguous.')
    codec,pci=candidates[0];sink='alsa_output.pci-'+pci.replace(':','_')+'.analog-stereo'
    stock=list((Path('/lib/modules')/KERNEL/'kernel/sound/hda/codecs').glob('snd-hda-codec-ca0132.ko*'))
    if len(stock)!=1:ap.error('Stock module does not resolve uniquely.')
    for cmd in ('pipeweaver-daemon','pipeweaver-client','pw-dump','amixer'):
        if not shutil.which(cmd):ap.error('Required command missing: '+cmd)
    if not Path('/usr/lib/firmware/ctefx-desktop.bin').exists() and not Path('/usr/lib/firmware/ctefx-desktop.bin.zst').exists():
        ap.error('Install ALSA desktop DSP firmware first; see installation instructions.')
    config=dict(user=a.user,uid=account.pw_uid,home=account.pw_dir,pci=pci,
        vendor_id='0x11020011',subsystem_id='0x11020051',kernel=KERNEL,module_path=str(module),
        module_sha256=sha(module),native_srcversion=run(['modinfo','-F','srcversion',str(module)]),
        stock_module=str(stock[0]),stock_module_sha256=sha(stock[0]),sink=sink,version='0.16',
        output_select=1,unmute_front_on_start=True)
    userunits=Path(account.pw_dir)/'.config/systemd/user'
    pipeweaver=userunits/'pipeweaver.service'
    unit=(BASE/'ae5-direct.service.in').read_text().replace('@UID@',str(account.pw_uid))
    writes={BASE/'installation.json':json.dumps(config,indent=2)+'\n',
            Path('/etc/systemd/system/ae5-direct.service'):unit}
    if not pipeweaver.exists():writes[pipeweaver]=(BASE/'pipeweaver.service').read_text()
    print(json.dumps({'configuration':config,'files_to_write':[str(p) for p in writes],
          'enable_on_boot':a.enable_on_boot,'audio_restart':False,'mixer_writes':False},indent=2))
    if not a.apply:return
    backup=Path('/var/lib/ae5-direct/backups')/datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    backup.mkdir(parents=True,mode=0o700)
    for p,data in writes.items():
        if p.is_symlink():raise RuntimeError('Refusing to replace a symlink: '+str(p))
        if p.exists():
            dest=backup/p.as_posix().lstrip('/');dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,dest)
        missing=[];parent=p.parent
        while not parent.exists():missing.append(parent);parent=parent.parent
        p.parent.mkdir(parents=True,exist_ok=True)
        p.write_text(data);p.chmod(0o644)
        if p==pipeweaver:
            for d in missing:os.chown(d,account.pw_uid,account.pw_gid)
            os.chown(p,account.pw_uid,account.pw_gid)
    # Activation, user daemon reload, and graph setup remain explicit user steps.
    run(['systemctl','daemon-reload'])
    if a.enable_on_boot:run(['systemctl','enable','ae5-direct.service','ae5-direct-sleep.service'])
    print('Deployment files written. Audio services were not restarted and mixers were not changed.')
    print('Backup: '+str(backup))

if __name__=='__main__':main()
