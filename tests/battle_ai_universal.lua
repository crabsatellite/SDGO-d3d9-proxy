-- Capability-controller scenarios in real Lua 5.0. No game service or account.
local now,generation,observation=0,1,nil
local checks=0
local calls={}
UNIT_MOVE_DIR_LEFT=3;UNIT_MOVE_DIR_RIGHT=4;UNIT_MOVE_DIR_BACKWARD=2
UNIT_MOVE_EVENT_MOVE=1;UNIT_MOVE_EVENT_DASH=4
function SDGO_BattleAI_Now() return now end
function SDGO_BattleAI_Authority() return 1 end
function SDGO_BattleAI_Generation() return generation end
function SDGO_BattleAI_Observe() return observation end
function SDGO_BattleAI_Native() calls.native=calls.native+1; return 123 end
function SDGO_BattleAI_AimTarget(id) calls.aim=id end
function SDGO_BattleAI_Cadence(ms) calls.cadence=ms end
function SDGO_BattleAI_RecoveryMode(active) calls.recoveryMode=active end
function SDGO_BattleAI_MeleeGate(value) calls.meleeGate=value end
function SDGO_BattleAI_Chance(percent) calls.rolls=(calls.rolls or 0)+1;return calls.roll or 0 end
function SDGO_BattleAI_Cover(id,x,y,z) calls.coverQueries=(calls.coverQueries or 0)+1;calls.coverX=x;return calls.coverCandidate end
function SDGO_BattleAI_CancelRoute()
  calls.cancelled=(calls.cancelled or 0)+1
  if not calls.staticRouteObservation then observation.routeActive=0 end
end
function SDGO_BattleAI_Route(id,force,x,y,z)
  calls.routes=calls.routes+1; calls.routeId=id;calls.force=force
    calls.routeX=x
  local result=calls.routeResult or 1
  if not calls.staticRouteObservation then observation.routeActive=result end
  return result
end
function SDGO_BattleAI_Log(text) calls.error=text end
local npc={}
function npc:GetID() return observation.id end
function npc:CheckHurt() return observation.hurt end
function npc:SetAttackTarget(id) calls.target=id end
function npc:SetAttackOnOff(on) calls.fire=on end
function npc:MoveStop() calls.stop=(calls.stop or 0)+1 end
function npc:SetMoveTargetPosRandom() calls.patrol=(calls.patrol or 0)+1 end
function npc:ChangeWeapon(slot) calls.switch=slot;calls.switches=(calls.switches or 0)+1 end
function npc:SetMoveTargetPos_Dir(dir,distance,facing,jump)
  assert(dir==facing and jump==false and distance>0)
  calls.moves=calls.moves+1;calls.direction=dir
end
function npc:SetMoveType(mode) calls.mode=mode;if mode==UNIT_MOVE_EVENT_DASH then calls.dashes=(calls.dashes or 0)+1 end end
function npc:SetJumpTime(seconds)
  calls.jumps=calls.jumps+1;calls.jumpSeconds=seconds
  if seconds>0 then calls.positiveJumps=(calls.positiveJumps or 0)+1 else calls.jumpCancels=(calls.jumpCancels or 0)+1 end
end
local function Target(id,hp,distance)
  return {id=id,hp=hp,distance=distance,enemy=1,attackable=1,visible=1,rangedFiring=0}
end
local function Weapon(slot,kind,range,damage,cycle)
  return {slot=slot,kind=kind,range=range,damage=damage,cycleMs=cycle,
    valid=1,ammoReady=1,ready=1,movingShot=1}
end
local function Reset(config)
  now=0;generation=generation+1;calls={native=0,moves=0,jumps=0,routes=0}
  observation={id=20,generation=generation,valid=1,hp=1000,unitId=999999,
    pos={x=0,y=0,z=0},goal={x=2000,y=0,z=0},moving=0,
    boostFraction=1,canJump=1,selectedSlot=1,hurt=0,meleeBusy=0,specialBusy=0,
    weapons={Weapon(0,'melee',250,2000,600),Weapon(1,'hitscan',1800,700,200)},
    targets={Target(1,1000,800),Target(2,200,1000)}}
  SDGO_BattleAI_TestConfig=config or {}
  if SDGO_BattleAI_TestConfig.Difficulty==nil then SDGO_BattleAI_TestConfig.Difficulty=2 end
  dofile('mods/battle_ai/universal_ai.lua')
end
local function Tick(time)
  now=time;SDGO_BattleAI_GenericUpdate(npc)
  assert(SDGO_BattleAI_Universal.faults==0,calls.error or 'controller fault')
