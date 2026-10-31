-- Capability-driven controller. The native adapter supplies the current form's
-- weapon records and target observations; no VSAI family or machine-ID policy.
local D={GenericMode=1,Enabled=1,DecisionMs=100,ReactionMs=180,TargetHoldMs=600,
  FarTargetSlack=900,FarTargetRatioPercent=200,WeaponHoldMs=450,AttackIntervalMs=120,
  BurstMs=900,BurstPauseMs=100,AirStrafe=1,StrafeSwitchMs=450,StrafeDistance=400,
  JumpMs=600,JumpCooldownMs=1500,BoostReservePercent=20,ThreatMemoryMs=1600,
  StuckMs=1400,ProgressDistance=30,ArrivalDistance=100,RecoveryMs=650,
  RecoveryCooldownMs=1000,RecoveryDistance=350,RepathMs=700,TargetMemoryMs=2500,PatrolMs=1600,LogDecisions=0}
local C={}
for k,v in pairs(D) do C[k]=v end
for k,v in pairs(config or SDGO_BattleAI_TestConfig or {}) do C[k]=v end
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
local function Candidate(s,t,now)
  return t.id~=s.id and t.hp>0 and Flag(t.enemy) and Flag(t.attackable) and
    (not s.blocked[t.id] or now>=s.blocked[t.id])
end
local function PickTarget(o,s,now)
  local current=nil
  for _,t in pairs(o.targets) do
    if t.id==s.target and Candidate(s,t,now) then current=t end
    if Candidate(s,t,now) and Flag(t.visible) then
      local copy={}
      for key,value in pairs(t) do copy[key]=value end
      copy.seen=now;s.memory[t.id]=copy
    elseif t.hp<=0 then s.memory[t.id]=nil end
  end
  local currentRange=current and AnyInRange(o,current)
  local best,near=nil,nil
  for _,t in pairs(o.targets) do
    if Candidate(s,t,now) and Flag(t.visible) then
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
    if last and now-last.seen<C.TargetMemoryMs and Candidate(s,last,now) then
      local copy={}
      for key,value in pairs(last) do copy[key]=value end
      copy.visible=0;copy.remembered=true;best=copy
    end
  end
  -- Never hold a dead/invisible/out-of-range target over a reachable opponent.
  -- A short hold prevents tiny HP fluctuations from repeatedly cancelling shots.
  if best and currentRange and Flag(current.visible) and now<s.targetAt and best.id~=current.id then
    best=current
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
local function PickWeapon(o,s,t,threat,now)
  local best,current=nil,nil
  local score=-1
  for _,w in pairs(o.weapons) do
    if w.slot==s.slot then current=w end
    local value=WeaponScore(w,t,threat)
    if value>score or (value==score and best and w.slot<best.slot) then best=w;score=value end
  end
  if score<0 then return nil end
  if current and Usable(current) and now<s.weaponAt and
     (InRange(current,t) or not InRange(best,t)) then return current end
  return best
end
local function Move(npc,s,target,now,force)
  if now<s.repathAt and not force then return end
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
  if s.recoverUntil and now<s.recoverUntil then return true end
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
    npc:SetMoveTargetPos_Dir(dir,C.RecoveryDistance,dir,false)
    npc:SetMoveType(UNIT_MOVE_EVENT_MOVE)
    s.recoverUntil=now+C.RecoveryMs
  else
    Move(npc,s,t,now,true); s.attempt=0; s.anchor=o.pos; s.progressAt=now
  end
  Log(s,'stuck recovery',now)
  return true
