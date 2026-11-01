-- Capability-driven controller. The native adapter supplies the current form's
-- weapon records and target observations; no VSAI family or machine-ID policy.
local D={GenericMode=1,Difficulty=1,Enabled=1,DecisionMs=0,ReactionMs=0,TargetHoldMs=0,
  FarTargetSlack=900,FarTargetRatioPercent=200,WeaponHoldMs=0,AttackIntervalMs=0,
  BurstMs=0,BurstPauseMs=0,AirStrafe=1,StrafeSwitchMs=450,StrafeDistance=400,
  JumpMs=600,JumpCooldownMs=1500,BoostReservePercent=20,ThreatMemoryMs=1600,
  StuckMs=1400,ProgressDistance=30,ArrivalDistance=100,RecoveryMs=650,
  RecoveryCooldownMs=1000,RecoveryDistance=350,RepathMs=700,TargetMemoryMs=2500,PatrolMs=1600,LogDecisions=0,
  CoverChance=90,CoverSearchMs=1600,CoverTimeoutMs=2200,OrbwalkStep=120,OrbwalkMs=180,FlankMs=750}
local C={}
for k,v in pairs(D) do C[k]=v end
for k,v in pairs(config or SDGO_BattleAI_TestConfig or {}) do C[k]=v end
-- Old player configuration must not reintroduce an artificial combat pause.
if C.Difficulty>=1 then
  C.GenericMode=1;C.AttackIntervalMs=0
end
local U=SDGO_BattleAI_Universal or {states={},faults=0}
local function Flag(v) return v~=nil and v~=false and v~=0 end
local function Min(a,b) if a<b then return a else return b end end
local function Max(a,b) if a>b then return a else return b end end
local function Distance2(a,b)
  local x,y,z=a.x-b.x,a.y-b.y,a.z-b.z
  return x*x+y*y+z*z
end
local function Log(s,reason,now)
  if C.LogDecisions==1 and s.reason~=reason then
    SDGO_BattleAI_Log(string.format('generic NPC %d: %s',s.id,reason))
  end
  s.reason=reason
end
local function DirectMove(npc,o,dir,distance,mode)
  if Flag(o.defenceMoving) then return false end
  -- A direct step owns the motor now. Otherwise Think's trailing FollowPath
  -- can immediately overwrite this command with an old pursuit waypoint.
  SDGO_BattleAI_CancelRoute()
  npc:SetMoveTargetPos_Dir(dir,distance,dir,false)
  npc:SetMoveType(mode)
  return true
end
local function State(o,now)
  local s=U.states[o.id]
  if not s or s.generation~=o.generation or now-s.seen>6000 then
    s={id=o.id,generation=o.generation,seen=now,decisionAt=0,targetAt=0,weaponAt=0,
      turnAt=0,hopAt=0,repathAt=0,progressAt=now,anchor=o.pos,attempt=0,recoveryAt=0,memory={},blocked={},patrolAt=0}
    U.states[o.id]=s
  end
  s.seen=now
  if not s.blocked then s.blocked={} end
  if s.unitId~=o.unitId or s.alternate~=o.alternate then
    s.unitId=o.unitId;s.alternate=o.alternate;s.slot=nil;s.weaponAt=0;s.readyAt=nil
    s.weave=nil;s.cover=nil;s.orbUntil=nil
  end
  if not U.sweepAt or now>=U.sweepAt then
    U.sweepAt=now+2000
    for id,old in pairs(U.states) do
      if now-old.seen>6000 then U.states[id]=nil
      else for target,memory in pairs(old.memory) do
        if now-memory.seen>C.TargetMemoryMs+1000 then old.memory[target]=nil end
      end end
    end
  end
  return s
end
local function Usable(w)
  return Flag(w.valid) and w.range and w.range>0 and (Flag(w.ammoReady) or Flag(w.active))
end
local function InRange(w,t)
  if not Usable(w) or t.distance>w.range then return false end
  return Flag(t.visible) and Flag(t.attackable)
end
local function AnyInRange(o,t)
  for _,w in pairs(o.weapons) do if InRange(w,t) then return true end end
  return false
end
local function Candidate(o,s,t,now)
  return t.id~=s.id and t.hp>0 and Flag(t.enemy) and Flag(t.attackable) and
    (not s.blocked[t.id] or now>=s.blocked[t.id] or AnyInRange(o,t))