end
local function Check(ok,text) assert(ok,text);checks=checks+1;print('PASS '..text) end
Reset();Tick(0)
Check(calls.target==2,'focus lower remaining HP within comparable effective weapon range')
Reset();observation.targets[2].distance=5000;Tick(0)
Check(calls.target==1,'distant low-HP opponent outside actual weapon range does not draw chase')
Reset();observation.targets[2].visible=0;Tick(0)
Check(calls.target==1,'occluded low-HP opponent is excluded')
Reset();observation.targets[2].attackable=0;Tick(0)
Check(calls.target==1,'invulnerable low-HP opponent is excluded')
Reset();observation.targets[2].hp=2000;Tick(0)
observation.targets[2].hp=1;Tick(100)
Check(calls.target==2,'new lower-HP target replaces the old target without a hold timer')
Tick(700)
Check(calls.target==2,'lowest HP remains selected while it stays the best candidate')
observation.targets[2].hp=0;Tick(800)
Check(calls.target==1,'death invalidates held target immediately')
Reset();observation.weapons[2].range=4000;observation.targets[1].distance=500
observation.targets[2].hp=2000;Tick(0)
observation.targets[2].hp=1;observation.targets[2].distance=2800;Tick(700)
Check(calls.target==1,'extreme distance gap does not pull an ongoing close engagement away')
Reset();observation.targets={Target(1,1000,100)};Tick(0)
Check(calls.switch==0 and calls.fire==true,'close opponent requests melee while the actually bound gun keeps firing')
observation.selectedSlot=0;Tick(100);Tick(300)
Check(calls.fire==true,'acknowledged melee weapon attacks without an added reaction window')
Reset();observation.selectedSlot=0;Tick(0);Tick(300)
Check(calls.switch==1 and calls.fire==false,'switch request is not treated as immediate native acknowledgement')
observation.selectedSlot=1;Tick(400);Tick(600)
Check(calls.fire==true and calls.cadence==0,'confirmed ranged weapon has no extra attack-intent interval')
Reset();observation.weapons[2].ammoReady=0
observation.weapons[3]=Weapon(2,'missile',2000,1000,600);Tick(0)
Check(calls.switch==2,'empty rifle switches to another usable weapon instead of firing empty slot')
Reset();for _,w in pairs(observation.weapons) do w.ammoReady=0 end;Tick(0)
Check(calls.fire==false and calls.aim==0,'no usable weapon does not fabricate ammo or hits')
Reset({ReactionMs=0});observation.targets[2].rangedFiring=1
Tick(0);local first=calls.direction;Tick(500)
Check(calls.jumps==1 and calls.direction~=first and calls.fire==true,'ranged exchange combines native hop with alternating lateral movement and fire')
Tick(1600)
Check(calls.jumps==2,'jump requests obey cooldown rather than per-frame refill')
Reset({ReactionMs=0});observation.boostFraction=0.1;observation.targets[2].rangedFiring=1;Tick(0)
Check(calls.jumps==0 and calls.mode==UNIT_MOVE_EVENT_MOVE,'low boost uses walking strafe without extra jump')
Reset({ReactionMs=0});observation.weapons[2].movingShot=0;observation.targets[2].rangedFiring=1;Tick(0)
Check(calls.jumps==0 and calls.moves==0 and calls.fire==true,'stationary-only weapon keeps native firing movement restriction')
Reset({ReactionMs=0,BurstMs=300,BurstPauseMs=200});Tick(0);Tick(300)
Check(calls.fire==true,'legacy burst configuration cannot create an intent pause')
Tick(500)
Check(calls.fire==true,'attack intent stays continuous across the old burst-pause boundary')
Reset();observation.targets={Target(1,1000,3000)};Tick(0);Tick(100);Tick(800)
Check(calls.routes==2 and calls.fire==false,'out-of-range pursuit uses bounded navigation requests')
Reset();calls.routeResult=0;observation.targets={Target(1,1000,3000)};Tick(0);Tick(100)
Check(calls.routes==1 and calls.aim==0,'failed route is temporarily excluded from chase')
Reset();observation.hurt=1;Tick(0)
Check(calls.fire==false and calls.routes==0 and calls.jumps==0,'hurt state cancels controller intent')
Reset();observation.hp=0;Tick(0)
Check(calls.fire==false and calls.routes==0,'dead actor does not attack or move')
Reset();observation.valid=0;Tick(0)
Check(calls.native==1 and calls.target==nil,'unknown native data fails back to original script')
Reset();Tick(0);Tick(100);Tick(200)
Check(calls.native==1,'stock initialization executes once per generation, not every frame')
generation=generation+1;observation.generation=generation;Tick(300)
Check(calls.native==2,'respawn reinitializes generic controller')
Reset();observation.unitId=123;Tick(0);local selected=calls.target
Reset();observation.unitId=987654;Tick(0)
Check(calls.target==selected,'arbitrary unit ID does not select a hardcoded AI archetype')
Reset();observation.meleeBusy=1;observation.selectedSlot=0;Tick(0)
Check(calls.switch==nil and calls.moves==0,'active melee combo is not interrupted by ranged preference')
Reset({ReactionMs=0,BurstMs=100});observation.rangedBusy=1;observation.weapons[2].active=1
observation.weapons[2].ammoReady=0;observation.weapons[2].ready=0;Tick(0);Tick(1000)
Check(calls.switch==nil and calls.fire==true,'native charged/continuous ranged state retains held input after ammo consumption and burst expiry')
Reset();observation.targets[2].groundPos={x=1000,y=0,z=10};Tick(0)
observation.targets[1].visible=0;observation.targets[2].visible=0
observation.targets[2].groundPos={x=3000,y=0,z=999};Tick(100)
Check(calls.routes==1 and calls.routeX==1000 and not calls.fire,'occluded known enemy is pursued through last visible position rather than live hidden position')
Tick(3000)
Check((calls.patrol or 0)>0 and calls.aim==0,'lost target memory expires and native patrol replaces stationary waiting')
Reset();Tick(0);observation.alternate=1
for _,candidate in pairs(observation.targets) do candidate.hp=10000 end
observation.weapons[1]=Weapon(0,'projectile',1800,5000,200)
Tick(100)
Check(calls.switch==0,'form/bank change immediately reevaluates weapon data despite previous hold')
Reset();observation.targets={Target(1,1000,3000)};observation.moving=1;Tick(0)
for t=100,1500,100 do observation.pos={x=0,y=t/10,z=0};Tick(t) end
Check(calls.moves>0 and (calls.cancelled or 0)>0,'hopping against a wall triggers recovery and releases the previous navigation owner')
Reset({ReactionMs=0});observation.targets={Target(2,100,100)};observation.selectedSlot=0;observation.meleePressure=1;Tick(0)
Check(calls.switch==1 and calls.meleeGate==1 and calls.fire==false,'melee pressure selects ranged and suppresses a sword counter')
Reset({ReactionMs=0});observation.targets={Target(2,100,100)};observation.meleePressure=1;observation.weapons[2].ammoReady=0;Tick(0)
Check(calls.fire==false and calls.switch==nil,'empty ranged loadout under melee pressure does not fall back to a forbidden sword counter')
Reset({ReactionMs=0});calls.staticRouteObservation=true;local t=observation.targets[2]
t.pos={x=1000,y=100,z=0};t.groundPos={x=1000,y=0,z=0};t.forward={x=1,y=0,z=0};t.hurt=1
observation.allies={{id=40,meleeSwinging=1,contactTarget=2,contactAt=0}};Tick(0)
Check(calls.routeX==650 and calls.fire==true,'ally delivered melee contact plus active victim hurt requests a rear route while continuing gunfire')
observation.routeActive=0;observation.moving=0;Tick(100)
Check(calls.routes==2 and SDGO_BattleAI_Universal.states[20].flanking and not calls.cancelled,
      'lost flank route is reacquired immediately instead of retaining a stale movement owner')
observation.routeActive=1;observation.pos={x=650,y=0,z=0};local flankCancels=calls.cancelled or 0;Tick(200)
Check(not SDGO_BattleAI_Universal.states[20].flanking and calls.cancelled>flankCancels,
      'arriving behind the caught target releases flank ownership for the next action')
Reset({ReactionMs=0});t=observation.targets[2]
t.pos={x=1000,y=100,z=0};t.groundPos={x=1000,y=0,z=0};t.forward={x=1,y=0,z=0};t.hurt=1
observation.allies={{id=40,meleeSwinging=1,contactTarget=2,contactAt=0}};Tick(0)
observation.routeActive=0;observation.moving=0;calls.routeResult=0;Tick(100)
Check(not SDGO_BattleAI_Universal.states[20].flanking and calls.fire==true,
      'failed flank reacquisition returns control to ordinary combat in the same decision')