end
local function Evade(npc,o,s,w,t,now,canFire)
  if C.AirStrafe~=1 or w.kind=='melee' or now>=(s.threatUntil or 0) then return false end
  -- Stationary-only weapons evade between shots, not by bypassing their motor rules.
  if not Flag(w.movingShot) and canFire then return false end
  if now>=s.turnAt then
    s.left=not s.left; s.turnAt=now+C.StrafeSwitchMs
    local dir=s.left and UNIT_MOVE_DIR_LEFT or UNIT_MOVE_DIR_RIGHT
    npc:SetMoveTargetPos_Dir(dir,C.StrafeDistance,dir,false)
    local boost=(o.boostFraction or 0)*100>C.BoostReservePercent
    npc:SetMoveType(boost and UNIT_MOVE_EVENT_DASH or UNIT_MOVE_EVENT_MOVE)
    if boost and now>=s.hopAt and Flag(o.canJump) then
      npc:SetJumpTime(C.JumpMs/1000)
      s.hopAt=now+C.JumpCooldownMs
    end
  end
  Log(s,'ranged duel: air strafe',now)
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
  if o.hp<=0 or Flag(o.hurt) or Flag(npc:CheckHurt()) then
    npc:SetAttackOnOff(false); npc:MoveStop(); s.readyAt=nil; s.recoverUntil=nil
    SDGO_BattleAI_CancelRoute()
    return 0
  end
  if now<s.decisionAt then return 123 end
  s.decisionAt=now+C.DecisionMs
  local t=PickTarget(o,s,now)
  if not t then
    npc:SetAttackOnOff(false); s.readyAt=nil; s.target=nil
    SDGO_BattleAI_CancelRoute()
    SDGO_BattleAI_AimTarget(0)
    if now>=s.patrolAt and not Flag(o.meleeBusy) and not Flag(o.specialBusy) then
      npc:SetMoveTargetPosRandom()
      npc:SetMoveType(UNIT_MOVE_EVENT_MOVE)
      s.patrolAt=now+C.PatrolMs
    end
    Log(s,'no visible reachable opponent',now)
    return 123
  end
  if s.target~=t.id then
    s.target=t.id; s.targetAt=now+C.TargetHoldMs; s.readyAt=nil
    s.repathAt=0
    npc:SetAttackTarget(t.id)
  end
  if Flag(t.rangedFiring) or Flag(o.hurtRecently) then s.threatUntil=now+C.ThreatMemoryMs end
  local threat=now<(s.threatUntil or 0)
  if Flag(o.rangedBusy) then
    -- A charged/continuous native ranged state may already have consumed its
    -- round. Do not switch weapons, pulse its held input or cut it at a burst boundary.
    for _,active in pairs(o.weapons) do
      if active.slot==o.selectedSlot and Flag(active.valid) then
        local keep=Flag(t.visible) and Flag(t.attackable) and t.distance<=active.range
        SDGO_BattleAI_AimTarget(keep and t.id or 0)
        SDGO_BattleAI_CancelRoute()
        if keep then Evade(npc,o,s,active,t,now,true) end
        npc:SetAttackOnOff(keep)
        SDGO_BattleAI_Cadence(C.AttackIntervalMs)
        Log(s,'native ranged state owns held input',now)
        return 123
      end
    end
    return 123
  end
  local w=PickWeapon(o,s,t,threat,now)
  if not w then
    npc:SetAttackOnOff(false); npc:MoveStop(); SDGO_BattleAI_AimTarget(0)
    SDGO_BattleAI_CancelRoute()
    Log(s,'all weapons unavailable or empty',now); return 123
  end
  if o.selectedSlot~=w.slot then
    if Flag(o.meleeBusy) or Flag(o.specialBusy) then return 123 end
    npc:SetAttackOnOff(false)
    if s.requestedSlot~=w.slot or now>=(s.switchRetryAt or 0) then
      npc:ChangeWeapon(w.slot)
      s.requestedSlot=w.slot; s.switchRetryAt=now+300
    end
    s.readyAt=nil; SDGO_BattleAI_AimTarget(0)
    Log(s,'waiting for native weapon switch',now)
    return 123
  end
  if s.slot~=w.slot then
    s.slot=w.slot; s.weaponAt=now+C.WeaponHoldMs; s.readyAt=nil; s.requestedSlot=nil
  end
  local inRange=InRange(w,t)
  local ready=inRange and Flag(w.ready)
  if ready then if not s.readyAt then s.readyAt=now end else s.readyAt=nil end
  local reaction=ready and now-s.readyAt>=C.ReactionMs
  if s.pauseUntil and now<s.pauseUntil then reaction=false
  elseif s.pauseUntil then s.pauseUntil=nil; s.burstAt=nil end
  if reaction and not s.burstAt then s.burstAt=now end
  if reaction and C.BurstMs>0 and now-s.burstAt>=C.BurstMs then
    s.pauseUntil=now+C.BurstPauseMs; s.burstAt=nil; reaction=false
  end
  if Flag(o.meleeBusy) or Flag(o.specialBusy) then return 123 end
  SDGO_BattleAI_AimTarget(inRange and t.id or 0)
  if Recover(npc,o,s,t,now) then return 123 end
  if not inRange then
    npc:SetAttackOnOff(false)
    Move(npc,s,t,now,false)
    Log(s,'approach through navigation',now)
    return 123
  end
  SDGO_BattleAI_CancelRoute()
  local evasive=Evade(npc,o,s,w,t,now,reaction)
  if not evasive then
    if w.kind~='melee' and t.distance<w.range*0.18 then
      npc:SetMoveTargetPos_Dir(UNIT_MOVE_DIR_BACKWARD,Min(C.StrafeDistance,w.range*0.2),UNIT_MOVE_DIR_BACKWARD,false)
      npc:SetMoveType(UNIT_MOVE_EVENT_MOVE)
    else npc:MoveStop() end
  end
  -- Intent only: native animation, ammo, weapon cooldown, projectile generation
  -- and collisions execute the attack. No synthesized hit/damage packet.
  npc:SetAttackOnOff(reaction)
  SDGO_BattleAI_Cadence(C.AttackIntervalMs)
  Log(s,reaction and 'attack intent' or 'reaction or burst interval',now)
  return 123
end
function SDGO_BattleAI_GenericUpdate(npc)
  if C.Enabled~=1 or C.GenericMode~=1 or SDGO_BattleAI_Authority()~=1 or U.faults>=3 then
    return SDGO_BattleAI_Native()
  end
  local ok,result=pcall(Tick,npc)
  if ok then return result end
  U.faults=U.faults+1
  if U.faults<=3 then SDGO_BattleAI_Log('generic policy error; native AI retained: '..tostring(result)) end
  npc:SetAttackOnOff(false)
  return SDGO_BattleAI_Native()
end
U.faults=0
SDGO_BattleAI_Universal=U
