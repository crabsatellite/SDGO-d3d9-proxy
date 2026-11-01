"""One lifecycle owner for registered Mod background services and IPC resources."""
from __future__ import annotations
import threading
import time
from . import storage


class ServiceRegistry:
    def __init__(self, workspace):
        self.workspace = workspace
        self.entries = {}
        self.resources = {}
        self.lock = threading.RLock()
        self.stop_event = threading.Event()
        self.thread = None

    def register(self, identity, title, controller, start):
        with self.lock:
            if identity in self.entries: raise ValueError('后台服务重复登记：' + identity)
            self.entries[identity] = {'title': title, 'controller': controller, 'start': start,
                                      'lock': threading.RLock(), 'retry_at': 0, 'error': '', 'unhealthy_since': None}

    def resource(self, identity, factory):
        with self.lock:
            value = self.resources.get(identity)
            if value is None:
                value = factory()
                self.resources[identity] = value
            return value

    def release_resource(self, identity):
        with self.lock:
            value = self.resources.pop(identity, None)
        if value is not None: value.close()

    def desired(self, identity):
        return self.workspace.settings.get('services', {}).get(identity, {}).get('enabled', False) is True

    def _desired(self, identity, enabled):
        with storage.LOCK:
            self.workspace.settings.setdefault('services', {})[identity] = {'enabled': enabled}
            self.workspace.save_workspace()

    def describe(self, identity=None):
        def one(key, entry):
            status = entry['controller'].state()
            return {**status, 'id': key, 'title': entry['title'], 'desired': self.desired(key),
                    'error': status.get('error') or entry['error'], 'owner': 'manager'}
        with self.lock:
            if identity is not None:
                if identity not in self.entries: raise ValueError('后台服务未登记')
                return one(identity, self.entries[identity])
            return {'services': [one(key, entry) for key, entry in self.entries.items()],
                    'resources': [{'id': key, 'owner': 'manager', 'running': value.process.poll() is None}
                                  for key, value in self.resources.items() if hasattr(value, 'process')]}

    def start(self, identity, persist=True):
        if identity not in self.entries: raise ValueError('后台服务未登记')
        entry = self.entries[identity]
        with entry['lock']:
            if not persist and (not self.desired(identity) or self.stop_event.is_set()):
                return self.describe(identity)
            if persist: self._desired(identity, True)
            try:
                status = entry['controller'].state()
                if not status.get('running'): entry['start']()
                entry['error'] = ''
            except Exception as error:
                entry['error'] = str(error)
                entry['retry_at'] = time.monotonic() + 15
                raise
            status = entry['controller'].state()
            if not status.get('running'): entry['retry_at'] = time.monotonic() + 15
            return self.describe(identity)

    def stop(self, identity, persist=True):
        if identity not in self.entries: raise ValueError('后台服务未登记')
        entry = self.entries[identity]
        with entry['lock']:
            if persist: self._desired(identity, False)
            entry['controller'].stop()
            entry['error'] = ''
            return self.describe(identity)

    def restore(self):
        """Restore only explicitly enabled Manager services, including after restart."""
        if self.thread and self.thread.is_alive(): return
        self.stop_event.clear()
        def supervise():
            while not self.stop_event.is_set():
                for identity, entry in list(self.entries.items()):
                    if self.stop_event.is_set(): break
                    status=entry['controller'].state()
                    if self.desired(identity) and status.get('running') and status.get('healthy') is False:
                        if entry['unhealthy_since'] is None:entry['unhealthy_since']=time.monotonic()
                        elif time.monotonic()-entry['unhealthy_since']>=15:
                            self.stop(identity,persist=False);entry['unhealthy_since']=None
                    else:entry['unhealthy_since']=None
                    if self.desired(identity) and not entry['controller'].state().get('running') and time.monotonic() >= entry['retry_at']:
                        try: self.start(identity, persist=False)
                        except Exception: pass  # The registered service reports the bounded error.
                self.stop_event.wait(2)
        self.thread = threading.Thread(target=supervise, name='sdgo-service-supervisor', daemon=True)
        self.thread.start()

    def close(self):
        self.stop_event.set()
        if self.thread: self.thread.join(timeout=25)
        for identity in list(self.entries): self.stop(identity, persist=False)
        for identity in list(self.resources): self.release_resource(identity)