Reset({ReactionMs=0});t=observation.targets[2]
t.pos={x=1000,y=100,z=0};t.groundPos={x=1000,y=0,z=0};t.forward={x=1,y=0,z=0}
observation.allies={{id=40,meleeSwinging=1,contactTarget=2,contactAt=0}};Tick(0)
Check(calls.routes==0,'ally sword intent without an actively hurt victim does not claim a catch')
local function SetupWeave()
  Reset({ReactionMs=0,WeaponHoldMs=0});calls.roll=1
  observation.targets={Target(2,100,100)};local victim=observation.targets[1]
  victim.hurt=1;victim.groundPos={x=100,y=0,z=0}
  observation.allies={{id=40,pos={x=500,y=0,z=0},rangedTarget=2,rangedAt=0}}
  observation.selectedSlot=0;observation.meleeBusy=1;observation.meleeSwinging=1
  observation.comboKnown=1;observation.comboIndex=1;observation.comboMax=3
  observation.contactTarget=2;observation.contactAt=0;observation.shots=0
  observation.contactCombo=1;observation.contactAction=77;observation.actionHandle=77
end
SetupWeave();Tick(0)
Check(calls.meleeGate==1 and calls.fire==false and SDGO_BattleAI_Universal.states[20].weave.stage=='release','supported caught penultimate melee releases continuation before the final knockdown')
observation.meleeBusy=0;observation.meleeSwinging=0;Tick(100)
Check(calls.switch==1 and calls.direction==UNIT_MOVE_DIR_BACKWARD,'weave waits for native melee exit then disengages and requests a gun')
observation.selectedSlot=1;Tick(200)
Check(calls.fire==true and SDGO_BattleAI_Universal.states[20].weave.stage=='shoot','weave fires after native weapon commit')
Tick(300)
Check(SDGO_BattleAI_Universal.states[20].weave.stage=='shoot','elapsed time without a primary fire receipt cannot complete the shooting phase')
observation.shots=1;Tick(400)
Check(calls.switch==0 and SDGO_BattleAI_Universal.states[20].weave.stage=='return','one actual configured primary fire resumes melee')
observation.selectedSlot=0;observation.meleeBusy=1;observation.meleeSwinging=1;observation.comboIndex=0;Tick(500)
observation.comboIndex=1;Tick(600)
Check(calls.meleeGate==0 and SDGO_BattleAI_Universal.states[20].weave.stage=='finish','second melee chain keeps its final strike instead of weaving forever')
SetupWeave();observation.allies[1].rangedTarget=999;Tick(0)
Check(SDGO_BattleAI_Universal.states[20].weave==nil,'unrelated allied fire cannot trigger supported melee weaving')
SetupWeave();observation.comboMax=1;observation.comboIndex=0;Tick(0)
Check(SDGO_BattleAI_Universal.states[20].weave==nil,'single-hit weapon has no penultimate segment to suppress')
SetupWeave();observation.contactTarget=0;Tick(0)
Check(SDGO_BattleAI_Universal.states[20].weave==nil,'a whiff without delivered contact never enters the catch weave')
SetupWeave();observation.contactCombo=0;Tick(0)
Check(SDGO_BattleAI_Universal.states[20].weave==nil,'a still-fresh previous slash cannot masquerade as penultimate contact')
SetupWeave();observation.contactAction=76;Tick(0)
Check(SDGO_BattleAI_Universal.states[20].weave==nil,'contact from another native action handle cannot release the current swing')
SetupWeave();calls.roll=0;Tick(0);observation.meleeBusy=0;observation.meleeSwinging=0;Tick(100)
observation.selectedSlot=1;observation.shots=1;Tick(200)
Check(SDGO_BattleAI_Universal.states[20].weave.stage=='shoot','two-shot weave requires the second distinct native primary fire')
observation.shots=2;Tick(300)
Check(SDGO_BattleAI_Universal.states[20].weave.stage=='return','second primary fire completes the two-shot weave')
SetupWeave();calls.roll=0;Tick(0);observation.meleeBusy=0;observation.meleeSwinging=0;Tick(100)
observation.selectedSlot=1;observation.shots=1;observation.weapons[2].ammoReady=0;observation.weapons[2].ready=0;Tick(200)
Check(calls.switch==0 and SDGO_BattleAI_Universal.states[20].weave.stage=='return',
      'one delivered weave shot returns to melee when no second ranged round is usable')
SetupWeave();calls.roll=0;Tick(0);observation.weapons[2].ammoReady=0;observation.weapons[2].ready=0
observation.meleeBusy=0;observation.meleeSwinging=0;Tick(100)
Check(calls.fire==true and SDGO_BattleAI_Universal.states[20].weave.stage=='return',
      'ranged loss before the first weave shot resumes melee without waiting for the weave timeout')
SetupWeave();observation.nativeTarget=2;observation.specialAvailable=1;observation.specialRange=1800;observation.spReady=1
Tick(0);observation.meleeBusy=0;observation.meleeSwinging=0;Tick(100)
Check(calls.switch==1 and SDGO_BattleAI_Universal.states[20].weave.stage=='shoot',
      'full SP does not replace a supported melee weave with special capture and interrupt allied damage')
Reset({ReactionMs=0});calls.roll=1;t=observation.targets[2];t.pos={x=1000,y=100,z=0}
calls.coverCandidate={valid=1,hide={x=0,y=0,z=0},peek={x=70,y=0,z=0}};observation.shots=0;Tick(0)
Check(calls.routeX==70 and calls.fire==true,'selected cover opportunity moves to the sampled smallest fireable edge')
observation.pos={x=70,y=0,z=0};Tick(100)
Check(SDGO_BattleAI_Universal.states[20].cover.stage=='peek','cover does not retract merely because attack intent was requested')
observation.shots=1;observation.rangedBusy=1;observation.moveCancelOpen=0;observation.weapons[2].movingShot=0;Tick(200)
Check(calls.routeX==70 and calls.fire==true,'a native ranged action keeps held input while waiting for its legal retract window')
observation.moveCancelOpen=1;Tick(300)
Check(calls.routeX==0 and SDGO_BattleAI_Universal.states[20].cover.stage=='tuck','actual primary shot and legal cancel window retract to the covered anchor')
calls.routeResult=0;SDGO_BattleAI_Universal.states[20].cover.routeAt=400;Tick(400)
Check(SDGO_BattleAI_Universal.states[20].cover==nil,'failed cover retraction abandons the tactic instead of retaining a movement owner')
local failedRoutes=calls.routes;observation.rangedBusy=0;Tick(401)
Check(calls.routes==failedRoutes and calls.fire==true,'failed cover retraction does not stop and retry every observation')
Reset({ReactionMs=0});calls.roll=1;observation.targets={Target(2,200,1000)};t=observation.targets[1]
t.pos={x=1000,y=100,z=0};t.groundPos={x=1000,y=0,z=0}
calls.coverCandidate={valid=1,hide={x=0,y=0,z=0},peek={x=70,y=0,z=0}};Tick(0)
t.visible=0;Tick(100)
Check(SDGO_BattleAI_Universal.states[20].cover==nil and calls.routes==2 and calls.routeX==1000,
      'los loss while peeking releases cover immediately and resumes pursuit')