end
local function PickTarget(o,s,now)
  local current=nil
  for _,t in pairs(o.targets) do
    if t.id==s.target and Candidate(o,s,t,now) then current=t end
    if Candidate(o,s,t,now) and Flag(t.visible) then
      local copy=s.memory[t.id] or {}
      for key in pairs(copy) do if key~='seen' and t[key]==nil then copy[key]=nil end end
      for key,value in pairs(t) do copy[key]=value end
      copy.seen=now;s.memory[t.id]=copy
    elseif t.hp<=0 then s.memory[t.id]=nil end
  end
  local currentRange=current and AnyInRange(o,current)
  local best,near=nil,nil
  for _,t in pairs(o.targets) do
    if Candidate(o,s,t,now) and Flag(t.visible) then
      if not near or t.distance<near.distance or (t.distance==near.distance and t.id<near.id) then near=t end
      local tooFar=currentRange and t.distance>current.distance+C.FarTargetSlack and
        t.distance*100>current.distance*C.FarTargetRatioPercent
      if AnyInRange(o,t) and not tooFar and
         (not best or t.hp<best.hp or (t.hp==best.hp and
          (t.distance<best.distance or (t.distance==best.distance and t.id<best.id)))) then best=t end
    end
  end
  best=best or (currentRange and current) or near
  if not best and s.target then
    local last=s.memory[s.target]
    if last and now-last.seen<C.TargetMemoryMs and Candidate(o,s,last,now) then
      local copy={}
      for key,value in pairs(last) do copy[key]=value end
      copy.visible=0;copy.remembered=true;best=copy
    end
  end
  return best
end
local function WeaponScore(w,t,threat)
  if not Usable(w) then return -1 end
  local cycle=Max(w.cycleMs or 300,50)
  local score=Max(w.damage or 1,1)*1000/cycle
  if t.distance>w.range then score=score*0.15*w.range/Max(t.distance,1) end
  if w.kind=='melee' then
    if t.distance<=w.range*0.9 then score=score*1.2 else score=score*0.6 end
  elseif t.distance<w.range*0.08 then score=score*0.6 end
  if threat and Flag(w.movingShot) then score=score*1.15 end
  if Flag(w.ready)==false then score=score*0.85 end
  return score
end
local function PickWeapon(o,s,t,threat,now,forceKind)
  local best,current=nil,nil
  local score,tier=-1,-1
  for _,w in pairs(o.weapons) do
    local allowed=not forceKind or (forceKind=='melee' and w.kind=='melee') or (forceKind=='ranged' and w.kind~='melee' and w.kind~='other')
    local value=allowed and WeaponScore(w,t,threat) or -1
    if allowed and w.slot==o.selectedSlot and Usable(w) then current=w end
    local rank=InRange(w,t) and (Flag(w.ready) and 2 or 1) or 0
    if value>=0 and (rank>tier or (rank==tier and
       (value>score or (value==score and best and w.slot<best.slot)))) then
      best=w;score=value;tier=rank
    end
  end
  if score<0 then return nil end
  -- Do not pay a native switch/animation cost for a tie or to replace a shot
  -- that can already finish the current target. Cooldown/empty/range changes
  -- still select a newly actionable weapon immediately.
  if current and InRange(current,t) and Flag(current.ready) and not forceKind and
     ((current.damage or 0)>=t.hp or WeaponScore(current,t,threat)>=score) then return current end
  -- With no fireable weapon, do not churn between cooling-down slots. The
  -- first genuinely ready option always wins on the next native observation.
  if tier<2 and current and InRange(current,t) then return current end
  return best
end
local function Chance(percent)
  return SDGO_BattleAI_Chance and SDGO_BattleAI_Chance(percent)==1
end
local function MeleeGate(value)
  if SDGO_BattleAI_MeleeGate then SDGO_BattleAI_MeleeGate(value and 1 or 0) end
end
local function Support(o,t,now)
  for _,a in pairs(o.allies or {}) do
    if a.rangedTarget==t.id and a.rangedAt and now-a.rangedAt>=0 and now-a.rangedAt<=1100 and
       a.pos and t.groundPos and Distance2(a.pos,t.groundPos)<=2500*2500 then return true end
  end
  return false
end
local function Weave(npc,o,s,t,now)
  if C.Difficulty<2 then s.weave=nil;MeleeGate(false);return nil,false end
  local function HasRanged()
    for _,w in pairs(o.weapons) do
      if Usable(w) and w.kind~='melee' and w.kind~='other' and t.distance<=w.range then return true end
    end
    return false
  end
  local q=s.weave
  if q and (q.target~=t.id or now-q.started>6500 or not Flag(t.visible)) then s.weave=nil;q=nil end
  if not q and Flag(o.meleeSwinging) and Flag(o.comboKnown) and o.comboMax>=2 and
     o.comboIndex==o.comboMax-2 and o.contactTarget==t.id and now-(o.contactAt or 0)<=650 and
     o.contactCombo==o.comboIndex and o.contactAction==o.actionHandle and
     Flag(t.hurt) and Support(o,t,now) then
    if HasRanged() then
      q={stage='release',target=t.id,started=now,shots=o.shots or 0,goal=Chance(50) and 1 or 2}
      s.weave=q
    end
  end
  MeleeGate(Flag(o.meleePressure) or (q and q.stage=='release'))
  if not q then return Flag(o.meleePressure) and 'ranged' or nil,false end
  if q.stage=='release' then
    npc:SetAttackOnOff(false)
    if Flag(o.meleeBusy) then return nil,true end
    q.shots=o.shots or 0;s.readyAt=nil;s.weaponAt=0
    if HasRanged() then
      q.stage='shoot'
      DirectMove(npc,o,UNIT_MOVE_DIR_BACKWARD,120,UNIT_MOVE_EVENT_MOVE)
    else q.stage='return' end
    MeleeGate(Flag(o.meleePressure))
  end
  if q.stage=='shoot' then
    local fired=(o.shots or 0)-q.shots
    if fired<q.goal and HasRanged() then return 'ranged',false end
    q.stage='return';s.readyAt=nil;s.weaponAt=0
  end
  if q.stage=='return' then
    if Flag(o.meleePressure) then return 'ranged',false end
    if Flag(o.meleeTracking) or Flag(o.meleeSwinging) then q.stage='finish' end
    return 'melee',false
  end
  if q.stage=='finish' then
    if Flag(o.meleeBusy) then return 'melee',false end
    s.weave=nil
  end
  return nil,false
