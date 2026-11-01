-- Lua 5.0, base + string libraries only. Native VSAI remains the action executor.
-- The adapter prefixes this chunk with a local `config` table from config.ini.
local defaults = { Enabled=1, DecisionMs=150, ReactionMs=300, StuckMs=1400,
  ProgressDistance=30, ArrivalDistance=100, RecoveryDistance=350, RecoveryMs=650,
  RecoveryCooldownMs=1000, RangedMinDistance=450, TacticCooldownMs=900, LogDecisions=0, GenericMode=0, Difficulty=1 }
local C = {}
for k,v in pairs(defaults) do C[k]=v end
for k,v in pairs(config or SDGO_BattleAI_TestConfig or {}) do C[k]=v end
local M = SDGO_BattleAI or { originals={}, wrappers={}, states={}, faults=0 }
local specs = {}
local function Add(family, count, short, ranged)
  for i=1,count do
    local n=string.format('%02d',i)
    local tag='VSAI_'..(short and short..n or family..'_'..n)
    specs['VSAI_'..family..'_'..n..'_Update']={tag=tag, ranged=ranged,
      attack='ATTACK',
      change='ChangeNpcAI_'..tag..'_Step'}
  end
end
Add('Gunfighter',10,'GF',true)
Add('Infighter',7,'IF',false)
Add('Tanker',5,nil,true)
Add('Stoker',8,nil,false)

local function Emit(s, reason, now)
  if C.LogDecisions==1 and (s.reason~=reason or now >= (s.logAt or 0)) then
    SDGO_BattleAI_Log(string.format('NPC %d: %s',s.id,reason))
    s.logAt=now+5000
  end
  s.reason=reason
end
local function Alive(npc)
  return npc:GetHp()>0 and CheckObejctAlive(npc:GetID())
end
local function Flag(value) return value~=nil and value~=false and value~=0 end
local function Eligible(npc)
  if SDGO_BattleAI_Authority()~=1 then return false end
  local id=npc:GetNPCID()
  return id>=3604001 and id<=3607000
end
local function State(npc,now)
  local id=npc:GetID()
  local gen=SDGO_BattleAI_Generation()
  local s=M.states[id]
  if not s or s.generation~=gen or now-s.seen>6000 then
    s={ id=id, generation=gen, seen=now, nextDecision=0, attempts=0, nextTactic=0,
        nextRecovery=0, progressAt=now, anchor=npc:GetNowPos() }
    M.states[id]=s
  end
  s.seen=now
  -- Object IDs are reused; generation comes from native InitNPCAI (also respawn).
  if not M.sweepAt or now>=M.sweepAt then
    M.sweepAt=now+2000
    for key,value in pairs(M.states) do
      if now-value.seen>6000 then M.states[key]=nil end
    end
  end
  return s
end
local function Step(spec,suffix) return _G['_ACT_'..spec.tag..'_'..suffix] end
local function Change(npc,spec,suffix)
  local fn=_G[spec.change]
  local step=Step(spec,suffix)
  local states=_G['g_NpcAI_'..spec.tag..'_Step']
  if type(fn)~='function' or step==nil or not states or not states.Init or
     type(states.Init[step])~='function' then return false end
  fn(npc,step)
  return true
end
local function Interruptible(npc,spec)
  local step=npc:GetLuaStep()
  for _,suffix in pairs({'NONE','MOVE','MOVE_TARGETPOS','RANDOMMOVE','STAND'}) do
    local value=Step(spec,suffix)
    if value~=nil and step==value then return true end
  end
  return false
end
local function Ready(npc,spec,target)
  if not target or target==0 or not CheckObejctAlive(target) then return false,'no target' end
  if not Flag(npc:CheckInAttackLength(target)) then return false,'out of weapon range' end
  if spec.ranged then
    if not Flag(npc:CheckBulletNum(UNIT_WEAPON_CHANGE_SECOND)) and
       not Flag(npc:CheckBulletNum(UNIT_WEAPON_CHANGE_THIRD)) then return false,'ranged ammo empty' end
    if not Flag(npc:RangedAttackTest(target)) then return false,'shot obstructed' end
  else
    local hit=npc:MeleeAttackTest(target)
    if hit~=true and hit~=target then return false,'melee unavailable' end
  end
  return true,'attack ready'
end
local function StopControl(npc,s)
  npc:MoveStop()
  npc:SetAttackOnOff(false)
  s.controlUntil=nil
end
local function Finish(npc,s,spec,now)
  StopControl(npc,s)
  if GetDistance(npc:GetNowPos(),s.anchor)>=C.ProgressDistance then s.attempts=0 end
  s.anchor=npc:GetNowPos()
  s.progressAt=now
  s.readyAt=nil
  Change(npc,spec,'NONE')
end
local function Control(npc,s,spec,now,dir,reason,random)
  -- These are the same native motor APIs the shipped VSAI scripts use. Never
  -- SetNowPos, refill boost, alter speed, suppress collisions, or fire a packet.
  npc:MoveStop()
  npc:SetAttackOnOff(false)
  if random then npc:SetMoveTargetPosRandom()
  else npc:SetMoveTargetPos_Dir(dir,C.RecoveryDistance,dir,false) end
  npc:SetMoveType(UNIT_MOVE_EVENT_MOVE)
  s.controlGoal=npc:GetMoveTargetPos()
  s.anchor=npc:GetNowPos()
  s.controlUntil=now+C.RecoveryMs
  s.nextDecision=now
  s.nextTactic=now+C.TacticCooldownMs
  Emit(s,reason,now)
end

