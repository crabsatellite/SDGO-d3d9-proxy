-- Decision regression in real Lua 5.0.3. Scene/motor/weapon APIs are fixtures.
local now,authority,generation=0,1,1
local objects={}
local logs={}
local checks=0
function SDGO_BattleAI_Now() return now end
function SDGO_BattleAI_Authority() return authority end
function SDGO_BattleAI_Generation() return generation end
function SDGO_BattleAI_Log(s) logs[1]=s end
function CheckObejctAlive(id) return objects[id] and objects[id].hp>0 end
function DocumentFind(id) return objects[id] end
function GetDistance(a,b)
  local x,y,z=a.x-b.x,a.y-b.y,a.z-b.z
  local square=x*x+y*y+z*z
  if square==0 then return 0 end
  local root=square
  for i=1,30 do root=(root+square/root)/2 end
  return root
end
UNIT_MOVE_DIR_LEFT=1; UNIT_MOVE_DIR_RIGHT=2; UNIT_MOVE_DIR_BACKWARD=3
UNIT_MOVE_EVENT_MOVE=4; UNIT_WEAPON_CHANGE_SECOND=2; UNIT_WEAPON_CHANGE_THIRD=3
local Unit={}
function Unit:GetID() return self.id end
function Unit:GetNPCID() return self.npcid end
function Unit:GetHp() return self.hp end
function Unit:CheckHurt() return self.hurt end
function Unit:GetLuaStep() return self.step end
function Unit:GetNowPos() return {x=self.x,y=0,z=self.z} end
function Unit:GetMoveTargetPos() return self.goal end
function Unit:SetMoveTargetPos(goal) self.goal=goal end
function Unit:GetMoveTargetFlag() return self.moving end
function Unit:GetAttackTarget() return self.target end
function Unit:CheckInAttackLength() return self.range end
function Unit:CheckBulletNum() return self.ammo end
function Unit:RangedAttackTest() return self.clear end
function Unit:MeleeAttackTest() return self.melee end
function Unit:MoveStop() self.motor=0 end
function Unit:SetAttackOnOff(on) self.firing=on end
function Unit:SetMoveType(value) self.motor=value end
function Unit:SetMoveTargetPos_Dir(dir,distance,facing,jump)
  assert(distance>0 and facing==dir and jump==false)
  self.history[self.moves+1]=dir; self.moves=self.moves+1
end
function Unit:SetMoveTargetPosRandom() self.history[self.moves+1]='random'; self.moves=self.moves+1 end
local function New(id)
  local u={id=id,npcid=3604001,hp=1000,hurt=false,x=0,z=0,goal={x=2000,y=0,z=0},
    step=3,target=1,range=false,ammo=true,clear=false,melee=0,moving=false,
    calls=0,moves=0,transitions=0,history={}}
  setmetatable(u,{__index=Unit}); objects[id]=u
  return u
end
local function Stub(family,count,short)
  for i=1,count do
    local n=string.format('%02d',i)
    local tag='VSAI_'..(short and short..n or family..'_'..n)
    _G['_ACT_'..tag..'_NONE']=3; _G['_ACT_'..tag..'_MOVE']=1
    _G['_ACT_'..tag..'_ATTACK']=2; _G['_ACT_'..tag..'_MOVE_TARGETPOS']=4
    _G['_ACT_'..tag..'_MOVINGATTACK']=9
    _G['_ACT_'..tag..'_RANDOMMOVE']=6; _G['_ACT_'..tag..'_STAND']=7
    _G['ChangeNpcAI_'..tag..'_Step']=function(npc,step)
      npc.step=step; npc.transitions=npc.transitions+1
    end
    _G['g_NpcAI_'..tag..'_Step']={Init={[1]=function() end,[2]=function() end,
      [3]=function() end,[4]=function() end,[6]=function() end,[7]=function() end,[9]=function() end}}
    _G['VSAI_'..family..'_'..n..'_Update']=function(npc)
      npc.calls=npc.calls+1
      if npc.originalOverride then npc:originalOverride() end
      return 123
    end
  end
