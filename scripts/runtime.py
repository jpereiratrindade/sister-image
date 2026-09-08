#!/usr/bin/env python3
"""Installed lifecycle adapter. No builds, deployment mutation or shared PID files."""
import fcntl
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
import urllib.request

# Ensure local loopback requests bypass any environment HTTP proxy (e.g., Squid)
urllib.request.install_opener(urllib.request.build_opener(urllib.request.ProxyHandler({})))

project = Path(__file__).resolve().parents[1]
env = os.environ.copy()
env.update({
    'http_proxy': '',
    'https_proxy': '',
    'HTTP_PROXY': '',
    'HTTPS_PROXY': '',
    'no_proxy': '127.0.0.1,localhost',
    'NO_PROXY': '127.0.0.1,localhost'
})

mode = env.get('SISTER_RUNTIME_MODE', '')
instance = env.get('SISTER_RUNTIME_INSTANCE_ID', '')
if mode == 'dev-preview':
    for key in ('SISTER_RUNTIME_INSTANCE_ID', 'SISTER_RUNTIME_STATE_DIR', 'SISTER_RUNTIME_RUN_DIR', 'SISTER_RUNTIME_DATA_DIR'):
        if not env.get(key):
            raise SystemExit(f'Marcador DEV ausente: {key}')
    if env.get('SISTER_RUNTIME_CLEANUP_SCOPE') != 'preview-only':
        raise SystemExit('Escopo DEV invalido')
    env['SISTER_IMAGE_ACCESS_MODE'] = 'local'
state = Path(env.get('SISTER_RUNTIME_STATE_DIR', str(Path.home() / '.local/state/sister-image'))).resolve()
run = Path(env.get('SISTER_RUNTIME_RUN_DIR', str(state / 'run'))).resolve()
data = Path(env.get('SISTER_RUNTIME_DATA_DIR', str(state / 'data'))).resolve()
for p in (state, run, data):
    p.mkdir(parents=True, exist_ok=True)
env.update(SISTER_RUNTIME_STATE_DIR=str(state), SISTER_RUNTIME_RUN_DIR=str(run), SISTER_RUNTIME_DATA_DIR=str(data))
resolved = env.get('SISTER_RESOLVED_DEPLOYMENT_FILE')
if resolved:
    declaration = json.loads(Path(resolved).read_text())
    matches = [c for c in declaration['components'] if c['system_id'] == 'sister_image']
    if len(matches) != 1:
        raise SystemExit('Binding sister_image ausente ou ambiguo')
    binding = matches[0]['runtime']
    if binding['transport'] != 'tcp':
        raise SystemExit('Binding requer TCP')
    address, port = binding['listen'], int(binding['port'])
else:
    address, port = '127.0.0.1', int(env.get('SISTER_IMAGE_PORT', '8096'))
if address != '127.0.0.1' or not 1024 <= port <= 65535:
    raise SystemExit('Use porta 1024..65535 em loopback; gateway pertence ao Infra')
binary = project / 'build/sister-image-http'
args = [str(binary), '--bind', address, '--port', str(port), '--data', str(data), '--web', str(project / 'web')]
pidfile = run / 'pid.json'

def process_identity(pid):
    try:
        proc = Path('/proc') / str(pid)
        stat = (proc / 'stat').read_text().rsplit(')', 1)[1].split()
        if stat[0] == 'Z':
            return None
        return {'start': stat[19], 'exe': str((proc / 'exe').resolve()).removesuffix(' (deleted)'),
                'args': (proc / 'cmdline').read_bytes().split(b'\0')[:-1]}
    except (OSError, IndexError):
        return None

def running():
    if not pidfile.exists():
        return None
    saved = json.loads(pidfile.read_text())
    observed = process_identity(saved['pid'])
    if observed is None:
        pidfile.unlink(missing_ok=True)
        return None
    if observed['start'] != saved['start'] or observed['exe'] != str(binary) or observed['args'] != [a.encode() for a in args] or saved['instance'] != instance:
        raise RuntimeError('PID nao pertence a esta instancia; operacao recusada')
    return saved['pid']

def observation(path):
    with urllib.request.urlopen(f'http://{address}:{port}{path}', timeout=3) as r:
        body = json.load(r)
    if body.get('system_id') != 'sister_image':
        raise RuntimeError('Identidade de runtime divergente')
    return body

def stop():
    pid = running()
    if pid:
        os.kill(pid, signal.SIGTERM)
        for _ in range(100):
            if not process_identity(pid):
                break
            time.sleep(.1)
        else:
            if running() == pid:
                os.kill(pid, signal.SIGKILL)
        pidfile.unlink(missing_ok=True)
    print('stopped')

def start():
    if running():
        print('running')
        return
    if not binary.is_file():
        raise RuntimeError('Artefato ausente; compile ou qualifique antes de start')
    with (run / 'runtime.log').open('ab') as log:
        child = subprocess.Popen(args, env=env, stdout=log, stderr=log, start_new_session=True)
    observed = None
    for _ in range(30):
        observed = process_identity(child.pid)
        if observed and observed['exe'] == str(binary):
            break
        if child.poll() is not None:
            raise RuntimeError('Runtime encerrou; consulte runtime.log')
        time.sleep(.05)
    if not observed or observed['exe'] != str(binary):
        child.terminate()
        raise RuntimeError('Nao foi possivel comprovar identidade do runtime')
    pidfile.write_text(json.dumps({'pid':child.pid,'start':observed['start'],'instance':instance}))
    for _ in range(60):
        if child.poll() is not None:
            pidfile.unlink(missing_ok=True)
            raise RuntimeError('Runtime encerrou; consulte runtime.log')
        try:
            if observation('/ready')['status'] == 'ready':
                print(f'running pid={child.pid} http://{address}:{port}')
                return
        except Exception:
            pass
        time.sleep(.1)
    stop()
    raise RuntimeError('Runtime nao ficou pronto')

try:
    with (run / 'lifecycle.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        action = sys.argv[1] if len(sys.argv) > 1 else 'run'
        if action == 'start': start()
        elif action == 'stop': stop()
        elif action == 'restart': stop(); start()
        elif action == 'status':
            pid = running()
            print(f'running pid={pid}' if pid else 'stopped')
            sys.exit(0 if pid else 3)
        elif action in ('health', 'readiness'):
            if not running(): raise RuntimeError('Runtime parado')
            result = observation('/health' if action == 'health' else '/ready')
            print(json.dumps(result))
            sys.exit(0 if result['status'] in ('ok','ready') else 1)
        elif action == 'run':
            os.execve(binary, args, env)
        else: raise RuntimeError('Acao desconhecida')
except Exception as exc:
    print(str(exc), file=sys.stderr)
    sys.exit(1)
