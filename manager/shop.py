"""Headless domain bridge; the retired shop EXE is never launched or needed."""
from __future__ import annotations
import json
from pathlib import Path
import subprocess
import threading
from .providers import Provider, Binary
from . import storage


class Bridge:
    def __init__(self):
        here = Path(__file__).parent / 'engines/shop'
        library = here / 'SdgoShopServices.dll'
        if not library.exists(): library = Path(__file__).resolve().parents[1] / 'build/manager-services/SdgoShopServices.dll'
        if not library.exists(): raise ValueError('请先构建 Manager 的商城业务程序集')
        self.process = subprocess.Popen(['powershell.exe','-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass',
            '-File',str(here / 'bridge.ps1'),'-Library',str(library)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, encoding='utf-8', creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        self.lock = threading.RLock()

    def call(self, value):
        with self.lock:
            if self.process.poll() is not None: raise ValueError('商城业务进程已退出，请刷新工作区')
            self.process.stdin.write(json.dumps(value,ensure_ascii=False)+'\n');self.process.stdin.flush()
            line = self.process.stdout.readline()
            if not line: raise ValueError('商城业务进程没有返回结果')
            result = json.loads(line)
            if not result.get('ok'): raise ValueError(result.get('error','商城操作未完成'))
            return result['value']

    def close(self):
        if self.process.poll() is None:
            self.process.stdin.close()
            try: self.process.wait(timeout=5)
            except subprocess.TimeoutExpired: self.process.terminate()


class Shop(Provider):
    id, title = 'shop', '商城商品'
    fields = {'workspace':'现有商城配置目录','world':'WorldServer 目录','client':'客户端目录（显示标签更新）','demo':'演示模式（仅测试时填 1）'}
    def request(self, method, path, body, headers):
        if not self.config.get('workspace'): raise ValueError('请先选择商城配置工作区')
        if self.engine is None: self.engine=Bridge()
        action = 'read' if method=='GET' else path.rsplit('/',1)[-1]
        value=self.engine.call({**self.config,'action':action,'body':body,'revision':headers.get('X-Shop-Revision','')})
        if method=='GET' and path.endswith('/export'): return Binary(storage.json_bytes(value['catalog']),'application/json','shop.json')
        return value
