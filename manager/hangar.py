"""Owner installation only; players act on the native inventory page."""
from .providers import Provider
from .engines.hangar.server import Server
from . import storage


class Hangar(Provider):
    id,title='hangar','机库整理与批量回收服务端'
    fields={'server_ini':'目标原生服务器 INI（需含 DB Game、DB Member、DB StoreBilling）'}

    def revision(self):return storage.revision(storage.json_bytes(self.config))

    def request(self,method,path,body,headers):
        revision=self.revision()
        if method=='GET' and path=='/api/state':
            state=Server(self.config['server_ini']).status() if self.config.get('server_ini') else {'installed':False,'activation':'configure_server'}
            return {**state,'revision':revision}
        if method=='POST' and path in ('/api/install','/api/remove'):
            if body.get('revision')!=revision:raise storage.Conflict('机库服务端配置已改变，请刷新')
            if not self.config.get('server_ini'):raise ValueError('请先选择原生服务器 INI')
            server=Server(self.config['server_ini'])
            try:
                result=server.install() if path.endswith('/install') else server.remove()
            except Exception as error:
                import pyodbc
                if isinstance(error,pyodbc.Error):
                    raise ValueError('服务端排序安装未完成（SQLSTATE '+str(error.args[0])[:5]+'），本次更改已回退') from None
                raise
            return {**result,'revision':revision}
        raise ValueError('机库操作未登记')