end
local function RoutePoint(t,p,allowRanged)
  return SDGO_BattleAI_Route(t.id,0,p.x,p.y,p.z,allowRanged and 1 or 0)==1
end
local function Flank(npc,o,s,w,t,now)
  if C.Difficulty<2 then return false end
  local function Release()
    if s.flanking and Flag(o.routeActive) then SDGO_BattleAI_CancelRoute() end
    s.flanking=false
  end
  local mayMove=not Flag(o.rangedBusy) or Flag(w.movingShot) or Flag(o.moveCancelOpen)
  if w.kind=='melee' or not t.forward or not t.groundPos or not Flag(t.visible) or not mayMove then
    Release();return false
  end
  local caught=false
  for _,a in pairs(o.allies or {}) do
    if a.contactTarget==t.id and now-(a.contactAt or 0)<=650 and Flag(t.hurt) and
       (Flag(a.meleeSwinging) or Flag(a.meleeTracking)) then caught=true end
  end
  if not caught then Release();return false end
  local f=t.forward;local length=f.x*f.x+f.z*f.z
  if length<0.25 or length>4 then Release();return false end
  -- Facing is a native unit vector. Keep the rear point within rifle range.
  local back=Min(350,w.range*0.25)
  local p={x=t.groundPos.x-f.x*back,y=t.groundPos.y,z=t.groundPos.z-f.z*back}
  if Distance2(o.pos,p)<100*100 then Release();return false end
  local lost=s.flanking and not Flag(o.routeActive)
  if lost or (not s.flanking and now>=(s.flankAt or 0)) then
    s.flankAt=now+C.FlankMs;s.flanking=RoutePoint(t,p,true)
    if s.flanking then Log(s,'rear flank with ranged intent',now);return true end
  end
  if s.flanking and Flag(o.routeActive) then Log(s,'rear flank with ranged intent',now);return true end
  s.flanking=false;return false
end
local function CoverStep(npc,o,s,w,t,now)
  if C.Difficulty<2 then s.cover=nil;s.coverSearching=nil;return false,false end
  if w.kind=='melee' or not t.pos or not SDGO_BattleAI_Cover then s.cover=nil;s.coverSearching=nil;return false,false end
  if s.coverSearching and s.coverSearching~=t.id then s.coverSearching=nil;s.coverAt=0 end
  local q=s.cover
  if q and (q.target~=t.id or now-q.started>C.CoverTimeoutMs or Flag(o.meleePressure) or
     (q.stage=='peek' and not Flag(t.visible))) then
    s.cover=nil;q=nil;s.coverSearching=nil;SDGO_BattleAI_CancelRoute()
  end
  if not q and not Flag(t.visible) then s.coverSearching=nil;return false,false end
  local mayMove=not Flag(o.rangedBusy) or Flag(w.movingShot) or Flag(o.moveCancelOpen)
  if not q and not mayMove then return false,false end
  if not q and now>=(s.coverAt or 0) then
    s.coverAt=now+C.CoverSearchMs
    if s.coverSearching==t.id or Chance(C.CoverChance) then
      local p=SDGO_BattleAI_Cover(t.id,t.pos.x,t.pos.y,t.pos.z,s.peekLeft and -1 or 1)
      if p and Flag(p.pending) then
        s.coverSearching=t.id;s.coverAt=now
        return false,false
      end
      s.coverSearching=nil
      if p and Flag(p.valid) and p.hide and p.peek then
        q={target=t.id,hide=p.hide,peek=p.peek,stage='peek',started=now,shots=o.shots or 0,retryAt=now+300}
        s.cover=q
        s.peekLeft=not s.peekLeft
        if not RoutePoint(t,q.peek,true) then s.cover=nil;q=nil end
      end
    end
  end
  if not q then return false,false end
  if q.stage=='peek' and mayMove and (now>=(q.retryAt or now+1) or
     (not Flag(o.moving) and not Flag(o.routeActive))) and Distance2(o.pos,q.peek)>25*25 then
    q.retryAt=now+300
    if not RoutePoint(t,q.peek,true) then s.cover=nil;return false,false end
  end
  if (o.shots or 0)>q.shots then
    if q.stage=='peek' then
      q.stage='tuck';SDGO_BattleAI_CancelRoute()
      q.routeAt=now
    end
  end
  if q.stage=='tuck' then
    -- Request only legal native movement cancellation. Never set B38/cooldown.
    if not Flag(o.rangedBusy) or Flag(w.movingShot) or Flag(o.moveCancelOpen) then
      if not q.routeAt and not Flag(o.routeActive) and not Flag(o.moving) and Distance2(o.pos,q.hide)>=25*25 then q.routeAt=now end
      if q.routeAt and now>=q.routeAt then
        if RoutePoint(t,q.hide,true) then q.routeAt=nil
        else
          -- Route failure already stopped the native motor. Abandon this
          -- cover opportunity so ordinary combat can take over this frame
          -- instead of stopping its movement again on every observation.
          s.cover=nil;q=nil
          return false,false
        end
      end
      if Distance2(o.pos,q.hide)<25*25 then
        s.cover=nil;s.coverAt=0
        return false,false
      end
    end
    Log(s,'cover: retract after primary fire',now);return true,not mayMove
  end
  Log(s,'cover: smallest fireable edge',now)
  return true,Flag(t.visible) and t.distance<=w.range