Reset({ReactionMs=0});t=observation.targets[2];t.pos={x=1000,y=100,z=0};calls.roll=0;Tick(0)
Tick(100)
Check((calls.coverQueries or 0)==0,'rejected cover opportunity performs no expensive geometry query or per-frame reroll')
Check(calls.rolls==1,'cover selection probability is rolled once per opportunity cooldown')
Reset({ReactionMs=0});observation.targets={Target(2,100,1000)};t=observation.targets[1]
t.pos={x=1000,y=100,z=0};t.groundPos={x=1000,y=0,z=0};Tick(0)
t.visible=0;t.pos={x=3000,y=100,z=0};calls.roll=1
calls.coverCandidate={valid=1,hide={x=0,y=0,z=0},peek={x=70,y=0,z=0}};Tick(1700)
Check((calls.coverQueries or 0)==0 and calls.routeX==1000 and calls.fire==false,
      'occluded remembered targets are pursued without starting a cover peek')
Reset({ReactionMs=0});t=observation.targets[2];t.meleeTracking=1;observation.weapons[2].movingShot=0;observation.shots=0;Tick(0)
observation.shots=1;observation.rangedBusy=1;observation.moveCancelOpen=0;Tick(100)
Check(calls.moves==0,'orbwalk cannot cancel an unopened native shooting animation')
observation.moveCancelOpen=1;Tick(200)
Check(calls.moves==1 and calls.fire==false,'opened native animation gate permits one short step after a real shot')
observation.rangedBusy=0;observation.weapons[2].ready=0;Tick(400)
Check(calls.fire==false,'orbwalking does not bypass the true weapon cooldown')
observation.weapons[2].ready=1;Tick(500)
Check(calls.fire==true,'orbwalking resumes firing when the weapon is natively ready')
Reset({ReactionMs=0});observation.targets[2].meleeApproaching=1;observation.shots=0;Tick(0)
observation.shots=1;observation.rangedBusy=1;observation.moveCancelOpen=1;Tick(100)
Check(calls.moves==1 and calls.fire==true,'a moving-shot weapon orbwalks against a closing melee loadout before its tracking dash starts')
Reset({Difficulty=1});observation.nativeTarget=2;observation.specialAvailable=1;observation.specialRange=1800;observation.spReady=0
observation.targets[2].selectedTarget=20;observation.targets[2].rangedFiring=1;observation.targets[2].hp=2000
SDGO_BattleAI_ProfileUpdate(npc)
Check(calls.switch==nil,'Medium cannot use the NPC unconditional SP override without the player resource gate')
observation.spReady=1;observation.hurt=1;SDGO_BattleAI_ProfileUpdate(npc)
Check(calls.switch==nil,'Medium cannot launch a special while the player hurt gate forbids it')
observation.hurt=0;SDGO_BattleAI_ProfileUpdate(npc)
Check(calls.switch==3 and calls.fire==true,'Medium requests the native special slot while its current gun keeps legal output')
now=100;observation.selectedSlot=3;observation.specialSelected=1;SDGO_BattleAI_ProfileUpdate(npc)
Check(calls.fire==true,'confirmed native special selection emits attack intent through the ordinary action path')
now=200;observation.spReady=0;SDGO_BattleAI_ProfileUpdate(npc)
Check(calls.fire==false,'SP debit stops further special request pulses while native authorization is pending')
observation.specialBusy=1;SDGO_BattleAI_ProfileUpdate(npc);observation.specialBusy=0;now=1000;SDGO_BattleAI_ProfileUpdate(npc)
Check(calls.switch==1,'completed native special returns to the prior ordinary weapon')
Check(SDGO_BattleAI_Universal.profiles[20].special.returning,'special restore retains ownership until native weapon selection is acknowledged')
now=1100;observation.selectedSlot=0;observation.specialSelected=0;SDGO_BattleAI_ProfileUpdate(npc)
Check(SDGO_BattleAI_Universal.profiles[20].special==nil,'any committed ordinary slot immediately returns control without waiting for the previous slot')
observation.selectedSlot=1;now=1200;SDGO_BattleAI_ProfileUpdate(npc)
Check(SDGO_BattleAI_Universal.profiles[20].special==nil,'confirmed previous ordinary slot releases the special lease')
Reset({Difficulty=1});observation.nativeTarget=2;observation.specialAvailable=1;observation.specialRange=1800;observation.spReady=1
observation.targets[2].selectedTarget=20;observation.targets[2].rangedFiring=1;observation.targets[2].hp=2000
SDGO_BattleAI_ProfileUpdate(npc);observation.selectedSlot=3;observation.specialSelected=1;observation.targets[2].hp=0
now=100;SDGO_BattleAI_ProfileUpdate(npc)
Check(calls.switch==1 and calls.fire==false,'special target death releases intent and returns to the prior ordinary weapon')
Reset({Difficulty=1});observation.nativeTarget=2;observation.specialAvailable=1;observation.specialRange=1800;observation.spReady=1
observation.targets[2].selectedTarget=20;observation.targets[2].rangedFiring=1;observation.targets[2].hp=2000
observation.hurt=1;observation.specialControlAllowed=1;SDGO_BattleAI_ProfileUpdate(npc)
Check(calls.switch==3,'a native style-2-permitted hurt window may request a special without overriding forbidden states')
observation.selectedSlot=3;observation.specialSelected=1;observation.specialControlAllowed=0;now=100;SDGO_BattleAI_ProfileUpdate(npc)
Check(calls.fire==false,'a queued special loses attack intent immediately when a forbidden control state arrives')
Reset({Difficulty=1});observation.nativeTarget=1;observation.specialAvailable=1;observation.specialRange=1800;observation.spReady=1
observation.hurt=1;observation.meleeBusy=1;observation.specialControlAllowed=1;observation.meleeAttacker=2
SDGO_BattleAI_ProfileUpdate(npc)
Check(calls.switch==3 and SDGO_BattleAI_Universal.profiles[20].special.target==2,
      'a player-permitted hurt cancel requests special against the actual melee attacker instead of the old front target')
