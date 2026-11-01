"""Native file/directory selection requested by a click in the Manager UI."""
import json
import subprocess
import threading

LOCK=threading.Lock()
SCRIPT=r'''
[Console]::InputEncoding=[Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
$request=[Console]::ReadLine() | ConvertFrom-Json
Add-Type -AssemblyName System.Windows.Forms
$owner=New-Object System.Windows.Forms.Form
$owner.TopMost=$true;$owner.ShowInTaskbar=$false;$owner.Opacity=0;$owner.Show()
try {
 if($request.kind -eq 'directory'){$dialog=New-Object System.Windows.Forms.FolderBrowserDialog}
 else {$dialog=New-Object System.Windows.Forms.OpenFileDialog;$dialog.Filter='Supported files|*.ini;*.json;*.dll;*.zip|All files|*.*'}
 try {
  $result=$dialog.ShowDialog($owner)
  $value=$null
  if($result -eq [System.Windows.Forms.DialogResult]::OK){if($request.kind -eq 'directory'){$value=$dialog.SelectedPath}else{$value=$dialog.FileName}}
  @{path=$value} | ConvertTo-Json -Compress
 }finally{$dialog.Dispose()}
}finally{$owner.Close();$owner.Dispose()}
'''


def choose(kind):
    if kind not in ('directory','file'):raise ValueError('选择器类型无效')
    if not LOCK.acquire(blocking=False):raise ValueError('已有一个路径选择窗口，请先完成或取消')
    try:
        result=subprocess.run(['powershell.exe','-NoProfile','-STA','-NonInteractive','-Command',SCRIPT],
            input=json.dumps({'kind':kind}),capture_output=True,text=True,encoding='utf-8',timeout=300,
            creationflags=subprocess.CREATE_NO_WINDOW)
        if result.returncode:raise ValueError('路径选择窗口未完成')
        return json.loads(result.stdout)
    finally:LOCK.release()