end
local function Orbwalk(npc,o,s,w,t,now)
  if C.Difficulty<2 then return false,false end
  if w.kind=='melee' or not Flag(t.visible) or t.distance>w.range or
     (not Flag(t.meleeApproaching) and not Flag(t.meleeTracking) and not Flag(t.meleeSwinging) and not Flag(t.hurt) and not Support(o,t,now)) then return false,false end
  local count=o.shots or 0
  if s.orbShots==nil then s.orbShots=count end
  if count>s.orbShots then
    s.orbShots=count;s.orbPending=true
  end
  if s.orbPending and (not Flag(o.rangedBusy) or Flag(o.moveCancelOpen)) and now>=(s.orbUntil or 0) then
    s.orbPending=false;s.orbUntil=now+C.OrbwalkMs;s.orbLeft=not s.orbLeft
    local dir=s.orbLeft and UNIT_MOVE_DIR_LEFT or UNIT_MOVE_DIR_RIGHT
    DirectMove(npc,o,dir,C.OrbwalkStep,
      (o.boostFraction or 0)*100>C.BoostReservePercent and UNIT_MOVE_EVENT_DASH or UNIT_MOVE_EVENT_MOVE)
  end
  if now<(s.orbUntil or 0) then
    -- A native ready weapon ends a stationary-only step immediately. The step
    -- duration is a movement goal, never an extra firing cooldown.
    if (not Flag(w.movingShot) and not Flag(o.rangedBusy) and Flag(w.ready)) or
       (not Flag(o.moving) and o.goal and Distance2(o.pos,o.goal)<=C.ArrivalDistance*C.ArrivalDistance) then
      s.orbUntil=nil
      return false,false
    end
    Log(s,'orbwalk: legal step after primary fire',now)
    return true,not Flag(w.movingShot)
  end
  return false,false
end
local function Move(npc,o,s,target,now,force)
  if now<s.repathAt and not force and (Flag(o.moving) or Flag(o.routeActive)) then return end
  s.repathAt=now+C.RepathMs
  local status
  if target.remembered and target.groundPos then
    local p=target.groundPos
    status=SDGO_BattleAI_Route(target.id,force and 1 or 0,p.x,p.y,p.z)
  else status=SDGO_BattleAI_Route(target.id,force and 1 or 0) end
  if status==0 then
    s.blocked[target.id]=now+2500
    npc:MoveStop()
    Log(s,'route unavailable',now)
  end
end
local function Recover(npc,o,s,t,now)
  if s.recoverUntil and now<s.recoverUntil and Flag(o.moving) and o.goal and
     Distance2(o.pos,o.goal)>C.ArrivalDistance*C.ArrivalDistance then return true end
  if s.recoverUntil then s.recoverUntil=nil; s.anchor=o.pos; s.progressAt=now;s.repathAt=0 end
  if not Flag(o.moving) or not o.goal or Distance2(o.pos,o.goal)<=C.ArrivalDistance*C.ArrivalDistance then
    s.anchor=o.pos; s.progressAt=now; return false
  end
  local dx,dz=o.pos.x-s.anchor.x,o.pos.z-s.anchor.z
  -- Repeated vertical hopping against a wall is not horizontal path progress.
  if dx*dx+dz*dz>=C.ProgressDistance*C.ProgressDistance then
    s.anchor=o.pos; s.progressAt=now; s.attempt=0; return false
  end
  if now-s.progressAt<C.StuckMs or now<s.recoveryAt then return false end
  s.attempt=s.attempt+1; s.recoveryAt=now+C.RecoveryMs+C.RecoveryCooldownMs
  SDGO_BattleAI_CancelRoute()
  npc:SetAttackOnOff(false)
  if s.attempt<=2 then
    local dir=s.attempt==1 and UNIT_MOVE_DIR_LEFT or UNIT_MOVE_DIR_RIGHT
    DirectMove(npc,o,dir,C.RecoveryDistance,UNIT_MOVE_EVENT_MOVE)
    s.recoverUntil=now+C.RecoveryMs
  else
    Move(npc,o,s,t,now,true); s.attempt=0; s.anchor=o.pos; s.progressAt=now
  end
  Log(s,'stuck recovery',now)
  return true
