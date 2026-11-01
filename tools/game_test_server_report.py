"""Validate passive native Battle observer evidence for every requested mode.

This reader never launches a process or sends a game request. The server's
single-scenario summary cannot substitute for the per-battle raw lifecycle in
a client matrix, so this report groups and validates all four native events.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

try:
    from . import game_test
except ImportError:
    import game_test


BATTLE_HOST = 'BattleServer-548A891D-0046C000'
STEPS = ('request_accepted', 'setup_complete', 'battle_active', 'battle_end')


def integer(value, name, minimum=0):
    if type(value) is not int or value < minimum:
        raise ValueError(f'invalid {name}')
    return value


def participants(event, pilot, expected_ai, setup):
    slots=event.get('slots')
    if not isinstance(slots,list):raise ValueError('missing native participant slots')
    ids=set(); seats=set(); humans=0; bots=0; teams={1:[0,0],2:[0,0]}
    for slot in slots:
        who=integer(slot.get('pilot_id'),'participant pilot',1)
        seat=integer(slot.get('slot'),'participant slot')
        team=integer(slot.get('team'),'participant team',1)
        if who in ids or seat in seats or seat>=12 or team not in teams:
            raise ValueError('ambiguous native participant identity')
        if type(slot.get('is_ai')) is not bool:raise ValueError('invalid native AI flag')
        integer(slot.get('unit_or_npc_id'),'participant unit',1)
        ids.add(who);seats.add(seat)
        if slot['is_ai']:bots+=1;teams[team][1]+=1
        else:
            if who!=pilot:raise ValueError('unexpected human pilot in isolated battle')
            humans+=1;teams[team][0]+=1
    if humans!=1 or bots!=(expected_ai if setup else 0):raise ValueError('native participant counts disagree with plan')
    if integer(event.get('active_slots'),'active slots')!=len(slots):raise ValueError('active-slot total mismatch')
    for team in (1,2):
        if integer(event.get(f'humans_team_{team}'),'human team count')!=teams[team][0] or integer(event.get(f'ai_team_{team}'),'AI team count')!=teams[team][1]:
            raise ValueError('native team totals disagree with slots')
    if setup:
        own=next(s['team'] for s in slots if not s['is_ai'])
        if teams[own][1]!=expected_ai//2 or teams[3-own][1]!=(expected_ai+1)//2:
            raise ValueError('native ally/enemy placement disagrees with plan')
    return ids


def evaluate(metadata: dict, paths: list[Path], pilot: int) -> dict:
    integer(pilot,'expected host pilot',1)
    plan=metadata['plan'];run_id=metadata['run_id']
    modes=game_test.parse_mode_sequence(plan['mode_sequence'])
    expected_ai=integer(plan['expected_ai'],'expected AI',1)
    errors=[];battles=[];sources=[];seen_paths=set()
    for path in paths:
        path=path.resolve()
        if path in seen_paths:continue
        seen_paths.add(path)
        current={};last_sequence=0;last_qpc=0
        try:
            raw=path.read_bytes()
            records=[json.loads(line) for line in raw.decode('utf-8').splitlines() if line.strip()]
            if not records:raise ValueError('empty server observer stream')
            if not all(isinstance(record,dict) for record in records):raise ValueError('server observer records must be objects')
            ready=records[0]
            if any(r.get('run_id')!=run_id or r.get('role')!='server' or r.get('suite')!='pvp-ai' for r in records):
                raise ValueError('server observer stream belongs to another run or role')
            if (ready.get('event')!='observer_ready' or ready.get('host_key')!=BATTLE_HOST or
                ready.get('normal_server_main') is not True or ready.get('require_end') is not True or
                ready.get('nonce')!=plan['title_nonce'] or integer(ready.get('expected_host_pilot_id'),'observer host',1)!=pilot or
                integer(ready.get('minimum_ai'),'observer AI count',1)!=expected_ai):
                raise ValueError('server observer startup binding does not match the client plan')
            pid=integer(ready.get('pid'),'native server PID',1)
            sources.append({'path':str(path),'sha256':hashlib.sha256(raw).hexdigest(),'pid':pid,'host_key':BATTLE_HOST})
            for event in records[1:]:
                name=event.get('event')
                if name not in STEPS:raise ValueError('unexpected or failed native observer event')
                sequence=integer(event.get('sequence'),'native sequence',1)
                qpc=integer(event.get('qpc'),'native timestamp',1)
                integer(event.get('thread_id'),'native thread',1)
                if sequence<=last_sequence or qpc<last_qpc:raise ValueError('native event ordering is not monotonic')
                last_sequence=sequence;last_qpc=qpc
                mode=integer(event.get('mode'),'native room mode',1)
                cookie=integer(event.get('room_cookie'),'native room cookie',1)
                if (mode not in modes or event.get('title')!=plan['room_title'] or
                    integer(event.get('host_pilot_id'),'room host',1)!=pilot or integer(event.get('status_code'),'native result')!=0):
                    raise ValueError('native room identity, result or mode does not match plan')
                if name==STEPS[0]:
                    if cookie in current and current[cookie]['phase']!=4:raise ValueError('room identity reused before battle ended')
                    battle={'mode':mode,'room_cookie':cookie,'pid':pid,'phase':0,'events':[], 'qpc_begin':qpc}
                    current[cookie]=battle;battles.append(battle)
                battle=current.get(cookie)
                if not battle or battle['mode']!=mode or battle['phase']>=4 or STEPS[battle['phase']]!=name:
                    raise ValueError('native battle lifecycle is missing a step or out of order')
                ids=participants(event,pilot,expected_ai,name!=STEPS[0])
                if name==STEPS[1]:battle['participant_ids']=sorted(ids)
                if name in STEPS[2:] and sorted(ids)!=battle['participant_ids']:
                    raise ValueError('native participants changed across the active battle')
                battle['events'].append(name);battle['phase']+=1
                if name==STEPS[3]:
                    battle['qpc_end']=qpc
                    battle['scores']=[integer(event.get(f'score_team_{team}'),'final native score') for team in (1,2)]
        except (OSError,UnicodeError,json.JSONDecodeError,ValueError,TypeError,KeyError) as error:
            errors.append(f'{path}: {error}')
    for mode in modes:
        matches=[b for b in battles if b['mode']==mode]
        if len(matches)!=1 or matches[0]['phase']!=4:
            errors.append(f'mode {mode} requires exactly one complete four-event native battle lifecycle')
    return {'schema':1,'run_id':run_id,'scope':'passive_native_server_lifecycle',
            'status':'fail' if errors else 'pass','expected_host_pilot_id':pilot,
            'required_modes':list(modes),'sources':sources,'battles':battles,'errors':errors}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run-dir',type=Path,required=True)
    parser.add_argument('--events',type=Path,action='append',required=True)
    parser.add_argument('--host-pilot',type=int,required=True)
    args=parser.parse_args()
    metadata=game_test.validate_metadata(args.run_dir)
    game_test.verify_hashes(metadata)
    report=evaluate(metadata,args.events,args.host_pilot)
    game_test.atomic_write(args.run_dir/'receipts/server-report.json',json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
    return 0 if report['status']=='pass' else 1


if __name__=='__main__':raise SystemExit(main())
