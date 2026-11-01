"""One Manager process per local workspace, even after repeated EXE launches."""
import json
import os
from pathlib import Path
import re
import urllib.request
from . import storage


class Lease:
    def __init__(self,root):
        self.root=Path(root).resolve();self.root.mkdir(parents=True,exist_ok=True)
        self.file=(self.root/'manager-instance.lock').open('a+b')
        try:
            self.file.seek(0,2)
            if not self.file.tell():self.file.write(b'\0');self.file.flush()
            self.file.seek(0)
            if os.name=='nt':
                import msvcrt
                msvcrt.locking(self.file.fileno(),msvcrt.LK_NBLCK,1)
            else:
                import fcntl
                fcntl.flock(self.file.fileno(),fcntl.LOCK_EX|fcntl.LOCK_NB)
        except OSError:
            self.file.close();self.file=None

    def close(self):
        if self.file:self.file.close();self.file=None

    def existing_url(self):
        value=storage.read_json(self.root/'manager-session.json',{})
        url=value.get('url','')
        if not re.fullmatch(r'http://127\.0\.0\.1:[0-9]+/',url):raise ValueError('此工作区已有 Manager 进程，但会话地址无法核对')
        with urllib.request.urlopen(url+'api/identity',timeout=3) as response:identity=json.load(response)
        if identity.get('application')!='sdgo_mod_manager' or identity.get('workspace_id')!=storage.revision(str(self.root).encode()):
            raise ValueError('现有端口不属于这个 Manager 工作区')
        return url