Reset({Difficulty=0});observation.nativeTarget=2;observation.specialAvailable=1;observation.specialRange=1800;observation.spReady=1
SDGO_BattleAI_ProfileUpdate(npc)
Check(calls.switch==nil and calls.fire==nil,'Easy never adds special attack requests')
for _,difficulty in pairs({1,2}) do
  Reset({Difficulty=difficulty,GenericMode=0,DecisionMs=1000,ReactionMs=3000,
    TargetHoldMs=5000,WeaponHoldMs=5000,AttackIntervalMs=10000,BurstMs=1,BurstPauseMs=5000})
  Tick(0)
  Check(calls.fire==true and calls.cadence==0,'Medium/Hard first legal frame attacks despite legacy idle configuration: '..difficulty)
  Tick(1);Tick(2);Tick(3)
  Check(calls.fire==true and calls.native==1,'Medium/Hard does not reenter stock idle or burst pauses: '..difficulty)
  observation.targets[1].hp=1;Tick(4)
  Check(calls.target==1 and calls.fire==true,'Medium/Hard switches target and fires in the same decision: '..difficulty)
  observation.weapons[2].ready=0;observation.weapons[3]=Weapon(2,'projectile',2000,1,2000);Tick(5)
  Check(calls.switch==2 and not calls.fire,'a natively ready weapon beats a stronger cooling-down weapon immediately: '..difficulty)
  local before=calls.switches;Tick(6)
  Check(calls.switches==before+1,'uncommitted native weapon requests have no extra retry wait: '..difficulty)
  observation.selectedSlot=2;Tick(7)
  Check(calls.fire==true,'the first confirmed weapon frame fires without a reaction timer: '..difficulty)
  observation.hurt=1;Tick(8);Check(not calls.fire,'native hurt still blocks attacks: '..difficulty)
  observation.hurt=0;Tick(9)
  Check(calls.fire==true,'native hurt recovery resumes attack on its first legal frame: '..difficulty)
  SDGO_BattleAI_Universal.states[20].recoverUntil=10000;Tick(10)
  Check(calls.fire==true and SDGO_BattleAI_Universal.states[20].recoverUntil==nil,'an orphaned recovery timestamp without motor ownership is discarded: '..difficulty)
  observation.weapons[3].ready=0;Tick(11)
  Check(not calls.fire and calls.moves>0,'native weapon cooldown uses movement without inventing an attack: '..difficulty)
  Reset({Difficulty=difficulty});observation.targets={};Tick(0);local patrols=calls.patrol
  observation.goal=observation.pos;Tick(1)
  Check(calls.patrol==patrols+1,'arriving at a patrol goal selects the next goal without a patrol pause: '..difficulty)
  Reset({Difficulty=difficulty});observation.defenceMoving=1;Tick(0)
  Check(calls.fire==true and not calls.stop and not calls.cancelled,
        'ordinary firing never cancels the native front/rear escape route: '..difficulty)
end
Reset({Difficulty=1});calls.roll=1;observation.targets[2].rangedFiring=1
observation.targets[2].hurt=1;observation.targets[2].pos={x=1000,y=100,z=0}
calls.coverCandidate={valid=1,hide={x=0,y=0,z=0},peek={x=70,y=0,z=0}}
Tick(0)
Check(calls.fire==true and calls.moves==1 and calls.mode==UNIT_MOVE_EVENT_MOVE and calls.jumps==0 and
      not calls.coverQueries and not calls.rolls,
      'Medium uses continuous ground fire movement without Hard cover, boost, jump or probability tactics')
Reset({Difficulty=1});observation.rangedBusy=1;observation.routeActive=1;Tick(0)
Check(calls.fire==true and calls.moves==1 and calls.mode==UNIT_MOVE_EVENT_MOVE and calls.jumps==0 and
      calls.cancelled==1,'Medium moving-shot fire keeps a ground strafe during the active native ranged state')
Reset({Difficulty=1});observation.weapons[2].movingShot=0;Tick(0)
Check(calls.fire==true and calls.moves==0 and calls.stop==1,
      'Medium stationary-only weapon stops and fires instead of inheriting ground strafe')
Reset({Difficulty=1});observation.weapons[2].movingShot=0;observation.rangedBusy=1;Tick(0)
Check(calls.fire==true and calls.moves==0 and calls.stop==1,
      'Medium active stationary-only ranged state remains stopped while holding fire')
Reset();observation.targets[2].meleeApproaching=1;observation.weapons[2].movingShot=0;Tick(0)
observation.shots=1;observation.rangedBusy=1;observation.moveCancelOpen=1;Tick(1)
observation.rangedBusy=0;Tick(2)
Check(calls.fire==true and SDGO_BattleAI_Universal.states[20].orbUntil==nil,
      'stationary orbwalk fires on native readiness without waiting out its movement duration')
Reset({Difficulty=1});observation.nativeTarget=2;observation.specialAvailable=1;observation.specialRange=1800;observation.spReady=1
observation.targets[2].selectedTarget=20;observation.targets[2].rangedFiring=1;observation.targets[2].hp=2000
SDGO_BattleAI_ProfileUpdate(npc);observation.selectedSlot=3;observation.specialSelected=1
now=1;SDGO_BattleAI_ProfileUpdate(npc);now=2;SDGO_BattleAI_ProfileUpdate(npc)
Check(calls.fire==true,'a natively admitted special has no artificial 500ms request pause')
observation.specialBusy=1;now=3;SDGO_BattleAI_ProfileUpdate(npc)
observation.specialBusy=0;now=4;SDGO_BattleAI_ProfileUpdate(npc)
observation.selectedSlot=1;observation.specialSelected=0;now=5;SDGO_BattleAI_ProfileUpdate(npc)
now=6;SDGO_BattleAI_ProfileUpdate(npc)
Check(calls.switch==3,'native completion and restored SP can immediately choose the next special without a four-second lock')
Reset();calls.routeResult=0;observation.targets={Target(2,100,3000)};Tick(0)
observation.targets[1].distance=800;Tick(1)
Check(calls.target==2 and calls.fire==true,'a now-fireable enemy immediately escapes an old failed-route exclusion')
Reset({Difficulty=1});observation.nativeTarget=2;observation.specialAvailable=1;observation.specialRange=1800;observation.spReady=1
observation.targets[2].selectedTarget=20;observation.targets[2].rangedFiring=1;observation.targets[2].hp=2000
SDGO_BattleAI_ProfileUpdate(npc);observation.selectedSlot=3;observation.specialSelected=1
now=1;SDGO_BattleAI_ProfileUpdate(npc);observation.spReady=0;observation.specialFinished=1
now=2;SDGO_BattleAI_ProfileUpdate(npc)
Check(calls.switch==1,'authoritative special completion restores the gun even when the brief busy state was between decision frames')
Reset({Difficulty=1});observation.weapons[2].movingShot=0;observation.id=30
local badNpc={}
for key,value in pairs(npc) do badNpc[key]=value end
function badNpc:GetID() return 30 end
function badNpc:MoveStop() error('isolated unit failure') end
for time=1,2 do now=time;SDGO_BattleAI_GenericUpdate(badNpc) end
Check(SDGO_BattleAI_Universal.faults==2 and SDGO_BattleAI_Universal.failures[30].count==2,
      'policy errors accumulate only against the failing NPC generation')
badNpc.MoveStop=npc.MoveStop;now=3;SDGO_BattleAI_GenericUpdate(badNpc)
Check(SDGO_BattleAI_Universal.failures[30]==nil,'a successful decision clears the NPC consecutive-error counter')
function badNpc:MoveStop() error('isolated unit failure') end
for time=4,6 do now=time;SDGO_BattleAI_GenericUpdate(badNpc) end
local faults=SDGO_BattleAI_Universal.faults;local nativeCalls=calls.native
now=7;SDGO_BattleAI_GenericUpdate(badNpc)
Check(SDGO_BattleAI_Universal.faults==faults and calls.native==nativeCalls+1,
      'three consecutive failures quarantine only that NPC generation')