end
local function GroundStrafe(npc,o,s,w,now,canFire)
  if C.Difficulty~=1 or not canFire or not Flag(w.movingShot) or
     w.kind=='melee' or w.kind=='other' then return false end
  local turn=now>=(s.groundTurnAt or 0)
  if turn then
    s.groundLeft=not s.groundLeft
    s.groundDir=s.groundLeft and UNIT_MOVE_DIR_LEFT or UNIT_MOVE_DIR_RIGHT
  end
  if not s.groundDir then s.groundDir=UNIT_MOVE_DIR_LEFT end
  if turn or not Flag(o.moving) or Flag(o.routeActive) then
    if not DirectMove(npc,o,s.groundDir,C.StrafeDistance,UNIT_MOVE_EVENT_MOVE) then return false end
    if turn then s.groundTurnAt=now+C.StrafeSwitchMs end
  end
  return true
end
local function Evade(npc,o,s,w,t,now,canFire)
  if C.Difficulty<2 then return false end
  if C.AirStrafe~=1 or w.kind=='melee' or now>=(s.threatUntil or 0) then return false end
  -- Stationary-only weapons evade between shots, not by bypassing their motor rules.
  if not Flag(w.movingShot) and canFire then return false end
  local turn=now>=s.turnAt
  if turn or not Flag(o.moving) or Flag(o.routeActive) then
    if turn then s.left=not s.left; s.turnAt=now+C.StrafeSwitchMs end
    local dir=s.left and UNIT_MOVE_DIR_LEFT or UNIT_MOVE_DIR_RIGHT
    local boost=(o.boostFraction or 0)*100>C.BoostReservePercent
    DirectMove(npc,o,dir,C.StrafeDistance,boost and UNIT_MOVE_EVENT_DASH or UNIT_MOVE_EVENT_MOVE)
    if boost and now>=s.hopAt and Flag(o.canJump) then
      npc:SetJumpTime(C.JumpMs/1000)
      s.hopAt=now+C.JumpCooldownMs
    end
  end
  Log(s,'ranged duel: air strafe',now)
  return true
end
local function SpecialTarget(o,t)
  return t and t.hp>0 and Flag(t.visible) and Flag(t.attackable) and t.distance<=(o.specialRange or 0)
end
local function HasOrdinary(o,slot)
  if not slot or slot<0 or slot>=3 then return false end
  for _,w in pairs(o.weapons or {}) do if w.slot==slot and Flag(w.valid) then return true end end
  return false
end
local function AllyCatch(o,t,now)
  if not Flag(t.hurt) then return false end
  for _,a in pairs(o.allies or {}) do
    if a.contactTarget==t.id and now-(a.contactAt or 0)>=0 and now-(a.contactAt or 0)<=650 and
       (Flag(a.meleeTracking) or Flag(a.meleeSwinging)) then return true end
  end
  return false
end
local function OrdinaryFinisher(o,t)
  for _,w in pairs(o.weapons or {}) do
    if InRange(w,t) and Flag(w.ready) and w.kind~='other' and (w.damage or 0)>=t.hp then return true end
  end
  return false
