import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
project=Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='sister-image-runtime-') as temp:
    root=Path(temp)
    with socket.socket() as s:
        s.bind(('127.0.0.1',0));port=s.getsockname()[1]
    binding=root/'binding.json'
    binding.write_text(json.dumps({'schema':'sister.infra.runtime.binding/1.0.0','components':[{'system_id':'sister_image','component_id':'image','runtime':{'transport':'tcp','listen':'127.0.0.1','port':port}}]}))
    env=dict(os.environ,SISTER_RUNTIME_MODE='dev-preview',SISTER_RUNTIME_INSTANCE_ID='runtime-test-instance',SISTER_RUNTIME_STATE_DIR=str(root/'state'),SISTER_RUNTIME_RUN_DIR=str(root/'run'),SISTER_RUNTIME_DATA_DIR=str(root/'data'),SISTER_RUNTIME_CLEANUP_SCOPE='preview-only',SISTER_RESOLVED_DEPLOYMENT_FILE=str(binding))
    def run(action,code=0):
        r=subprocess.run([str(project/'scripts/runtime.sh'),action],env=env,text=True,capture_output=True,timeout=20)
        assert r.returncode==code,(action,r.returncode,r.stdout,r.stderr)
        return r.stdout
    try:
        run('stop');run('start')
        first=json.loads((root/'run/pid.json').read_text())
        run('start')
        assert first==json.loads((root/'run/pid.json').read_text()),'start must be idempotent'
        assert json.loads(run('health'))['system_id']=='sister_image'
        assert json.loads(run('readiness'))['status']=='ready'
        assert (root/'data/manifest.json').exists()
        assert json.loads((root/'data/manifest.json').read_text())['transport']['internal_endpoint'].endswith(':'+str(port))
        run('stop');run('stop');run('status',3)
        # An unrelated live PID must never be signaled.
        (root/'run/pid.json').write_text(json.dumps({'pid':os.getpid(),'start':'wrong','instance':'runtime-test-instance'}))
        run('stop',1)
        (root/'run/pid.json').unlink()
    finally:
        run('stop')
print('Runtime binding, isolated persistent state, idempotency and foreign PID refusal passed')