local function Before(npc,spec)
  if C.Difficulty<2 or C.GenericMode==1 then return end
  if not Eligible(npc) then return false end
  local now=SDGO_BattleAI_Now()
  local id=npc:GetID()
  if not Alive(npc) or npc:CheckHurt() then
    local prior=M.states[id]
    if prior and prior.controlUntil then StopControl(npc,prior) end
    M.states[id]=nil
    return
  end
  local s=State(npc,now)
  if C.Enabled~=1 or M.faults>=3 then
    if s.controlUntil then StopControl(npc,s) end
  end
end

local function After(npc,spec)
  if C.Difficulty<2 or C.GenericMode==1 or C.Enabled~=1 or M.faults>=3 or not Eligible(npc) then return end
  if not Alive(npc) or npc:CheckHurt() then return end
  local now=SDGO_BattleAI_Now()
  local s=State(npc,now)
  if s.controlUntil then
    -- Native Update can change its state/goal on any frame, not only on a
    -- decision sample. Check ownership before the decision-rate gate.
    if not Interruptible(npc,spec) then s.controlUntil=nil; return end
    if now>=s.controlUntil then Finish(npc,s,spec,now); return end
    if now>=s.nextDecision then
      s.nextDecision=now+C.DecisionMs
      local target=npc:GetAttackTarget()
      if s.target~=target then s.target=target; s.readyAt=nil end
      local ready=Ready(npc,spec,target)
      if ready then
        if not s.readyAt then s.readyAt=now end
        if now-s.readyAt>=C.ReactionMs then
          StopControl(npc,s)
          if Change(npc,spec,spec.attack) then
            s.nextTactic=now+C.TacticCooldownMs; s.readyAt=nil
            Emit(s,'native attack after recovery',now)
            return
          end
        end
      else s.readyAt=nil end
    end
    -- Reapply a goal only when native logic replaced it. Keep its attack/timer
    -- updates running; do not open fire directly or restart path work needlessly.
    if GetDistance(npc:GetMoveTargetPos(),s.controlGoal)>1 then npc:SetMoveTargetPos(s.controlGoal) end
    npc:SetMoveType(UNIT_MOVE_EVENT_MOVE)
    return
  end
  if now<s.nextDecision then return end
  s.nextDecision=now+C.DecisionMs
  if not Interruptible(npc,spec) then
    -- Native attacks/combos take priority; their Init owns the motor now.
    s.controlUntil=nil
    s.anchor=npc:GetNowPos(); s.progressAt=now; s.readyAt=nil
    return
  end
  local target=npc:GetAttackTarget()
  if s.target~=target then s.target=target; s.readyAt=nil end
  local ready,reason=Ready(npc,spec,target)
  local here=npc:GetNowPos()
  local targetObj=target and target~=0 and DocumentFind(target)
  if spec.ranged and targetObj and CheckObejctAlive(target) and
     GetDistance(here,targetObj:GetNowPos())<C.RangedMinDistance and now>=s.nextTactic then
    Control(npc,s,spec,now,UNIT_MOVE_DIR_BACKWARD,'ranged spacing',false)
    return
  end
  if ready then
    if not s.readyAt then s.readyAt=now end
    if now-s.readyAt>=C.ReactionMs and now>=s.nextTactic and Change(npc,spec,spec.attack) then
      s.nextTactic=now+C.TacticCooldownMs
      s.readyAt=nil
      Emit(s,'native attack',now)
      return
    end
  else s.readyAt=nil end
  Emit(s,reason,now)

  local moving=Flag(npc:GetMoveTargetFlag()) and
    GetDistance(here,npc:GetMoveTargetPos())>C.ArrivalDistance
  if not moving or ready then
    s.anchor=here; s.progressAt=now
    return
  end
  if GetDistance(here,s.anchor)>=C.ProgressDistance then
    s.anchor=here; s.progressAt=now; s.attempts=0
  elseif now-s.progressAt>=C.StuckMs and now>=s.nextRecovery then
    s.attempts=s.attempts+1
    s.nextRecovery=now+C.RecoveryMs+C.RecoveryCooldownMs
    if s.attempts==1 then
      Control(npc,s,spec,now,UNIT_MOVE_DIR_LEFT,'stuck: left',false)
    elseif s.attempts==2 then
      Control(npc,s,spec,now,UNIT_MOVE_DIR_RIGHT,'stuck: right',false)
    else
      -- Use the engine's random destination selector as a bounded fallback.
      -- This does not establish obstacle clearance or solve global pathfinding.
      Control(npc,s,spec,now,UNIT_MOVE_DIR_BACKWARD,'stuck: native replan',true)
      s.attempts=0
    end
  end
end
local function Fault(err)
  M.faults=M.faults+1
  if M.faults<=3 then SDGO_BattleAI_Log('policy error; native AI retained: '..tostring(err)) end
end
local function Decorate(name,spec)
  return function(npc)
    local ok,err=pcall(M.before,npc,spec)
    if not ok then Fault(err) end
    -- Native errors still reach the game's own Lua handler. Never run it twice.
    local result=M.originals[name](npc)
    local worked,err=pcall(M.after,npc,spec)
    if not worked then Fault(err) end
    return result
  end
end
local function Refresh()
  for name,spec in pairs(specs) do
    local fn=_G[name]
    if type(fn)=='function' and fn~=M.wrappers[name] then
      M.originals[name]=fn
      M.wrappers[name]=Decorate(name,spec)
      _G[name]=M.wrappers[name]
    end
  end
end
-- Publish only after this chunk compiled and constructed a complete policy.
M.before=Before; M.after=After; M.faults=0
SDGO_BattleAI=M
SDGO_BattleAI_Refresh=Refresh
Refresh()