end
local function ProfileTick(npc,o,now,deferSpecial)
  if C.Difficulty<1 or not Flag(o.valid) or o.hp<=0 then return false end
  if not U.profiles then U.profiles={} end
  if now>=(U.profileSweepAt or 0) then
    U.profileSweepAt=now+2000
    for id,old in pairs(U.profiles) do if now-(old.seen or 0)>10000 then U.profiles[id]=nil end end
  end
  local p=U.profiles[o.id]
  if not p or p.generation~=o.generation or now-(p.seen or 0)>6000 then
    p={generation=o.generation,seen=now};U.profiles[o.id]=p
  end
  p.seen=now
  local specialControl=not Flag(o.hurt)
  if o.specialControlAllowed~=nil then specialControl=Flag(o.specialControlAllowed) end
  local lockedAttack=Flag(o.meleeBusy) and not Flag(o.hurt)
  local q=p.special
  local function Release()
    p.special=nil
    if SDGO_BattleAI_ProfileLease then SDGO_BattleAI_ProfileLease(-1) end
    return false
  end
  local function ReturnWeapon()
    q.returning=true;q.returnAt=q.returnAt or now
    npc:SetAttackOnOff(false)
    -- A transform may remove the old slot. Do not freeze the whole controller
    -- waiting for an impossible restoration; normal selection owns the new form.
    if o.selectedSlot==q.slot or not HasOrdinary(o,q.slot) or now-q.returnAt>=250 then return Release() end
    npc:ChangeWeapon(q.slot)
    return true
  end
  if q then
    if Flag(o.specialBusy) then q.active=true;q.returnAt=nil;return true end
    local admitted=Flag(o.specialAcknowledged) and (o.specialAckSequence or 0)>(q.ackSequence or 0)
    if admitted and (o.specialFinished or 0)<=(q.finished or 0) and now-q.started<=2200 then q.returnAt=nil;return true end
    if q.returning then return ReturnWeapon() end
    if q.active or (o.specialFinished or 0)>(q.finished or 0) or now-q.started>2200 then return ReturnWeapon() end
    if not specialControl or lockedAttack then
      npc:SetAttackOnOff(false)
      return true
    end
    local target=nil
    for _,t in pairs(o.targets or {}) do if t.id==q.target and SpecialTarget(o,t) then target=t end end
    if not target then return ReturnWeapon() end
    if not q.defensive and not admitted and (Flag(o.defenceMoving) or AllyCatch(o,target,now) or OrdinaryFinisher(o,target) or
       (not Flag(target.hurt) and not Flag(target.meleeTracking) and not Flag(target.meleeSwinging))) then return ReturnWeapon() end
    -- A resource rejection/debit without a busy/acknowledged action must not
    -- monopolize the controller for the 2.2s lost-notice watchdog.
    if not Flag(o.spReady) then return ReturnWeapon() end
    npc:SetAttackTarget(q.target)
    if not Flag(o.specialSelected) then npc:SetAttackOnOff(false);npc:ChangeWeapon(3)
    else npc:SetAttackOnOff(true) end
    return true
  end
  if deferSpecial or not Flag(o.specialAvailable) or not Flag(o.spReady) or
     not specialControl or lockedAttack or Flag(o.specialBusy) or Flag(o.rangedBusy) then return false end
  local target=nil;local defensiveCast=false
  for _,t in pairs(o.targets or {}) do
    local melee=t.selectedTarget==o.id and (Flag(t.meleeTracking) or Flag(t.meleeSwinging))
    local pressure=t.selectedTarget==o.id and Flag(t.rangedFiring)
    local received=t.id==o.meleeAttacker or (Flag(o.hurt) and t.id==(o.policyTarget or o.nativeTarget))
    local defensive=received or (not AllyCatch(o,t,now) and
      (melee or (pressure and not OrdinaryFinisher(o,t))))
    if SpecialTarget(o,t) and defensive then
      if not target or t.id==o.meleeAttacker or (target.id~=o.meleeAttacker and t.distance<target.distance) then target=t;defensiveCast=true end
    end
  end
  -- Medium reserves SP for pressure/catch defence. Hard can confirm an opening,
  -- but never steal an allied catch, spend SP on a normal one-shot, or stop an
  -- ongoing crossfire escape just because the gauge reached its cap.
  if not target and C.Difficulty>=2 and not Flag(o.defenceMoving) then
    for _,t in pairs(o.targets or {}) do
      if t.id==(o.policyTarget or o.nativeTarget) and SpecialTarget(o,t) and
         (Flag(t.hurt) or Flag(t.meleeTracking) or Flag(t.meleeSwinging)) and
         not AllyCatch(o,t,now) and not OrdinaryFinisher(o,t) then target=t end
    end
  end
  if not target then return false end
  if U.releases then U.releases[o.id]=nil end
  p.special={target=target.id,slot=o.selectedSlot,started=now,finished=o.specialFinished or 0,
             ackSequence=o.specialAckSequence or 0,defensive=defensiveCast}
  if SDGO_BattleAI_ProfileLease then SDGO_BattleAI_ProfileLease(o.selectedSlot) end
  MeleeGate(false);SDGO_BattleAI_CancelRoute();npc:SetAttackOnOff(false);npc:ChangeWeapon(3)
  return true
