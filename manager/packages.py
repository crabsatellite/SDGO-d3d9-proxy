"""Import allowlisted client overlays without executing package scripts."""
from __future__ import annotations
import base64
import io
import json
from pathlib import Path
import re
import zipfile
from . import storage

ALLOWED = {'.ini','.json','.bin','.dll','.spr','.txr','.png','.lua'}


def _running(executable):
    if __import__('os').name != 'nt': return False
    # Read-only process inventory. Never terminate a game to import a package.
    import subprocess
    script = 'Get-CimInstance Win32_Process -Filter "Name=\'GOnline.exe\'" | Select-Object -ExpandProperty ExecutablePath | ConvertTo-Json -Compress'
    result = subprocess.run(['powershell.exe','-NoProfile','-NonInteractive','-Command',script],capture_output=True,text=True,encoding='utf-8',creationflags=subprocess.CREATE_NO_WINDOW)
    if result.returncode: raise ValueError('无法确认客户端是否已关闭')
    values=json.loads(result.stdout) if result.stdout.strip() else []
    if isinstance(values,str):values=[values]
    return any(Path(value).resolve()==executable.resolve() for value in values if value)


def import_client(workspace, host_id, encoded):
    host=workspace.host(host_id)
    if host['role']!='client': raise ValueError('玩家配置包只能安装到客户端')
    root=Path(host['path']).resolve()
    if _running(root/'GOnline.exe'): raise ValueError('请先关闭这个客户端，再导入配置包')
    if not isinstance(encoded,str) or len(encoded)>96*1024*1024:raise ValueError('客户端配置包过大')
    raw=base64.b64decode(encoded,validate=True)
    pending={};total=0
    with zipfile.ZipFile(io.BytesIO(raw)) as archive:
        if len(archive.infolist())>4096:raise ValueError('配置包条目过多')
        for entry in archive.infolist():
            if entry.is_dir():continue
            name=entry.filename.replace('\\','/')
            if name.startswith('client/'):name=name[7:]
            if not name.startswith('sdgo/'):
                # A client package may carry readable notes and its fingerprint manifest.
                if '/' not in name and (name.lower().startswith('readme') or name in ('manifest.json','registrations.json','symbols.fragment.ini','Install-Operator.ps1','install_ai_catalog_client.ps1')):continue
                raise ValueError('配置包包含客户端以外的文件：'+name)
            target=storage.safe_child(root,name)
            parts=name.split('/')
            if len(parts)<4 or parts[1]!='mods' or not re.fullmatch(r'[a-z][a-z0-9_]{0,63}',parts[2]):
                raise ValueError('此配置包路径未登记：'+name)
            if target.suffix.lower() not in ALLOWED or entry.file_size>32*1024*1024:raise ValueError('此配置包文件类型或大小不受支持：'+name)
            total+=entry.file_size
            if total>96*1024*1024:raise ValueError('配置包解压总量过大')
            if target in pending:raise ValueError('配置包文件重复')
            data=archive.read(entry)
            if target.name=='mod.ini':
                ini=storage.ini_values(data)
                if ini.get('mod','id',fallback='')!=parts[2] or ini.get('mod','dll',fallback=parts[2]+'.dll')!=parts[2]+'.dll':raise ValueError('Mod 身份或 DLL 路径无效')
                if any(not h.startswith('GOnline-') for h in ini.get('mod','hosts',fallback='').split(',')):raise ValueError('包内 Mod 不属于客户端')
            if target.suffix.lower()=='.dll' and target.name!=parts[2]+'.dll':raise ValueError('Mod DLL 名称无效')
            pending[target]=data
        if not pending:raise ValueError('配置包没有客户端 Mod 文件')
        # Merge known symbol fragments as data, never run supplied setup scripts.
        if 'symbols.fragment.ini' in archive.namelist():
            fragment=storage.ini_values(archive.read('symbols.fragment.ini'))
            stamp=int(fragment.get('host','timestamp'),0);size=int(fragment.get('host','size_of_image'),0)
            name=f'GOnline-{stamp:08X}-{size:08X}.ini';path=root/'sdgo/symbols'/name
            original=path.read_bytes() if path.exists() else b'';merged=storage.ini_values(original)
            for section in fragment.sections():
                if merged.has_section(section):
                    if dict(merged[section])!=dict(fragment[section]):raise ValueError('符号定义存在冲突：'+section)
                else:merged[section]=dict(fragment[section])
            text=io.StringIO();merged.write(text,space_around_delimiters=False);pending[path]=text.getvalue().encode('ascii')
    with storage.LOCK:
        previous={p:(p.read_bytes() if p.exists() else None) for p in pending};written=[]
        try:
            for path,data in pending.items():storage.atomic(path,data);written.append(path)
        except OSError:
            for path in reversed(written):
                if previous[path] is None:path.unlink(missing_ok=True)
                else:storage.atomic(path,previous[path])
            raise
    return {'installed_files':len(pending),'activation':'restart_required','state':workspace.public()}
