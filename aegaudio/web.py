# SPDX-License-Identifier: GPL-2.0-or-later
"""AE-5-only browser control panel. Opening the page only reads audio state."""
import argparse
import datetime
import json
import secrets
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse

from . import backend

ASSETS = Path(__file__).with_name('web_assets')
IDENTITY = ('0x11020011', '0x11020051')
LOCK = threading.RLock()


def ae5():
    found = [c for c in backend.cards() if (c['vendor'], c['subsystem']) == IDENTITY]
    if len(found) != 1:
        raise ValueError('Expected one supported AE-5. No other audio card will be selected.')
    return found[0]


def section(c):
    n = c['name']
    if c['iface'] != 'MIXER':
        return 'status'
    if 'Equalizer' in n or n.startswith('EQ Band'):
        return 'equalizer'
    if 'Capture' in n or n == 'Input Source':
        return 'recording'
    if n.startswith(('FX:', 'Enable OutFX')):
        return 'sbx'
    if n in backend.PROTECTED or n.startswith(('AE-5:', 'Bass Redirection', 'Full-Range')):
        return 'playback'
    return 'mixer'


def control_lock(c, direct):
    if c['iface'] != 'MIXER' or not c['writable'] or c['type'] not in ('BOOLEAN', 'INTEGER', 'ENUMERATED'):
        return 'Information only'
    if direct and (c['name'] in backend.PROTECTED or section(c) in ('sbx', 'equalizer') or
                   c['name'].startswith(('FX:', 'Enable InFX', 'VoiceFX'))):
        return 'Unavailable while Direct playback is active'
    return ''


def current_state():
    card = ae5()
    controls = backend.controls(card)
    state = backend.state(card, controls)
    for c in controls:
        c['section'] = section(c)
        c['locked'] = control_lock(c, state['direct'])
        c['protected'] = c['name'] in backend.PROTECTED
    return dict(card=card, controls=controls, state=state,
                captured_at=datetime.datetime.now(datetime.timezone.utc).isoformat())