end
Stub('Gunfighter',10,'GF'); Stub('Infighter',7,'IF'); Stub('Tanker',5,nil); Stub('Stoker',8,nil)
local function Reset(config)
  now=0; authority=1; generation=generation+1; objects={}
  local human=New(1); human.x=1000; human.target=0
  SDGO_BattleAI_TestConfig=config or {}
  dofile('mods/battle_ai/battle_ai.lua')
  return New(20)
end
local function Tick(u,t,name)
  now=t
  return _G[name or 'VSAI_Gunfighter_01_Update'](u)
end
local function Check(ok,text)
  if not ok then
    print('DIAGNOSTIC '..tostring(logs[1])..' faults='..tostring(SDGO_BattleAI.faults))
    for k,v in pairs(SDGO_BattleAI.states[20] or {}) do print(tostring(k)..'='..tostring(v)) end
  end
  assert(ok,text); checks=checks+1; print('PASS '..text)
end

local u=Reset({Enabled=0}); u.range=true; u.clear=true
Check(Tick(u,0)==123 and Tick(u,3000)==123 and u.calls==2 and u.transitions==0,'disabled preserves original once and return value')
u=Reset(); u.range=true; u.clear=true
Tick(u,0); Tick(u,150); Tick(u,300)
Check(u.step==2 and u.transitions==1,'attack-ready idle bot reacts within configured 300 ms')
u=Reset(); u.range=true
for t=0,2100,150 do Tick(u,t) end
Check(u.transitions==0,'blocked line of fire never triggers attack')
u=Reset(); u.range=true; u.clear=true; u.ammo=false
for t=0,2100,150 do Tick(u,t) end
Check(u.transitions==0,'empty ranged weapons do not trigger attack')
u=Reset(); u.range=true; u.melee=0
for t=0,900,150 do Tick(u,t,'VSAI_Infighter_01_Update') end
Check(u.transitions==0,'native melee readiness zero is unavailable, not truthy')
u.melee=1; Tick(u,1050,'VSAI_Infighter_01_Update'); Tick(u,1350,'VSAI_Infighter_01_Update')
Check(u.step==2,'melee attack requires a valid native target result')
u=Reset(); objects[1].x=100; Tick(u,0)
Check(u.history[1]==UNIT_MOVE_DIR_BACKWARD,'ranged bot backs away instead of charging point blank')
u=Reset(); u.moving=true
for t=0,15000,150 do Tick(u,t) end
local left,right,replan=false,false,false
for _,v in pairs(u.history) do
  if v==1 then left=true elseif v==2 then right=true elseif v=='random' then replan=true end
