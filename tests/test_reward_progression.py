import copy
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from owner.rewards import model, progression
from owner.rewards.workspace import Workspace
from owner.rewards.worker import WalletWorker, ini_bytes, read_job
from test_reward_worker import Ledger
import test_reward_repository as sql_fixture


def epoch(value):
    return int(datetime.fromisoformat(value).replace(tzinfo=timezone.utc).timestamp())


class ProgressionTests(unittest.TestCase):
    def test_price_target_and_solo_without_mvp(self):
        p = progression.solo_policy(model.defaults())
        model.validate(p)
        self.assertEqual(p['pve']['base']['clear']['mb'], '2')
        self.assertEqual(p['pvp']['base']['win']['gb'], model.defaults()['pvp']['base']['win']['gb'])
        fixed = p['progression']['daily_first_mb'] + p['progression']['daily_mb']
        self.assertLess(fixed * 2, 69900)
        self.assertGreaterEqual(fixed * 3, 69900)
        self.assertEqual(fixed, 30000)
        self.assertEqual(p['progression']['weekly_mb'], 15000)

    def test_china_day_and_monday_year_boundaries(self):
        self.assertEqual(progression.periods(epoch('2026-11-01T15:59:59')), ('2026-11-01','2026-10-26'))
        self.assertEqual(progression.periods(epoch('2026-11-01T16:00:00')), ('2026-11-02','2026-11-02'))
        self.assertEqual(progression.periods(epoch('2026-12-31T16:00:00')), ('2027-01-01','2026-12-28'))

    def test_no_short_or_afk_progress_but_zero_kills_can_qualify(self):
        p = {**progression.defaults(), 'enabled': True}
        self.assertFalse(progression.qualifying({'active_seconds':179,'contributed':True},p))
        self.assertFalse(progression.qualifying({'active_seconds':180,'contributed':False},p))
        self.assertTrue(progression.qualifying({'active_seconds':180,'contributed':True},p))
        self.assertFalse(progression.qualifying({'active_seconds':180,'contributed':True},progression.defaults()))

    def test_invalid_configuration_does_not_replace_saved_policy(self):
        with tempfile.TemporaryDirectory() as root:
            w=Workspace(root);old=w.public();before=w.path.read_bytes()
            for change in ({'daily_matches':1},{'minimum_active_seconds':0},{'enabled':1},{'weekly_mb':-1}):
                with self.assertRaises(ValueError): w.save_progression({**progression.defaults(),**change},old['revision'])
                self.assertEqual(before,w.path.read_bytes())
            new=w.solo_preset(old['revision'])
            self.assertTrue(new['progression']['config']['enabled'])
            with self.assertRaises(ValueError):w.solo_preset(old['revision'])
            snapshot=w.runtime.root/'policies'/ (new['revision']+'.json')
            self.assertEqual(model.revision(json.loads(snapshot.read_text())),new['revision'])
            self.assertEqual(w.public()['policy']['progression']['daily_mb'],27000)

    def test_worker_snapshot_and_metadata_before_any_credit(self):
        with tempfile.TemporaryDirectory() as root:
            w=Workspace(root);state=w.solo_preset(w.public()['revision'])
            class Repo(Ledger):
                def apply_progression(self, job, policy):
                    self.bonus_policy=policy;self.balance+=policy['daily_first_mb'];return policy['daily_first_mb']
            repo=Repo();worker=WalletWorker(w.runtime.root,repo,clock=lambda:epoch('2026-11-01T12:00:00'))
            path=worker.outbox/'native.ini'
            def write(**changes):
                v={'SchemaVersion':2,'MatchId':'native.1','AccountNumber':1001,'PlanHash':'a'*64,'Mb':0,
                   'PolicyRevision':state['revision'],'ExpectedScope':repo.scope,'CompletedUnix':epoch('2026-11-01T10:00:00'),
                   'ActiveSeconds':180,'Contributed':1};v.update(changes);path.write_bytes(ini_bytes('reward',v))
            write(Contributed=2)
            self.assertFalse(worker.poll());self.assertEqual(repo.balance,1000)
            write(CompletedUnix=epoch('2026-11-02T10:00:00'))
            self.assertFalse(worker.poll());self.assertEqual(repo.balance,1000)
            write()
            snapshot=w.runtime.root/'policies'/(state['revision']+'.json')
            original=snapshot.read_bytes();snapshot.write_text('{}')
            self.assertFalse(worker.poll());self.assertEqual(repo.balance,1000)
            snapshot.write_bytes(original)
            # Disabling now must not revoke the original approved, captured job.
            w.save_progression(progression.defaults(),w.public()['revision'])
            self.assertTrue(worker.poll());self.assertEqual(repo.balance,4000)
            self.assertTrue(repo.bonus_policy['enabled'])
            self.assertIn(b'CurrentMb=4000', (worker.receipts/path.name).read_bytes())

    def test_snapshot_conflict_restores_canonical_policy_and_native_revision(self):
        with tempfile.TemporaryDirectory() as root:
            w=Workspace(root);state=w.public();before=w.path.read_bytes();native=w.runtime.policy_path.read_bytes()
            candidate=progression.solo_policy(w.read())
            collision=w.runtime.root/'policies'/(model.revision(candidate)+'.json')
            collision.write_text('{}')
            with self.assertRaises(ValueError):w.solo_preset(state['revision'])
            self.assertEqual(w.path.read_bytes(),before)
            self.assertEqual(w.runtime.policy_path.read_bytes(),native)