class Handler(BaseHTTPRequestHandler):
    server_version = 'AegAudioCommand/0.2'

    def log_message(self, *args):
        pass

    def send(self, data, status=200, mime='application/json'):
        if not isinstance(data, bytes):
            data = json.dumps(data).encode()
        self.send_response(status)
        self.send_header('Content-Type', mime)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.send_header('Referrer-Policy', 'no-referrer')
        self.send_header('Content-Security-Policy', "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'")
        self.end_headers()
        self.wfile.write(data)

    def record(self, kind, **details):
        entry = dict(time=datetime.datetime.now(datetime.timezone.utc).isoformat(), kind=kind, **details)
        with LOCK, (self.server.data_dir / 'history.jsonl').open('a', encoding='utf-8') as f:
            f.write(json.dumps(entry) + '\n')

    def history(self):
        path = self.server.data_dir / 'history.jsonl'
        if not path.exists():
            return []
        from collections import deque
        with path.open(encoding='utf-8') as f:
            return [json.loads(line) for line in deque(f, maxlen=100) if line.strip()]

    def do_GET(self):
        path = urlparse(self.path).path
        try:
            assets = {'/': ('index.html', 'text/html; charset=utf-8'),
                      '/app.js': ('app.js', 'text/javascript; charset=utf-8'),
                      '/style.css': ('style.css', 'text/css; charset=utf-8'),
                      '/favicon.svg': ('favicon.svg', 'image/svg+xml'),
                      '/profile-scenes.png': ('profile-scenes.png', 'image/png')}
            if path in assets:
                name, mime = assets[path]
                return self.send((ASSETS / name).read_bytes(), mime=mime)
            if path == '/api/state':
                with LOCK:
                    data = current_state()
                data['csrf'] = self.server.csrf
                return self.send(data)
            if path == '/api/history':
                return self.send(self.history())
            if path == '/api/profile':
                with LOCK:
                    card = ae5()
                    valid, omitted = [], []
                    for c in backend.controls(card):
                        try:
                            backend.validate(c, c['values'])
                            valid.append(c)
                        except ValueError:
                            omitted.append(c['name'])
                    profile = backend.profile(card, valid)
                    profile['omitted_unwritable_or_invalid'] = omitted
                    return self.send(profile)
            return self.send({'error': 'Not found'}, 404)
        except (ValueError, RuntimeError, OSError) as exc:
            self.send({'error': str(exc)}, 503)

    def body(self):
        if self.headers.get('Content-Type', '').split(';')[0] != 'application/json':
            raise ValueError('JSON request required')
        if not secrets.compare_digest(self.headers.get('X-AegAudio-Token', ''), self.server.csrf):
            raise ValueError('Session changed. Refresh the page before applying a change.')
        origin = self.headers.get('Origin')
        if origin and origin != 'http://' + self.headers.get('Host', ''):
            raise ValueError('Cross-origin audio changes are not accepted')
        n = int(self.headers.get('Content-Length', 0))
        if not 0 < n <= 1_000_000:
            raise ValueError('Invalid request size')
        body = json.loads(self.rfile.read(n))
        if not isinstance(body, dict):
            raise ValueError('Expected a JSON object')
        return body

    def check_device(self, b, card):
        if b.get('device_path') != card['device_path']:
            raise ValueError('Device identity changed. Refresh the page.')
        if 'card' in b and b['card'] != card['index']:
            raise ValueError('This panel controls only the detected AE-5.')

    def do_POST(self):
        path = urlparse(self.path).path
        try:
            b = self.body()
            with LOCK:
                card = ae5()
                self.check_device(b, card)
                if path == '/api/set':
                    matches = [c for c in backend.controls(card) if c['key'] == b.get('key')]
                    if len(matches) != 1:
                        raise ValueError('The control no longer exists. Refresh the page.')
                    c = matches[0]
                    reason = control_lock(c, backend.state(card)['direct'])
                    if reason:
                        raise ValueError(reason)
                    if c['values'] != b.get('expected'):
                        raise ValueError('This setting changed elsewhere. Refresh before applying.')
                    values = b.get('values')
                    if not isinstance(values, list) or any(not isinstance(v, str) for v in values):
                        raise ValueError('Expected a list of control values')
                    readback = backend.set_control(card, c, values, allow_protected=b.get('confirmed') is True)
                    self.record('control', control=c['name'], before=c['values'], after=readback['values'], result='ALSA readback matched')
                    return self.send({'ok': True, 'control': readback})
                if path == '/api/profile/preview':
                    plan = backend.profile_plan(card, b['profile'])
                    return self.send({'changes': [{'key': c['key'], 'name': c['name'], 'before': c['values'], 'after': v} for c, v in plan]})
                if path == '/api/profile/apply':
                    if backend.state(card)['direct']:
                        raise ValueError('Processing profiles cannot be applied during Direct playback.')
                    plan = backend.profile_plan(card, b['profile'])
                    view = [{'key': c['key'], 'name': c['name'], 'before': c['values'], 'after': v} for c, v in plan]
                    if b.get('reviewed') != view:
                        raise ValueError('Settings changed after preview. Review the profile again.')
                    for c, _ in plan:
                        reason = control_lock(c, False)
                        if reason:
                            raise ValueError(reason)
                    self.record('profile_attempt', changes=view)
                    backend.apply_profile(card, plan)
                    self.record('profile', changes=view, result='ALSA readbacks matched')
                    return self.send({'ok': True})
                if path == '/api/note':
                    note = b.get('note', '')
                    result = b.get('result', '')
                    if not isinstance(note, str) or not 1 <= len(note.strip()) <= 4000:
                        raise ValueError('Enter a note of 1 to 4000 characters')
                    if result not in ('Heard as expected', 'No audible change', 'Silent', 'Distorted', 'Other'):
                        raise ValueError('Select an observation')
                    self.record('listening_note', result=result, note=note.strip())
                    return self.send({'ok': True})
            self.send({'error': 'Not found'}, 404)
        except (ValueError, RuntimeError, OSError, KeyError, TypeError) as exc:
            self.record('error', action=path, detail=str(exc))
            self.send({'error': str(exc)}, 400)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=8770)
    parser.add_argument('--data-dir', type=Path, default=Path.home() / '.local/share/aegaudio-command')
    args = parser.parse_args()
    args.data_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.data_dir = args.data_dir
    server.csrf = secrets.token_urlsafe(32)
    print(f'AegAudio Command on http://{args.host}:{args.port}', flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == '__main__':
    main()