end
Check(left and right and replan,'stationary motor escalates left, right, then native destination replan')
Check(u.moves<10,'stuck recovery has a cooldown rather than per-frame commands')
u=Reset(); u.moving=true; objects[1].x=10000
for t=0,6000,150 do u.x=t/10; Tick(u,t) end
Check(u.moves==0,'a progressing unit is not mistaken for stuck')
u=Reset(); u.moving=0
for t=0,6000,150 do Tick(u,t) end
Check(u.moves==0,'numeric zero motor flag is stationary, not a stuck movement request')
u=Reset(); u.moving=true; u.hurt=true; u.range=true; u.clear=true
for t=0,3000,150 do Tick(u,t) end
Check(u.moves==0 and u.transitions==0,'hurt NPC keeps native control')
u.hurt=false; u.hp=0; Tick(u,4000)
Check(u.moves==0 and u.transitions==0,'dead NPC keeps native control')
u=Reset(); authority=0; objects[1].x=100; u.moving=true
for t=0,3000,150 do Tick(u,t) end
Check(u.moves==0 and u.transitions==0,'remote copy never runs enhancement')
u=Reset(); u.npcid=100; objects[1].x=100; Tick(u,0)
Check(u.moves==0,'mission boss or unrelated NPC excluded')
u=Reset(); objects[1].x=100; u.step=2
for t=0,3000,150 do Tick(u,t) end
Check(u.moves==0 and u.transitions==0,'active attack or combo is not interrupted for spacing')
u=Reset(); u.moving=true; Tick(u,0); Tick(u,1350)
generation=generation+1; Tick(u,1500)
Check(u.moves==0,'respawn generation resets old progress timers')
u=Reset(); local other=New(21); u.moving=true; other.moving=true
for t=0,1800,150 do other.x=t/10; Tick(u,t); Tick(other,t) end
Check(u.moves==1 and other.moves==0,'each NPC has independent movement progress')
u=Reset(); dofile('mods/battle_ai/battle_ai.lua'); dofile('mods/battle_ai/battle_ai.lua')
Tick(u,0)
Check(u.calls==1,'hot reload does not stack native wrappers')
u=Reset(); u.moving=true; Tick(u,0); Tick(u,1500)
SDGO_BattleAI_TestConfig={Enabled=0}; dofile('mods/battle_ai/battle_ai.lua')
Tick(u,1600)
Check(not SDGO_BattleAI.states[20].controlUntil and u.motor==0,'disable during recovery releases the owned motor')
u=Reset(); u.moving=true; Tick(u,0); Tick(u,1500); Tick(u,1650)
Check(u.calls==3,'native Update still runs once on every recovery frame')
u.range=true; u.clear=true
Tick(u,1800); Tick(u,1950); Tick(u,2100)
Check(u.step==2 and u.firing==false,'recovery becomes attack through native state transition, never direct fire')
u=Reset(); u.moving=true; Tick(u,0); Tick(u,1500); u.hurt=true; Tick(u,1650)
Check(u.motor==0 and not u.firing and not SDGO_BattleAI.states[20],'hurt during recovery cancels owned input')
u=Reset(); u.moving=true; Tick(u,0); Tick(u,1500); u.hp=0; Tick(u,1650)
Check(u.motor==0 and not u.firing and not SDGO_BattleAI.states[20],'death during recovery cancels owned input')
u.hp=1000; generation=generation+1; Tick(u,1800)
Check(not SDGO_BattleAI.states[20].controlUntil,'revived NPC does not inherit recovery')
u=Reset(); u.range=true; u.clear=true
Tick(u,0,'VSAI_Tanker_01_Update'); Tick(u,300,'VSAI_Tanker_01_Update')
Check(u.step==2,'Tanker requests its native ranged Attack state')
u=Reset(); u.range=true; u.clear=true; Tick(u,0)
u.target=2; local second=New(2); second.x=1000
Tick(u,300); Tick(u,450)
Check(u.transitions==0,'target change restarts reaction window')
Tick(u,600)
Check(u.transitions==1,'new target becomes attackable after reaction window')
u=Reset(); u.moving=true; Tick(u,0); Tick(u,1500)
local ownedGoal=SDGO_BattleAI.states[20].controlGoal
u.originalOverride=function(npc) npc.goal={x=9999,y=0,z=0} end
for elapsed=16,640,16 do
  Tick(u,1500+elapsed)
  assert(u.goal==ownedGoal,'recovery goal overwritten between decision samples')
end
Check(u.calls==42,'16 ms native frames retain recovery goal without skipping Update')
u.originalOverride=function(npc) npc.step=2; npc.goal={x=9999,y=0,z=0} end
Tick(u,2148)
Check(u.goal~=ownedGoal and not SDGO_BattleAI.states[20].controlUntil,'native attack immediately takes motor ownership between samples')
u=Reset(); u.GetNowPos=function() error('injected position API failure') end
for t=0,600,150 do Tick(u,t) end
Check(u.calls==5 and SDGO_BattleAI.faults>=3,'policy error falls back to exactly one native update')
print(string.format('ALL %d POLICY CHECKS PASSED (Lua 5.0)',checks))