class ProgressionSqlTests(sql_fixture.RewardRepositorySqlIntegrationTests):
    def setUp(self):
        super().setUp()
        c=self.connection.cursor();c.execute('DELETE dbo.SdgoBattleProgressMatch');c.execute('DELETE dbo.SdgoBattleProgressPeriod');self.connection.commit()
        self.progress={**progression.defaults(),'enabled':True}

    def job(self, number, timestamp='2026-10-26T10:00:00', **changes):
        job={'match_id':'realm.'+str(number),'account_number':900000001,'expected_scope':self.scope,
             'source_hash':hashlib.sha256(str(number).encode()).hexdigest(),'policy_revision':'b'*64,
             'completed_unix':epoch(timestamp),'active_seconds':180,'contributed':True}
        job.update(changes);return job

    def apply(self, job):
        return self.repository.apply_progression(job,self.progress)

    def scalar(self, sql):
        value=self.connection.cursor().execute(sql).fetchone()[0];self.connection.rollback();return value

    def test_daily_and_weekly_dedup_across_realms_and_restart(self):
        total=0
        for i in range(12):
            job=self.job(i,f'2026-10-{26+i//3:02}T10:00:00')
            bonus=self.apply(job);total+=bonus
            self.assertEqual(self.apply(job),bonus)
        self.assertEqual(total,4*30000+15000)
        self.assertEqual(self.repository.current_mb(900000001),136000)
        self.assertEqual(self.scalar('SELECT COUNT(*) FROM dbo.SdgoBattleProgressMatch'),12)
        self.assertEqual(self.scalar("SELECT MAX(MatchCount) FROM dbo.SdgoBattleProgressPeriod WHERE PeriodKind='W'"),12)
        # Different server, same account and day: cannot claim the first/third again.
        self.assertEqual(self.apply(self.job('other-server')),0)

    def test_china_boundary_and_short_or_afk_do_not_advance(self):
        for i, changes in enumerate(({'active_seconds':179},{'contributed':False})):
            self.assertEqual(self.apply(self.job(i,**changes)),0)
        self.assertEqual(self.scalar('SELECT COUNT(*) FROM dbo.SdgoBattleProgressPeriod'),0)
        self.assertEqual(self.apply(self.job(2,'2026-10-25T15:59:59')),3000)
        self.assertEqual(self.apply(self.job(3,'2026-10-25T16:00:00')),3000)
        self.assertEqual(self.scalar("SELECT COUNT(*) FROM dbo.SdgoBattleProgressPeriod WHERE PeriodKind='W'"),2)

    def test_bonus_failure_rolls_back_counters_and_receipt(self):
        c=self.connection.cursor()
        c.execute("CREATE TRIGGER dbo.RejectProgressFixture ON dbo.SdgoBattleProgressMatch AFTER INSERT AS THROW 51995,'fixture rollback',1;")
        self.connection.commit()
        try:
            with self.assertRaises(self.pyodbc.Error):self.apply(self.job(1))
            self.assertEqual(self.repository.current_mb(900000001),1000)
            self.assertEqual(self.scalar('SELECT COUNT(*) FROM dbo.SdgoBattleProgressPeriod'),0)
        finally:
            self.connection.cursor().execute('DROP TRIGGER dbo.RejectProgressFixture');self.connection.commit()
        self.assertEqual(self.apply(self.job(1)),3000)

    def test_retry_after_commit_uses_original_award_and_rejects_changed_identity(self):
        job=self.job(1);self.assertEqual(self.apply(job),3000)
        self.progress['daily_first_mb']=9999
        self.assertEqual(self.apply(job),3000)
        with self.assertRaises(self.pyodbc.Error):self.apply({**job,'source_hash':'c'*64})
        self.assertEqual(self.repository.current_mb(900000001),4000)

    def test_concurrent_different_match_instances_pay_milestones_once(self):
        from concurrent.futures import ThreadPoolExecutor
        from owner.rewards.sql_repository import RewardWalletRepository
        def pay(i):
            connection=self._connect(self.database,autocommit=False)
            try:return RewardWalletRepository(connection).apply_progression(self.job(i),self.progress)
            finally:connection.close()
        with ThreadPoolExecutor(max_workers=6) as pool:
            amounts=list(pool.map(pay,range(6)))
        self.assertEqual(sum(amounts),30000)
        self.assertEqual(self.repository.current_mb(900000001),31000)

    def test_missing_wallet_and_stale_scope_never_advance_progress(self):
        import uuid
        with self.assertRaises(self.pyodbc.Error):self.apply(self.job(1,account_number=900000002))
        with self.assertRaises(self.pyodbc.Error):self.apply(self.job(2,expected_scope=uuid.uuid4()))
        self.assertEqual(self.scalar('SELECT COUNT(*) FROM dbo.SdgoBattleProgressPeriod'),0)
        self.assertEqual(self.scalar('SELECT COUNT(*) FROM dbo.SdgoBattleProgressMatch'),0)

    def test_wallet_limit_failure_keeps_milestone_available_for_retry(self):
        self.connection.cursor().execute('UPDATE dbo.tbls_AccountPocket SET f_GC=2147483640 WHERE f_idx=900000001')
        self.connection.commit()
        with self.assertRaises(self.pyodbc.Error):self.apply(self.job(1))
        self.assertEqual(self.scalar('SELECT COUNT(*) FROM dbo.SdgoBattleProgressPeriod'),0)
        self.assertEqual(self.scalar('SELECT COUNT(*) FROM dbo.SdgoBattleProgressMatch'),0)
        self.connection.cursor().execute('UPDATE dbo.tbls_AccountPocket SET f_GC=1000 WHERE f_idx=900000001')
        self.connection.commit()
        self.assertEqual(self.apply(self.job(1)),3000)

    def test_worker_crash_after_bonus_commit_replays_without_double_credit(self):
        with tempfile.TemporaryDirectory() as root:
            w=Workspace(root);state=w.solo_preset(w.public()['revision'])
            worker=WalletWorker(w.runtime.root,self.repository)
            path=worker.outbox/'native.ini'
            path.write_bytes(ini_bytes('reward',{'SchemaVersion':2,'MatchId':'crash.1','AccountNumber':900000001,
                'PlanHash':'a'*64,'Mb':25,'PolicyRevision':state['revision'],'ExpectedScope':self.scope,
                'CompletedUnix':epoch('2026-10-26T10:00:00'),'ActiveSeconds':180,'Contributed':1}))
            with patch('owner.rewards.worker._atomic_write',side_effect=OSError('lost acknowledgement')):
                with self.assertRaises(OSError):worker.process(path)
            self.assertEqual(self.repository.current_mb(900000001),4025)
            self.assertTrue(worker.poll())
            self.assertEqual(self.repository.current_mb(900000001),4025)
            self.assertIn(b'BonusMb=3000', (worker.receipts/path.name).read_bytes())
