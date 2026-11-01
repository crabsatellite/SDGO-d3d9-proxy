import copy
import json
from pathlib import Path
import tempfile
import unittest

from tools import game_test_server_report as report


class ServerReportTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path=Path(self.temp.name)/'events.jsonl'
        self.metadata={'run_id':'6fd7578d-1e08-4a50-b2af-9dd44382b260',
                       'plan':{'mode_sequence':'1','expected_ai':3,'title_nonce':'gt1234','room_title':'gt1234 !ai e2 a1'}}

    def stream(self,modes=(1,)):
        common={'run_id':self.metadata['run_id'],'role':'server','suite':'pvp-ai'}
        events=[dict(common,event='observer_ready',host_key=report.BATTLE_HOST,pid=123,
                     normal_server_main=True,require_end=True,nonce='gt1234',expected_host_pilot_id=9229,minimum_ai=3)]
        host={'slot':0,'pilot_id':9229,'unit_or_npc_id':10003,'team':1,'is_ai':False}
        bots=[{'slot':seat,'pilot_id':2147483650+i,'unit_or_npc_id':3604001+i,'team':team,'is_ai':True}
              for i,(seat,team) in enumerate(((1,1),(6,2),(7,2)))]
        sequence=0
        for mode in modes:
            for step in report.STEPS:
                sequence+=1;setup=step!='request_accepted'
                events.append(dict(common,event=step,sequence=sequence,qpc=1000+sequence,thread_id=456,
                    mode=mode,room_cookie=100+mode,host_pilot_id=9229,title=self.metadata['plan']['room_title'],status_code=0,
                    humans_team_1=1,humans_team_2=0,ai_team_1=1 if setup else 0,ai_team_2=2 if setup else 0,
                    active_slots=4 if setup else 1,slots=copy.deepcopy([host]+(bots if setup else [])),score_team_1=24,score_team_2=36))
        return events

    def evaluate(self,events):
        self.path.write_text('\n'.join(json.dumps(e) for e in events)+'\n',encoding='utf-8')
        return report.evaluate(self.metadata,[self.path],9229)

    def test_real_matrix_requires_every_mode_not_only_first_server_summary(self):
        self.metadata['plan']['mode_sequence']='1,3,6,7,9'
        self.assertEqual(self.evaluate(self.stream((1,3,6,7,9)))['status'],'pass')
        incomplete=self.evaluate(self.stream())
        self.assertEqual(incomplete['status'],'fail')
        self.assertEqual(len(incomplete['errors']),4)

    def test_same_run_role_nonce_and_native_main_are_bound(self):
        for field,value in (('run_id','stale-run'),('role','client'),('nonce','wrong'),
                            ('normal_server_main',False),('require_end',False),('pid',False)):
            events=self.stream();events[0][field]=value
            self.assertEqual(self.evaluate(events)['status'],'fail',field)

    def test_missing_end_and_out_of_order_active_cannot_pass(self):
        events=self.stream()
        self.assertEqual(self.evaluate(events[:-1])['status'],'fail')
        events[2],events[3]=events[3],events[2]
        self.assertEqual(self.evaluate(events)['status'],'fail')

    def test_duplicate_battles_and_duplicate_native_sequences_fail(self):
        self.assertEqual(self.evaluate(self.stream((1,1)))['status'],'fail')
        events=self.stream();events[3]['sequence']=events[2]['sequence']
        self.assertEqual(self.evaluate(events)['status'],'fail')

    def test_participant_identity_and_ally_enemy_placement_are_verified(self):
        for mutate in (
            lambda e:e[2]['slots'][1].update(pilot_id=9229),
            lambda e:e[2]['slots'][1].update(team=2),
            lambda e:e[3]['slots'][1].update(pilot_id=999999),
            lambda e:e[2].update(ai_team_2=1),
            lambda e:e[2].update(active_slots=True),
        ):
            events=self.stream();mutate(events)
            self.assertEqual(self.evaluate(events)['status'],'fail')

    def test_title_host_and_failure_event_are_not_silently_ignored(self):
        for field,value in (('title','other room'),('host_pilot_id',6228),
                            ('status_code',False),('event','setup_failed')):
            events=self.stream();events[2][field]=value
            self.assertEqual(self.evaluate(events)['status'],'fail',field)

    def test_malformed_records_and_missing_files_fail_closed(self):
        self.assertEqual(self.evaluate([[]])['status'],'fail')
        self.path.write_text('invalid json',encoding='utf-8')
        self.assertEqual(report.evaluate(self.metadata,[self.path],9229)['status'],'fail')
        self.assertEqual(report.evaluate(self.metadata,[self.path.with_name('absent')],9229)['status'],'fail')

    def test_same_input_path_is_not_double_counted(self):
        self.evaluate(self.stream())
        result=report.evaluate(self.metadata,[self.path,self.path],9229)
        self.assertEqual(result['status'],'pass')
        self.assertEqual(len(result['sources']),1)


if __name__=='__main__':unittest.main()
