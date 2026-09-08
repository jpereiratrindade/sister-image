import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
import urllib.error
import jsonschema

binary, project = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()

def request(base, path, method='GET', data=None, headers=None, expected=200):
    try:
        r = urllib.request.urlopen(urllib.request.Request(base+path,data=data,method=method,headers=headers or {}),timeout=10)
    except urllib.error.HTTPError as error:
        r = error
    body = r.read()
    assert r.status == expected, (path,r.status,body)
    return json.loads(body) if 'application/json' in r.headers.get('Content-Type','') else body

def schema(name, body):
    jsonschema.Draft202012Validator(json.loads((project/'contracts/subsystem'/f'{name}.schema.json').read_text()), format_checker=jsonschema.FormatChecker()).validate(body)

with tempfile.TemporaryDirectory(prefix='sister-image-http-') as temp:
    root = Path(temp)
    for access in ('local','proxy'):
        with socket.socket() as sock:
            sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
        base=f'http://127.0.0.1:{port}'
        token='test-only-proxy-token-0123456789abcdef'
        env=dict(os.environ,SISTER_IMAGE_ACCESS_MODE=access,SISTER_IMAGE_PROXY_TOKEN=token)
        with (root/'server.log').open('w') as log:
            proc=subprocess.Popen([str(binary),'--port',str(port),'--data',str(root/access),'--web',str(project/'web')],env=env,stdout=log,stderr=log)
            try:
                for _ in range(100):
                    if proc.poll() is not None:raise AssertionError((root/'server.log').read_text())
                    try:request(base,'/health');break
                    except OSError:time.sleep(.05)
                for name,path in [('health','/health'),('readiness','/ready'),('manifest','/manifest'),('capabilities','/capabilities')]:schema(name,request(base,path))
                assert b'SisTer Image' in request(base,'/')
                schema('error',request(base,'/identity',expected=401))
                headers={'X-Sister-Proxy-Token':token}
                identity=dict(headers,**{'X-Sister-Subject':'researcher','X-Sister-Name':'Researcher','X-Sister-Email':'r@example.test','X-Sister-Role':'research','X-Request-ID':'request-test-123456789'})
                schema('identity',request(base,'/identity',headers=identity))
                schema('echo',request(base,'/echo','POST',b'{"value":"image"}',headers))
                if access=='proxy':request(base,'/api/jobs',expected=401)
                request(base,'/api/demo','POST',b'',dict(headers,Origin='https://untrusted.invalid'),403)
                request(base,'/api/demo?window=-1','POST',b'',headers,400)
                request(base,'/api/demo','POST',b'x'*70001,headers,413)
                job=request(base,'/api/demo?window=32&classes=3','POST',b'',headers,202)
                jsonschema.validate(job,json.loads((project/'contracts/job.schema.json').read_text()))
                for _ in range(100):
                    done=request(base,'/api/jobs/'+job['id'],headers=headers)
                    if done['status'] in ('completed','failed'):break
                    time.sleep(.05)
                assert done['status']=='completed',done
                report=request(base,f"/api/jobs/{job['id']}/report.json",headers=headers)
                data=request(base,f"/api/jobs/{job['id']}/map.tif",headers=headers)
                assert report['output_digest']=='sha256:'+hashlib.sha256(data).hexdigest()
                assert request(base,f"/api/jobs/{job['id']}/preview.pgm",headers=headers).startswith(b'P5\n')
                # Streaming upload round-trip, using a real TIFF as input.
                uploaded=request(base,'/api/classify?window=32&mode=patches','POST',data,dict(headers,**{'Content-Type':'image/tiff'}),202)
                for _ in range(100):
                    state=request(base,'/api/jobs/'+uploaded['id'],headers=headers)
                    if state['status'] in ('completed','failed'):break
                    time.sleep(.05)
                assert state['status']=='completed',state
                bad=request(base,'/api/classify','POST',b'not-a-tiff-file',dict(headers,**{'Content-Type':'image/tiff'}),202)
                for _ in range(100):
                    state=request(base,'/api/jobs/'+bad['id'],headers=headers)
                    if state['status']=='failed':break
                    time.sleep(.05)
                assert state['status']=='failed'
                request(base,f"/api/jobs/{job['id']}",'DELETE',headers=headers)
                request(base,f"/api/jobs/{job['id']}",headers=headers,expected=404)
            finally:
                proc.terminate()
                try:proc.wait(timeout=10)
                except subprocess.TimeoutExpired:proc.kill();proc.wait();raise
            assert proc.returncode==0,(proc.returncode,(root/'server.log').read_text())
print('HTTP contracts, classification, streaming upload, evidence, auth and shutdown passed')