end
local function Tick(npc)
  local o=SDGO_BattleAI_Observe()
  if not o or not Flag(o.valid) or not o.weapons or not o.targets then return SDGO_BattleAI_Native() end
  local now=SDGO_BattleAI_Now()
  local s=State(o,now)
  if not s.initialized then
    s.initialized=true
    -- Keep the game's initial terrain/action setup once per spawn. Subsequent
    -- decisions are generic and do not read the original script's shared globals.
    SDGO_BattleAI_Native(1)
  end
  local t=PickTarget(o,s,now)
  if t then o.policyTarget=t.id end
  if ProfileTick(npc,o,now,s.weave~=nil and not Flag(o.hurt)) then s.weave=nil;s.cover=nil;return 123 end
  if o.hp<=0 or Flag(o.hurt) then
    npc:SetAttackOnOff(false); npc:MoveStop(); s.readyAt=nil; s.recoverUntil=nil
    SDGO_BattleAI_CancelRoute()
    s.weave=nil;s.cover=nil;MeleeGate(false)
    return 0
  end
  if s.weave or Flag(o.meleeBusy) then
    for _,held in pairs(o.targets) do
      if held.id==s.target and held.hp>0 and Flag(held.attackable) then t=held end
    end
  end
  if not t then
    npc:SetAttackOnOff(false); s.readyAt=nil; s.target=nil
    SDGO_BattleAI_CancelRoute()
    SDGO_BattleAI_AimTarget(0)
    s.weave=nil;s.cover=nil;MeleeGate(false)
    if not Flag(o.meleeBusy) and not Flag(o.specialBusy) then
      if not s.patrolling or not o.goal or Distance2(o.pos,o.goal)<=C.ArrivalDistance*C.ArrivalDistance or now>=s.patrolAt then
        npc:SetMoveTargetPosRandom();s.patrolAt=now+C.PatrolMs;s.patrolling=true
      end
      npc:SetMoveType(UNIT_MOVE_EVENT_MOVE)
    end
    Log(s,'no visible reachable opponent',now)
    return 123
  end
  s.patrolling=false
  if s.target~=t.id then
    s.target=t.id; s.targetAt=now+C.TargetHoldMs; s.readyAt=nil
    s.repathAt=0
    npc:SetAttackTarget(t.id)
    s.cover=nil;s.flanking=false
  end
  local forced,waiting=Weave(npc,o,s,t,now)
  if waiting then Log(s,'melee weave: release before finisher',now);return 123 end
  if Flag(t.rangedFiring) or Flag(o.hurtRecently) then s.threatUntil=now+C.ThreatMemoryMs end
  local threat=now<(s.threatUntil or 0)
  if Flag(o.rangedBusy) and (forced~='melee' or not Flag(o.moveCancelOpen)) then
    -- A charged/continuous native ranged state may already have consumed its
    -- round. Do not switch weapons, pulse its held input or cut it at a burst boundary.
    for _,active in pairs(o.weapons) do
      if active.slot==o.selectedSlot and Flag(active.valid) then
        local keep=Flag(t.visible) and Flag(t.attackable) and t.distance<=active.range
        SDGO_BattleAI_AimTarget(keep and t.id or 0)
        local covered,peek=false,false
        if not Flag(o.defenceMoving) then covered,peek=CoverStep(npc,o,s,active,t,now) end
        if covered then keep=keep and peek
        elseif Flag(o.defenceMoving) then
        elseif Flank(npc,o,s,active,t,now) then
        else
          local walked,pause=Orbwalk(npc,o,s,active,t,now)
          if walked then keep=keep and not pause
          elseif not GroundStrafe(npc,o,s,active,now,keep) then
            SDGO_BattleAI_CancelRoute()
            if keep then
              if C.Difficulty==1 and not Flag(active.movingShot) then npc:MoveStop()
              else Evade(npc,o,s,active,t,now,true) end
            end
          end
        end
        npc:SetAttackOnOff(keep)
        SDGO_BattleAI_Cadence(C.AttackIntervalMs)
        Log(s,'native ranged state owns held input',now)
        return 123
      end
    end
    return 123
  end
  local w=PickWeapon(o,s,t,threat,now,forced)
  if not w then
    npc:SetAttackOnOff(false); SDGO_BattleAI_AimTarget(0)
    if Flag(o.defenceMoving) then
      Log(s,'weapons unavailable: retain defensive escape route',now)
      return 123
    end
    if Flag(o.meleePressure) then
      DirectMove(npc,o,UNIT_MOVE_DIR_BACKWARD,C.StrafeDistance,UNIT_MOVE_EVENT_MOVE)
    else Move(npc,o,s,t,now,false) end
    Log(s,'weapons unavailable: keep moving while native resources recover',now); return 123
  end
  if o.selectedSlot~=w.slot then
    if Flag(o.meleeBusy) or Flag(o.specialBusy) then return 123 end
    npc:SetAttackOnOff(false)
    if s.requestedSlot~=w.slot then s.switchLeft=not s.switchLeft end
    npc:ChangeWeapon(w.slot)
    s.requestedSlot=w.slot
    if not Flag(o.defenceMoving) and not s.weave then
      if not Flag(o.moving) then
        local dir=t.distance<w.range*0.18 and UNIT_MOVE_DIR_BACKWARD or
          (s.switchLeft and UNIT_MOVE_DIR_LEFT or UNIT_MOVE_DIR_RIGHT)
        DirectMove(npc,o,dir,C.StrafeDistance,UNIT_MOVE_EVENT_MOVE)
      end
      npc:SetMoveType(UNIT_MOVE_EVENT_MOVE)
    end
    s.readyAt=nil; SDGO_BattleAI_AimTarget(0)
    Log(s,'waiting for native weapon switch',now)
    return 123
  end
  if s.slot~=w.slot then
    s.slot=w.slot; s.weaponAt=now+C.WeaponHoldMs; s.readyAt=nil; s.requestedSlot=nil
  end
  local inRange=InRange(w,t)
  local reaction=inRange and Flag(w.ready)
  if Flag(o.meleeBusy) or Flag(o.specialBusy) then return 123 end
  SDGO_BattleAI_AimTarget(inRange and t.id or 0)
  if Flag(o.defenceMoving) then
    npc:SetAttackOnOff(reaction);SDGO_BattleAI_Cadence(0)
    Log(s,'front/rear crossfire: keep defensive movement and fire',now)
    return 123
  end
  if reaction then s.recoverUntil=nil
  elseif Recover(npc,o,s,t,now) then return 123 end
  local covered,peek=CoverStep(npc,o,s,w,t,now)
  if covered then
    npc:SetAttackOnOff(reaction and peek and inRange)
    SDGO_BattleAI_Cadence(C.AttackIntervalMs)
    return 123
  end
  if not inRange then
    npc:SetAttackOnOff(false)
    Move(npc,o,s,t,now,false)
    Log(s,'approach through navigation',now)
    return 123
  end
  local flanking=Flank(npc,o,s,w,t,now)
  local walked,pause=false,false
  if not covered and not flanking then walked,pause=Orbwalk(npc,o,s,w,t,now) end
  if covered then reaction=reaction and peek end
  if pause then reaction=false end
  local evasive=false
  local ground=false
  if not covered and not flanking and not walked then
    local tooClose=w.kind~='melee' and t.distance<w.range*0.18
    if not tooClose then ground=GroundStrafe(npc,o,s,w,now,reaction) end
    if not ground then
      SDGO_BattleAI_CancelRoute()
      evasive=Evade(npc,o,s,w,t,now,reaction)
    end
  end
  if not covered and not flanking and not walked and not evasive and not ground then
    if w.kind~='melee' and t.distance<w.range*0.18 then
      DirectMove(npc,o,UNIT_MOVE_DIR_BACKWARD,Min(C.StrafeDistance,w.range*0.2),UNIT_MOVE_EVENT_MOVE)
    elseif not reaction then
      -- Spend native cooldown/reload time positioning, rather than adding an
      -- idle state. A ready stationary-only weapon stops and fires immediately.
      if not s.cooldownMoving or not Flag(o.moving) then
        s.cooldownLeft=not s.cooldownLeft
        local dir=s.cooldownLeft and UNIT_MOVE_DIR_LEFT or UNIT_MOVE_DIR_RIGHT
        DirectMove(npc,o,dir,C.StrafeDistance,UNIT_MOVE_EVENT_MOVE)
      end
      s.cooldownMoving=true
    else s.cooldownMoving=false;npc:MoveStop() end
  end
  -- Intent only: native animation, ammo, weapon cooldown, projectile generation
  -- and collisions execute the attack. No synthesized hit/damage packet.
  npc:SetAttackOnOff(reaction)
  SDGO_BattleAI_Cadence(C.AttackIntervalMs)
  Log(s,reaction and 'attack intent' or 'native weapon unavailable: position for next attack',now)
  return 123
