-- Capability-controller scenarios in real Lua 5.0. No game service or account.
local now,generation,observation=0,1,nil
local checks=0
local calls={}
UNIT_MOVE_DIR_LEFT=3;UNIT_MOVE_DIR_RIGHT=4;UNIT_MOVE_DIR_BACKWARD=2
UNIT_MOVE_EVENT_MOVE=1;UNIT_MOVE_EVENT_DASH=4
function SDGO_BattleAI_Now() return now end
function SDGO_BattleAI_Authority() return 1 end
function SDGO_BattleAI_Observe() return observation end
function SDGO_BattleAI_Native() calls.native=calls.native+1; return 123 end
function SDGO_BattleAI_AimTarget(id) calls.aim=id end
function SDGO_BattleAI_Cadence(ms) calls.cadence=ms end
function SDGO_BattleAI_MeleeGate(value) calls.meleeGate=value end
function SDGO_BattleAI_Chance(percent) calls.rolls=(calls.rolls or 0)+1;return calls.roll or 0 end
function SDGO_BattleAI_Cover(id,x,y,z) calls.coverQueries=(calls.coverQueries or 0)+1;calls.coverX=x;return calls.coverCandidate end
function SDGO_BattleAI_CancelRoute() calls.cancelled=(calls.cancelled or 0)+1 end
function SDGO_BattleAI_Route(id,force,x,y,z)
  calls.routes=calls.routes+1; calls.routeId=id;calls.force=force
    calls.routeX=x
  return calls.routeResult or 1
end
function SDGO_BattleAI_Log(text) calls.error=text end
local npc={}
function npc:CheckHurt() return observation.hurt end
function npc:SetAttackTarget(id) calls.target=id end
function npc:SetAttackOnOff(on) calls.fire=on end
function npc:MoveStop() calls.stop=(calls.stop or 0)+1 end
function npc:SetMoveTargetPosRandom() calls.patrol=(calls.patrol or 0)+1 end
function npc:ChangeWeapon(slot) calls.switch=slot end
function npc:SetMoveTargetPos_Dir(dir,distance,facing,jump)
  assert(dir==facing and jump==false and distance>0)
  calls.moves=calls.moves+1;calls.direction=dir