observation.id=20;now=8;SDGO_BattleAI_GenericUpdate(npc)
Check(calls.fire==true and SDGO_BattleAI_Universal.failures[20]==nil and SDGO_BattleAI_Universal.faults==faults,
      'one quarantined NPC does not disable another Medium or Hard unit')
generation=generation+1;observation.generation=generation;observation.id=30;now=9
SDGO_BattleAI_GenericUpdate(badNpc)
Check(SDGO_BattleAI_Universal.faults==faults+1 and SDGO_BattleAI_Universal.failures[30].generation==generation and
      SDGO_BattleAI_Universal.failures[30].count==1,'a new NPC generation may retry after an older generation was quarantined')
dofile('mods/battle_ai/universal_ai.lua')
Check(SDGO_BattleAI_Universal.faults==0 and next(SDGO_BattleAI_Universal.failures)==nil,
      'policy reload clears unit quarantine and permits a corrected script to retry')

for _,difficulty in pairs({1,2}) do
  Reset({Difficulty=difficulty});observation.defenceMoving=1;observation.routeActive=1
  for _,w in pairs(observation.weapons) do w.ammoReady=0 end
  local before=calls.routes;Tick(0)
  Check(not calls.fire and calls.routes==before and not calls.cancelled and observation.routeActive==1,
        'empty loadout preserves an ongoing defensive escape instead of replacing its path: '..difficulty)
end
Reset({Difficulty=1});observation.nativeTarget=2;observation.specialAvailable=1;observation.specialRange=1800;observation.spReady=1
Tick(0)
Check(not calls.switch and calls.fire==true,'Medium saves full SP when there is no actual pressure')
local function HardOpportunity()
  Reset({Difficulty=2});observation.nativeTarget=2;observation.specialAvailable=1;observation.specialRange=1800;observation.spReady=1
  observation.targets[1].hp=5000;observation.targets[2].hp=2000;observation.targets[2].hurt=1
end
HardOpportunity();Tick(0)
Check(calls.switch==3,'Hard confirms a controlled high-value opening for offensive special')
HardOpportunity();observation.targets[2].hp=1;Tick(0)
Check(not calls.switch and calls.fire==true,'Hard uses ordinary fire instead of wasting special on an estimated one-shot')
HardOpportunity();observation.allies={{id=40,contactTarget=2,contactAt=0,meleeSwinging=1}};Tick(0)
Check(not calls.switch and calls.fire==true,'Hard does not steal an allied melee catch with offensive special')
HardOpportunity();observation.targets[2].hurt=0;Tick(0)
Check(not calls.switch and calls.fire==true,'full SP alone is not an offensive special opportunity')
HardOpportunity();observation.defenceMoving=1;Tick(0)
Check(not calls.switch and calls.fire==true and not calls.cancelled,'non-emergency special does not cancel a crossfire escape')
local function DefensiveSpecial()
  Reset({Difficulty=1});observation.nativeTarget=2;observation.specialAvailable=1;observation.specialRange=1800;observation.spReady=1
  observation.targets[2].selectedTarget=20;observation.targets[2].rangedFiring=1;observation.targets[2].hp=2000
  SDGO_BattleAI_ProfileUpdate(npc);observation.selectedSlot=3;observation.specialSelected=1
end
DefensiveSpecial();observation.spReady=0;observation.specialAcknowledged=1;observation.specialAckSequence=1;now=1;calls.switch=nil
SDGO_BattleAI_ProfileUpdate(npc)
Check(not calls.switch and SDGO_BattleAI_Universal.profiles[20].special~=nil,
      'server-admitted special keeps ownership across a debit-before-busy observation gap')
observation.specialAcknowledged=0;now=2;SDGO_BattleAI_ProfileUpdate(npc)
Check(calls.switch==1,'unadmitted resource loss returns to normal weapons without a 2.2-second freeze')
DefensiveSpecial();observation.specialAcknowledged=1;observation.specialAckSequence=1;now=2201;calls.switch=nil;SDGO_BattleAI_ProfileUpdate(npc)
Check(calls.switch==1,'a lost completion cannot leave an acknowledged but natively non-busy special frozen forever')
DefensiveSpecial();observation.specialBusy=1;now=1;SDGO_BattleAI_ProfileUpdate(npc)
observation.specialBusy=0;observation.specialFinished=1;observation.weapons={Weapon(0,'melee',250,2000,600)}
now=2;calls.switch=nil;SDGO_BattleAI_ProfileUpdate(npc)
Check(SDGO_BattleAI_Universal.profiles[20].special==nil and not calls.switch,
      'a transformed-away restoration slot releases the lease immediately')
DefensiveSpecial();observation.specialBusy=1;now=1;SDGO_BattleAI_ProfileUpdate(npc)
observation.specialBusy=0;observation.specialFinished=1;now=2;SDGO_BattleAI_ProfileUpdate(npc)
now=253;SDGO_BattleAI_ProfileUpdate(npc)
Check(SDGO_BattleAI_Universal.profiles[20].special==nil,
      'permanent native restoration rejection has a bounded handoff rather than an infinite lease')
SDGO_BattleAI_ProfilePreviousSlot=function() return 1 end
SDGO_BattleAI_BoundSlot=function() return 3,1,0 end
SDGO_BattleAI_ProfileLease=function(slot) calls.lease=slot end
calls.switch=nil;SDGO_BattleAI_ProfileRelease(npc)
Check(calls.lease==-1 and not calls.switch,'Easy handoff releases a previous slot missing from the committed form')
SDGO_BattleAI_ProfilePreviousSlot=nil;SDGO_BattleAI_BoundSlot=nil;SDGO_BattleAI_ProfileLease=nil

Reset();observation.weapons[3]=Weapon(2,'projectile',1800,5000,50);Tick(0)
Check(not calls.switch and calls.fire==true,'an in-hand ready finisher does not pay a switch cost for a larger theoretical DPS score')
Reset();observation.selectedSlot=2;observation.targets[1].hp=5000;observation.targets[2].hp=5000
observation.weapons[3]=Weapon(2,'hitscan',1800,700,200);Tick(0)
Check(not calls.switch and calls.fire==true,'equal-value ready weapons keep the committed slot rather than switching by slot number')
Reset();calls.roll=1;observation.targets[2].pos={x=1000,y=100,z=0};calls.coverCandidate={pending=1};Tick(0)
local rolls=calls.rolls;Tick(1)
Check(calls.fire==true and calls.coverQueries==2 and calls.rolls==rolls,
      'bounded pending cover search continues firing and does not reroll its opportunity each frame')
calls.coverCandidate={valid=1,hide={x=0,y=0,z=0},peek={x=70,y=0,z=0}};Tick(2)
Check(SDGO_BattleAI_Universal.states[20].cover and calls.routeX==70,
      'a completed incremental cover search hands over to its verified peek route')