end
function SDGO_BattleAI_ProfileRelease(npc)
  local slot=SDGO_BattleAI_ProfilePreviousSlot and SDGO_BattleAI_ProfilePreviousSlot() or -1
  local identified,id=pcall(npc.GetID,npc)
  if not identified or id==nil then id=tostring(npc) end
  if not U.releases then U.releases={} end
  local function Release()
    U.releases[id]=nil
    if U.profiles then U.profiles[id]=nil end
    if SDGO_BattleAI_ProfileLease then SDGO_BattleAI_ProfileLease(-1) end
  end
  local selected,mask,busy=-1,-1,0
  if SDGO_BattleAI_BoundSlot then selected,mask,busy=SDGO_BattleAI_BoundSlot() end
  if Flag(busy) then U.releases[id]=nil;return 123 end
  local valid=mask==nil or mask<0 or
    (slot==0 and (mask==1 or mask==3 or mask==5 or mask==7)) or
    (slot==1 and (mask==2 or mask==3 or mask==6 or mask==7)) or
    (slot==2 and mask>=4)
  if slot<0 or slot>=3 or not valid or selected==slot then Release();return 123 end
  local now=SDGO_BattleAI_Now()
  local generation=SDGO_BattleAI_Generation and SDGO_BattleAI_Generation() or 0
  local pending=U.releases[id]
  if not pending or pending.slot~=slot or pending.generation~=generation then
    pending={slot=slot,generation=generation,started=now};U.releases[id]=pending
  end
  if now-pending.started>=250 then Release();return 123 end
  npc:SetAttackOnOff(false);npc:ChangeWeapon(slot)
  return 123
end
function SDGO_BattleAI_ProfileUpdate(npc)
  if C.Enabled~=1 or C.Difficulty~=1 or SDGO_BattleAI_Authority()~=1 then return 123 end
  local ok,result=pcall(function() return ProfileTick(npc,SDGO_BattleAI_Observe(),SDGO_BattleAI_Now()) end)
  if not ok then SDGO_BattleAI_Log('profile action unavailable: '..tostring(result)) end
  return 123
end
function SDGO_BattleAI_GenericUpdate(npc)
  if C.Difficulty<1 or C.Enabled~=1 or C.GenericMode~=1 or SDGO_BattleAI_Authority()~=1 then
    return SDGO_BattleAI_Native()
  end
  local identified,id=pcall(npc.GetID,npc)
  if not identified or id==nil then id=tostring(npc) end
  local generation=SDGO_BattleAI_Generation and SDGO_BattleAI_Generation() or 0
  local failure=U.failures and U.failures[id]
  if failure and failure.generation==generation and failure.count>=3 then return SDGO_BattleAI_Native() end
  local ok,result=pcall(Tick,npc)
  if ok then
    if U.failures then U.failures[id]=nil end
    return result
  end
  U.faults=U.faults+1
  if not U.failures then U.failures={} end
  if not failure or failure.generation~=generation then failure={generation=generation,count=0};U.failures[id]=failure end
  failure.count=failure.count+1
  if failure.count<=3 then SDGO_BattleAI_Log('generic policy error; native AI retained: '..tostring(result)) end
  npc:SetAttackOnOff(false)
  return SDGO_BattleAI_Native()
end
U.faults=0
U.failures={}
U.difficulty=C.Difficulty
SDGO_BattleAI_Universal=U
