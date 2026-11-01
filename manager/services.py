"""Manager-owned background capabilities; never start or stop game processes."""
import threading
from pathlib import Path


class WalletService:
    def __init__(self):
        self.thread = None
        self.stop_event = threading.Event()
        self.error = ''

    def state(self):
        return {'running': bool(self.thread and self.thread.is_alive()), 'error': self.error,
                'ownership': 'manager_process', 'delivery': 'native_MB_ledger'}

    def start(self, ini, runtime, workspace):
        if self.thread and self.thread.is_alive(): return self.state()
        if not ini or not Path(ini).is_file(): raise ValueError('请先选择包含 DB StoreBilling 的原生服务器 INI')
        workspace = Path(workspace)
        if (workspace/'battle-reward-wallet-worker.json').exists():
            raise ValueError('已有旧入口登记的钱包工作器，请先完成该工作器的迁移，不会重复启动或停止它')
        from .storage import ini_values
        import time
        status = Path(runtime)/'wallet-status.ini'
        if status.exists():
            values=ini_values(status.read_bytes())
            if values.get('wallet','Healthy',fallback='0')=='1' and time.time()-values.getint('wallet','HeartbeatUnix',fallback=0)<20:
                raise ValueError('此运行目录已有健康工作器，Manager 未启动第二份')
        workspace.mkdir(parents=True,exist_ok=True)
        lease=(workspace/'battle-reward-wallet-worker.lock').open('a+b')
        try:
            import msvcrt
            lease.seek(0);lease.write(b'\0');lease.flush();lease.seek(0)
            msvcrt.locking(lease.fileno(),msvcrt.LK_NBLCK,1)
        except Exception:
            lease.close()
            raise ValueError('另一个 Manager 或奖励入口拥有此工作区') from None
        self.stop_event.clear(); self.error = ''
        ready = threading.Event()
        def run():
            connection = worker = None
            try:
                from .database import connect
                from owner.rewards.sql_repository import RewardWalletRepository
                from owner.rewards.worker import WalletWorker
                connection = connect(ini, 'DB StoreBilling')
                worker = WalletWorker(runtime, RewardWalletRepository(connection))
                if not worker.poll(): raise ValueError('钱包工作器尚未发布健康状态')
                ready.set()
                while not self.stop_event.wait(1): worker.poll()
            except Exception:
                self.error = '钱包服务未就绪，请检查工作区、现有工作器和数据库安装状态'
                ready.set()
            finally:
                if worker:
                    worker.last_error = 'wallet_worker_stopped'
                    worker.heartbeat(False)
                if connection: connection.close()
                lease.close()
        self.thread = threading.Thread(target=run, name='sdgo-wallet', daemon=True)
        self.thread.start(); ready.wait(20)
        return self.state()

    def stop(self):
        self.stop_event.set()
        if self.thread: self.thread.join(timeout=20)
        return self.state()
