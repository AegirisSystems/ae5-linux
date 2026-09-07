#!/usr/bin/python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Narrow desktop authorization entry point for the configured Direct service."""
from pathlib import Path
import json,os,subprocess,sys

def main():
    if len(sys.argv)!=2 or sys.argv[1] not in ('start','stop'):
        raise SystemExit('Expected start or stop.')
    if os.geteuid()!=0:raise SystemExit('System authorization is required.')
    cfg=json.loads(Path('/usr/lib/ae5-direct/installation.json').read_text())
    actor=os.environ.get('PKEXEC_UID') or os.environ.get('SUDO_UID')
    if actor is None or int(actor)!=cfg['uid']:
        raise SystemExit('Only the configured desktop user can request this transition.')
    return subprocess.run(['/usr/bin/systemctl',sys.argv[1],'ae5-direct.service'],timeout=560).returncode

if __name__=='__main__':raise SystemExit(main())