end
function npc:SetMoveType(mode) calls.mode=mode end
function npc:SetJumpTime(seconds) calls.jumps=calls.jumps+1;calls.jumpSeconds=seconds end
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
Check(calls.target==1,'short target hold avoids cancelling a shot immediately')
Tick(700)
Check(calls.target==2,'lowest HP wins once hold expires')
observation.targets[2].hp=0;Tick(800)
Check(calls.target==1,'death invalidates held target immediately')
Reset();observation.weapons[2].range=4000;observation.targets[1].distance=500
observation.targets[2].hp=2000;Tick(0)
observation.targets[2].hp=1;observation.targets[2].distance=2800;Tick(700)
Check(calls.target==1,'extreme distance gap does not pull an ongoing close engagement away')
Reset();observation.targets={Target(1,1000,100)};Tick(0)
Check(calls.switch==0 and calls.fire==false,'close opponent selects data-driven melee and waits for native switch')
observation.selectedSlot=0;Tick(100);Tick(300)
Check(calls.fire==true,'acknowledged melee weapon attacks after reaction window')
Reset();observation.selectedSlot=0;Tick(0);Tick(300)
Check(calls.switch==1 and calls.fire==false,'switch request is not treated as immediate native acknowledgement')
observation.selectedSlot=1;Tick(400);Tick(600)
Check(calls.fire==true and calls.cadence==120,'confirmed ranged weapon requests configured attack intent cadence')
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
Check(calls.fire==false,'configured burst duration creates an intent pause')
Tick(500)
Check(calls.fire==true,'attack intent resumes after configured pause')
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
Reset({ReactionMs=0});local t=observation.targets[2]
t.pos={x=1000,y=100,z=0};t.groundPos={x=1000,y=0,z=0};t.forward={x=1,y=0,z=0};t.hurt=1
observation.allies={{id=40,meleeSwinging=1,contactTarget=2,contactAt=0}};Tick(0)
Check(calls.routeX==650 and calls.fire==true,'ally delivered melee contact plus active victim hurt requests a rear route while continuing gunfire')
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
Reset({ReactionMs=0});calls.roll=1;t=observation.targets[2];t.pos={x=1000,y=100,z=0}
calls.coverCandidate={valid=1,hide={x=0,y=0,z=0},peek={x=70,y=0,z=0}};observation.shots=0;Tick(0)
Check(calls.routeX==70 and calls.fire==true,'selected cover opportunity moves to the sampled smallest fireable edge')
observation.pos={x=70,y=0,z=0};Tick(100)
Check(SDGO_BattleAI_Universal.states[20].cover.stage=='peek','cover does not retract merely because attack intent was requested')
observation.shots=1;observation.rangedBusy=1;observation.moveCancelOpen=0;observation.weapons[2].movingShot=0;Tick(200)
Check(calls.routeX==70 and calls.fire==false,'stationary shot waits for native movement-cancel window before retracting')
observation.moveCancelOpen=1;Tick(300)
Check(calls.routeX==0 and SDGO_BattleAI_Universal.states[20].cover.stage=='tuck','actual primary shot and legal cancel window retract to the covered anchor')
calls.routeResult=0;SDGO_BattleAI_Universal.states[20].cover.routeAt=400;Tick(400)
Check(SDGO_BattleAI_Universal.states[20].cover.routeAt==600,'failed cover retraction retains a bounded retry instead of silently abandoning its route')
calls.routeResult=1;Tick(600)
Check(SDGO_BattleAI_Universal.states[20].cover.routeAt==nil,'cover retries a transiently unavailable return route')
Reset({ReactionMs=0});t=observation.targets[2];t.pos={x=1000,y=100,z=0};calls.roll=0;Tick(0)
Tick(100)
Check((calls.coverQueries or 0)==0,'rejected cover opportunity performs no expensive geometry query or per-frame reroll')
Check(calls.rolls==1,'cover selection probability is rolled once per opportunity cooldown')
Reset({ReactionMs=0});observation.targets={Target(2,100,1000)};t=observation.targets[1]
t.pos={x=1000,y=100,z=0};t.groundPos={x=1000,y=0,z=0};Tick(0)
t.visible=0;t.pos={x=3000,y=100,z=0};calls.roll=1
calls.coverCandidate={valid=1,hide={x=0,y=0,z=0},peek={x=70,y=0,z=0}};Tick(1700)
Check(calls.routeX==70 and calls.coverX==1000 and calls.fire==false,'occluded cover search uses frozen last-visible position and waits for a fresh firing ray')
Reset({ReactionMs=0});t=observation.targets[2];t.meleeTracking=1;observation.weapons[2].movingShot=0;observation.shots=0;Tick(0)
observation.shots=1;observation.rangedBusy=1;observation.moveCancelOpen=0;Tick(100)
Check(calls.moves==0,'orbwalk cannot cancel an unopened native shooting animation')
observation.moveCancelOpen=1;Tick(200)
Check(calls.moves==1 and calls.fire==false,'opened native animation gate permits one short step after a real shot')
observation.rangedBusy=0;observation.weapons[2].ready=0;Tick(400)
Check(calls.fire==false,'orbwalking does not bypass the true weapon cooldown')
observation.weapons[2].ready=1;Tick(500)
Check(calls.fire==true,'orbwalking resumes firing when the weapon is natively ready')
Reset({Difficulty=1});observation.nativeTarget=2;observation.specialAvailable=1;observation.specialRange=1800;observation.spReady=0
SDGO_BattleAI_ProfileUpdate(npc)
Check(calls.switch==nil,'Medium cannot use the NPC unconditional SP override without the player resource gate')
observation.spReady=1;observation.hurt=1;SDGO_BattleAI_ProfileUpdate(npc)
Check(calls.switch==nil,'Medium cannot launch a special while the player hurt gate forbids it')
observation.hurt=0;SDGO_BattleAI_ProfileUpdate(npc)
Check(calls.switch==3 and calls.fire==false,'Medium requests the native special slot after legal hurt recovery')
now=100;observation.selectedSlot=3;observation.specialSelected=1;SDGO_BattleAI_ProfileUpdate(npc)
Check(calls.fire==true,'confirmed native special selection emits attack intent through the ordinary action path')
now=200;observation.spReady=0;SDGO_BattleAI_ProfileUpdate(npc)
Check(calls.fire==false,'SP debit stops further special request pulses while native authorization is pending')
observation.specialBusy=1;SDGO_BattleAI_ProfileUpdate(npc);observation.specialBusy=0;now=1000;SDGO_BattleAI_ProfileUpdate(npc)
Check(calls.switch==1,'completed native special returns to the prior ordinary weapon')
Check(SDGO_BattleAI_Universal.profiles[20].special.returning,'special restore retains ownership until native weapon selection is acknowledged')
now=1100;observation.selectedSlot=0;observation.specialSelected=0;SDGO_BattleAI_ProfileUpdate(npc)
Check(SDGO_BattleAI_Universal.profiles[20].special~=nil,'an ordinary but different slot does not falsely acknowledge restoration')
observation.selectedSlot=1;now=1200;SDGO_BattleAI_ProfileUpdate(npc)
Check(SDGO_BattleAI_Universal.profiles[20].special==nil,'confirmed previous ordinary slot releases the special lease')
Reset({Difficulty=1});observation.nativeTarget=2;observation.specialAvailable=1;observation.specialRange=1800;observation.spReady=1
SDGO_BattleAI_ProfileUpdate(npc);observation.selectedSlot=3;observation.specialSelected=1;observation.targets[2].hp=0
now=100;SDGO_BattleAI_ProfileUpdate(npc)
Check(calls.switch==1 and calls.fire==false,'special target death releases intent and returns to the prior ordinary weapon')
Reset({Difficulty=1});observation.nativeTarget=2;observation.specialAvailable=1;observation.specialRange=1800;observation.spReady=1
observation.hurt=1;observation.specialControlAllowed=1;SDGO_BattleAI_ProfileUpdate(npc)
Check(calls.switch==3,'a native style-2-permitted hurt window may request a special without overriding forbidden states')
observation.selectedSlot=3;observation.specialSelected=1;observation.specialControlAllowed=0;now=100;SDGO_BattleAI_ProfileUpdate(npc)
Check(calls.fire==false,'a queued special loses attack intent immediately when a forbidden control state arrives')
Reset({Difficulty=0});observation.nativeTarget=2;observation.specialAvailable=1;observation.specialRange=1800;observation.spReady=1
SDGO_BattleAI_ProfileUpdate(npc)
Check(calls.switch==nil and calls.fire==nil,'Easy never adds special attack requests')
print(string.format('ALL %d UNIVERSAL CONTROLLER CHECKS PASSED (Lua 5.0)',checks))