Reset();calls.roll=1;observation.targets[2].pos={x=1000,y=100,z=0}
calls.coverCandidate={valid=1,hide={x=0,y=0,z=0},peek={x=70,y=0,z=0}};Tick(0)
observation.routeActive=0;calls.routeResult=0;Tick(1)
local peekFailures=calls.routes;Tick(2)
Check(SDGO_BattleAI_Universal.states[20].cover==nil and calls.routes==peekFailures and calls.fire==true,
      'a failed peek-route retry abandons the opportunity instead of stopping the motor on every frame')

Reset();Tick(0)
local memory=SDGO_BattleAI_Universal.states[20].memory[2]
observation.targets[2].temporary=true;Tick(1)
observation.targets[2].temporary=nil;observation.targets[2].hp=150;Tick(2)
Check(SDGO_BattleAI_Universal.states[20].memory[2]==memory and memory.hp==150 and memory.temporary==nil,
      'target memory reuses its table without retaining removed observation fields')

Reset({Difficulty=1});Tick(0);observation.moving=1;observation.routeActive=1;calls.staticRouteObservation=true
local priorMoves=calls.moves;Tick(1)
Check(calls.moves==priorMoves+1 and calls.cancelled>0,
      'Medium takes the motor back from an intervening pursuit route without waiting for its strafe timer')
Reset({Difficulty=2});observation.targets[2].rangedFiring=1;Tick(0)
observation.moving=1;observation.routeActive=1;calls.staticRouteObservation=true
priorMoves=calls.moves;Tick(1)
Check(calls.moves==priorMoves+1 and calls.cancelled>0,
      'Hard resumes direct evasion after cancelling an old pursuit route within the same strafe phase')

DefensiveSpecial();observation.specialAcknowledged=1;observation.specialAckSequence=1;now=2201
SDGO_BattleAI_ProfileUpdate(npc);observation.selectedSlot=1;observation.specialSelected=0;now=2202;SDGO_BattleAI_ProfileUpdate(npc)
now=2203;SDGO_BattleAI_ProfileUpdate(npc);observation.selectedSlot=3;observation.specialSelected=1;now=2204;calls.fire=nil
SDGO_BattleAI_ProfileUpdate(npc)
Check(calls.fire==true,'an old acknowledged cast cannot suppress admission input for a new cast after watchdog handoff')
HardOpportunity();Tick(0);observation.selectedSlot=3;observation.specialSelected=1
observation.allies={{id=40,contactTarget=2,contactAt=1,meleeSwinging=1}};Tick(1)
Check(calls.switch==1 and calls.fire==false,'Hard cancels an unadmitted offensive special when an ally catches the target during weapon selection')
HardOpportunity();Tick(0);observation.selectedSlot=3;observation.specialSelected=1;observation.targets[2].hp=1;Tick(1)
Check(calls.switch==1 and calls.fire==false,'Hard rechecks ordinary finishing value before admitting its pending offensive special')
Reset();calls.roll=1;observation.targets[2].pos={x=1000,y=100,z=0}
calls.coverCandidate={valid=1,hide={x=0,y=0,z=0},peek={x=70,y=0,z=0}};Tick(0)
observation.pos={x=70,y=0,z=0};observation.shots=1;Tick(1)
observation.routeActive=0;observation.moving=0;calls.routeResult=0;Tick(2)
Check(SDGO_BattleAI_Universal.states[20].cover==nil and calls.fire==true,
      'a lost tuck route is rebuilt or abandoned immediately instead of waiting for its cover timeout')
Reset({Difficulty=1});local lease=1
SDGO_BattleAI_ProfilePreviousSlot=function() return lease end
SDGO_BattleAI_BoundSlot=function() return 3,7,0 end
SDGO_BattleAI_ProfileLease=function(slot) lease=slot end
now=0;SDGO_BattleAI_ProfileRelease(npc);now=251;SDGO_BattleAI_ProfileRelease(npc)
calls.fire=true;now=252;SDGO_BattleAI_ProfileRelease(npc)
Check(lease==-1 and calls.fire==true,'Easy or disable handoff cannot keep clearing attack after permanent restoration rejection')
SDGO_BattleAI_ProfilePreviousSlot=nil;SDGO_BattleAI_BoundSlot=nil;SDGO_BattleAI_ProfileLease=nil

for _,difficulty in pairs({1,2}) do
  Reset({Difficulty=difficulty});observation.moving=1;observation.targets[2].rangedFiring=1
  Tick(0)
  for time=100,1500,100 do observation.pos={x=0,y=time/20,z=0};Tick(time) end
  local state=SDGO_BattleAI_Universal.states[20]
  Check(state.recovering and calls.recoveryMode==1 and calls.mode==UNIT_MOVE_EVENT_MOVE and calls.jumpSeconds==0,
        'ready ranged AI with no horizontal progress releases thrust and enters ground recovery: '..difficulty)
  Check(calls.fire==true,'legal moving-shot output continues during wall recovery: '..difficulty)
  local dashCount=calls.dashes or 0;local jumpCount=calls.positiveJumps or 0;local deadline=state.recoverUntil
  observation.moving=0;Tick(1600)
  Check(state.recoverUntil==deadline and state.recovering and calls.recoveryMode==1 and
        (calls.dashes or 0)==dashCount and (calls.positiveJumps or 0)==jumpCount,
        'ready weapon and a cleared motor flag cannot preempt recovery with more dash or lift: '..difficulty)
  observation.goal=observation.pos;Tick(1700)
  Check(not state.recovering and calls.recoveryMode==0,'arrival releases recovery motor ownership: '..difficulty)
end
Reset();observation.moving=1;observation.rangedBusy=1;observation.weapons[2].active=1
observation.weapons[2].ammoReady=0;observation.weapons[2].ready=0;Tick(0);Tick(1500)
Check(SDGO_BattleAI_Universal.states[20].recovering and calls.fire==true and calls.jumpSeconds==0,
      'active native moving-shot ranged state cannot bypass the wall watchdog')
Reset();observation.moving=1;observation.rangedBusy=1;observation.weapons[2].movingShot=0
observation.moveCancelOpen=0;Tick(0);Tick(1500)
Check(not SDGO_BattleAI_Universal.states[20].recovering and calls.fire==true and calls.recoveryMode~=1,
      'wall watchdog does not cancel a stationary-only native action before its legal window')
observation.moveCancelOpen=1;Tick(1501)
Check(SDGO_BattleAI_Universal.states[20].recovering and calls.recoveryMode==1,
      'the first legal movement-cancel window admits pending wall recovery')
