"""Manager-owned background capabilities; never start or stop game processes."""
import threading
from pathlib import Path


class WalletService:
    def __init__(self):
        self.thread = None
        self.stop_event = threading.Event()
        self.error = ''
        self.healthy = False

    def state(self):
        return {'running': bool(self.thread and self.thread.is_alive()), 'healthy': bool(self.thread and self.thread.is_alive() and self.healthy), 'error': self.error,
                'ownership': 'manager_process', 'delivery': 'native_MB_ledger'}

    def start(self, ini, runtime):
        if self.thread and self.thread.is_alive(): return self.state()
        if not ini or not Path(ini).is_file(): raise ValueError('请先选择包含 DB StoreBilling 的原生服务器 INI')
        runtime = Path(runtime)
        if runtime.is_symlink(): raise ValueError('运行目录不能是目录链接')
        from .storage import ini_values
        import time
        status = Path(runtime)/'wallet-status.ini'
        if status.exists():
            values=ini_values(status.read_bytes())
            if values.get('wallet','Healthy',fallback='0')=='1' and time.time()-values.getint('wallet','HeartbeatUnix',fallback=0)<20:
                raise ValueError('此运行目录已有健康工作器，Manager 未启动第二份')
        runtime.mkdir(parents=True,exist_ok=True)
        lease=(runtime/'.manager-wallet.lock').open('a+b')
        try:
            import msvcrt
            lease.seek(0,2)
            if not lease.tell(): lease.write(b'\0');lease.flush()
            lease.seek(0)
            msvcrt.locking(lease.fileno(),msvcrt.LK_NBLCK,1)
        except Exception:
            lease.close()
            raise ValueError('另一个 Manager 或奖励入口拥有此工作区') from None
        self.stop_event.clear(); self.error = ''; self.healthy = False
        ready = threading.Event()
        def run():
            connection = worker = None
            try:
                from .database import connect
                from owner.rewards.sql_repository import RewardWalletRepository
                from owner.rewards.worker import WalletWorker
                connection = connect(ini, 'DB StoreBilling')
                worker = WalletWorker(runtime, RewardWalletRepository(connection))
                self.healthy = worker.poll()
                if not self.healthy: raise ValueError('钱包工作器尚未发布健康状态')
                ready.set()
                while not self.stop_event.wait(1):
                    self.healthy = worker.poll()
                    self.error = '' if self.healthy else '钱包运行时未就绪，请查看后台任务状态'
            except Exception:
                self.error = '钱包服务未就绪，请检查工作区、现有工作器和数据库安装状态'
                ready.set()
            finally:
                self.healthy = False
                try:
                    if worker:
                        worker.last_error = 'wallet_worker_stopped'
                        worker.heartbeat(False)
                finally:
                    try:
                        if connection: connection.close()
                    finally: lease.close()
        self.thread = threading.Thread(target=run, name='sdgo-wallet', daemon=True)
        self.thread.start(); ready.wait(20)
        return self.state()

    def stop(self):
        self.stop_event.set()
        if self.thread: self.thread.join(timeout=20)
        return self.state()