for _,branch in pairs({'empty','switch','defence','flank'}) do
  Reset();observation.moving=1
  if branch=='empty' then for _,w in pairs(observation.weapons) do w.ammoReady=0 end
  elseif branch=='switch' then observation.selectedSlot=0
  elseif branch=='defence' then observation.defenceMoving=1;observation.routeActive=1
  else
    local target=observation.targets[2];target.pos={x=1000,y=100,z=0};target.groundPos={x=1000,y=0,z=0};target.forward={x=1,y=0,z=0};target.hurt=1
    observation.allies={{id=40,contactTarget=2,contactAt=0,meleeSwinging=1}}
  end
  Tick(0);Tick(1500)
  Check(SDGO_BattleAI_Universal.states[20].recovering and calls.recoveryMode==1 and calls.mode==UNIT_MOVE_EVENT_MOVE,
        'wall watchdog runs before the '..branch..' early-return movement branch')
end
Reset();observation.moving=1;Tick(0);Tick(1500)
local timed=SDGO_BattleAI_Universal.states[20];local expires=timed.recoverUntil
Tick(expires+1)
Check(not timed.recovering and calls.recoveryMode==0,'recovery expiry cannot permanently monopolize movement')

Reset();observation.targets={Target(2,200,1000)};observation.targets[1].hurt=1;Tick(0)
Check(calls.target==2 and calls.fire==true,
      'ordinary hit-stun remains a legal attack and combo target when native damage protection is absent')
Reset();observation.targets[2].damageProtected=1;Tick(0)
Check(calls.target==1 and calls.fire==true,
      'a protected low-HP target yields immediately to another legal enemy')
Reset();observation.targets={Target(2,200,1000)}
local protected=observation.targets[1];protected.damageProtected=1
protected.pos={x=1000,y=100,z=0};protected.groundPos={x=1000,y=0,z=0};Tick(0)
Check(calls.fire==false and calls.aim==2 and calls.target==2 and calls.moves>0 and
      calls.mode==UNIT_MOVE_EVENT_MOVE and (calls.positiveJumps or 0)==0,
      'a sole protected target is faced and circled on foot without invulnerable attack output')
Reset();observation.targets={Target(2,200,3000)}
protected=observation.targets[1];protected.damageProtected=1
protected.pos={x=3000,y=100,z=0};protected.groundPos={x=3000,y=0,z=0};Tick(0)
Check(calls.fire==false and calls.routes==1 and calls.routeX>1700 and calls.routeX<2000 and
      calls.mode==UNIT_MOVE_EVENT_MOVE and SDGO_BattleAI_Universal.states[20].movementIntent,
      'a distant protected target uses native walking navigation to a weapon-range staging point')
Reset();observation.targets={Target(2,200,100)}
protected=observation.targets[1];protected.damageProtected=1
protected.pos={x=100,y=100,z=0};protected.groundPos={x=100,y=0,z=0};Tick(0)
Check(calls.fire==false and calls.direction==UNIT_MOVE_DIR_BACKWARD and calls.mode==UNIT_MOVE_EVENT_MOVE,
      'a protected target inside the current weapon spacing causes a fuel-free walking retreat')
Reset();observation.targets={Target(2,200,1000)};Tick(0)
observation.targets[1].damageProtected=1;observation.targets[1].visible=0;calls.fire=nil;Tick(1)
Check(SDGO_BattleAI_Universal.states[20].memory[2]==nil and calls.fire==false and calls.aim==0 and
      (calls.patrol or 0)>0,
      'live protection deletes stale target memory instead of resurrecting an invulnerable chase')
Reset();Tick(0)
observation.targets[2].damageProtected=1;observation.meleeBusy=1;observation.selectedSlot=0
calls.fire=nil;calls.meleeGate=nil;Tick(1)
Check(calls.target==1 and calls.fire==false and calls.meleeGate==1,
      'a protected held melee target cannot override retargeting or request another combo segment')
observation.meleeBusy=0;Tick(2)
Check(calls.meleeGate==0 and calls.switch==1,
      'protected-target melee suppression releases for the first legal post-animation weapon request')
observation.selectedSlot=1;Tick(3)
Check(calls.fire==true,'retargeted ranged fire starts on the first committed post-protection weapon frame')
Reset();observation.targets={Target(2,200,1000)};Tick(0)
observation.targets[1].damageProtected=1;observation.rangedBusy=1;observation.weapons[2].active=1
calls.fire=nil;Tick(1)
Check(calls.fire==false and calls.aim==2,
      'an active native ranged state stops held attack input while its sole target is protected')
DefensiveSpecial();observation.targets[2].damageProtected=1;now=1;calls.switch=nil;calls.fire=nil
SDGO_BattleAI_ProfileUpdate(npc)
Check(calls.switch==1 and calls.fire==false,
      'a pending special returns to its ordinary weapon when native target protection begins')
Reset();observation.targets={Target(2,200,1000)}
protected=observation.targets[1];protected.damageProtected=1;protected.hurt=1
protected.pos={x=1000,y=100,z=0};protected.groundPos={x=1000,y=0,z=0};Tick(0)
protected.damageProtected=0;calls.fire=nil;Tick(1)
Check(calls.fire==true and calls.target==2,
      'protection expiry resumes attack immediately even while ordinary target hit-stun remains active')

for _,difficulty in pairs({1,2}) do
  Reset({Difficulty=difficulty});observation.moving=0;observation.targets[2].rangedFiring=1
  Tick(0)
  for time=100,1500,100 do Tick(time) end
  Check(SDGO_BattleAI_Universal.states[20].recovering and calls.recoveryMode==1,
        'repeated policy movement without a native moving flag still accumulates horizontal stagnation: '..difficulty)
end

Reset();observation.targets[2].rangedFiring=1;Tick(0)
local previousBoosts=calls.dashes or 0;observation.moving=1;observation.boostFraction=0.1;Tick(1)
Check(calls.mode==UNIT_MOVE_EVENT_MOVE and calls.jumpSeconds==0 and (calls.dashes or 0)==previousBoosts,
      'fuel exhaustion releases held thrust and lift before the next lateral-switch timer')
Reset({Difficulty=1});observation.selectedSlot=0;observation.targets={Target(1,2000,900)}
Tick(0)
Check(calls.switch==1 and calls.routes>0 and calls.fire==false,
      'uncommitted ranged switch approaches an enemy instead of circling forever')
Reset({Difficulty=1});observation.weapons[3]=Weapon(2,'hitscan',1400,10,500)
observation.selectedSlot=2;observation.targets={Target(1,2000,900)}
Tick(0)
Check(calls.switch==1 and calls.fire==true,
      'a pending better weapon does not silence the currently bound legal gun')
local submitted=nil
SDGO_BattleAI_RequestWeapon=function(slot) submitted=slot;return 1 end
Tick(1)
Check(submitted==1 and calls.fire==true,'native request bridge keeps actual-slot firing while waiting for commit')
SDGO_BattleAI_RequestWeapon=nil
print(string.format('ALL %d UNIVERSAL CONTROLLER CHECKS PASSED (Lua 5.0)',checks))
