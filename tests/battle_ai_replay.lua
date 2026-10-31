-- Executes the client's unmodified Lua 5.0 bytecode with an explicit game API
-- fixture. This validates native script/state compatibility, not real physics.
assert(SDGO_BattleAI_ReplayDir,'set SDGO_BattleAI_ReplayDir before this test')
local realPrint=print
local now,objects=0,{}
local gen=0
local units={}
UNIT_MOVE_EVENT_MOVE=1; UNIT_MOVE_EVENT_DASH=2
UNIT_MOVE_DIR_LEFT=1; UNIT_MOVE_DIR_RIGHT=2; UNIT_MOVE_DIR_FORWARD=3; UNIT_MOVE_DIR_BACKWARD=4
UNIT_WEAPON_CHANGE_FIRST=1; UNIT_WEAPON_CHANGE_SECOND=2; UNIT_WEAPON_CHANGE_THIRD=3; UNIT_WEAPON_CHANGE_SPECIAL=4
NPCSTATE_EXCITATION=1; NPCSTATE_RETREAT=2
function print(...) end -- Native diagnostic strings are intentionally noisy.
function SDGO_BattleAI_Now() return now end
function SDGO_BattleAI_Authority() return 1 end
function SDGO_BattleAI_Generation() return gen end
function SDGO_BattleAI_Log(text) error(text) end -- Any policy/API error fails the replay.
function GetDistance(a,b)
  local x,y,z=a.x-b.x,a.y-b.y,a.z-b.z
  local square=x*x+y*y+z*z
  if square==0 then return 0 end
  local root=square
  for i=1,30 do root=(root+square/root)/2 end
  return root
end
function DocumentFind(id) return objects[id] end
function CheckObejctAlive(id) return objects[id] and objects[id].hp>0 end
function RandomValue(n) return 0 end
function GAF_FindTarget(npc) npc:SetAttackTarget(1) end
function SearchLockPlayer(id) objects[id]:SetAttackTarget(1); return true end
SearchLockOnlyPlayer=SearchLockPlayer
function OurTeamTargetSearch(npc) return 0 end
function GAF_TargetLost_070822(npc,pos) npc:SetMoveTargetPos(pos) end
function CheckArrvial_Not_Y(id,pos,distance) return GetDistance(objects[id]:GetNowPos(),pos)<distance end
function GAF_ChangeNpcAIStep(states,npc,step)
  if states.Release[npc.step] then states.Release[npc.step](npc) end
  npc.step=step
  if states.Init[step] then states.Init[step](npc) end
end
local U={}
function U:GetID() return self.id end
function U:GetNPCID() return self.npcid end
function U:GetHp() return self.hp end
function U:GetLuaStep() return self.step end
function U:GetAttackTarget() return self.target end
function U:SetAttackTarget(id) self.target=id end
function U:GetNowPos() return {x=self.x,y=0,z=0} end
U.GetPos=U.GetNowPos
function U:GetMoveTargetPos() return self.goal end
function U:GetMoveTargetFlag() return self.moving end
function U:GetTargetAreaPos() return self.goal end
function U:SetMoveTargetPos(p) self.goal=p; self.moving=true end
U.SetMoveShotPos=U.SetMoveTargetPos
function U:SetMoveTargetPosRandom() self.moves=self.moves+1; self.goal={x=self.x+500,y=0,z=0}; self.moving=true end
function U:SetMoveTargetPos_Dir(dir,distance,facing,jump)
  assert(type(dir)=='number' and type(distance)=='number' and facing==dir and jump==false)
  self.moves=self.moves+1; self.moving=true; self.goal={x=self.x+distance,y=0,z=0}
end
function U:SetMoveType(mode) self.mode=mode end
function U:MoveStop() self.moving=false end
function U:ClearActEvent() self.firing=false end
function U:SetAttackOnOff(value)
  self.firing=value
  if value and not self.firstShot then self.firstShot=now end
end
function U:ChangeWeapon(slot) self.weapon=slot end
function U:CheckBulletNum(slot) return self.ammo end
function U:CheckInAttackLength(target) return self.range end
function U:RangedAttackTest(target) return self.clear end
function U:MeleeAttackTest(target) return self.clear end
function U:CheckHurt() return false end
function U:CheckAccDamage(value) return false end
function U:InitAccDamage() end
function U:InitTerrainRect() end
function U:SetTerrainRectOnOff(value) end
function U:SetBoostGauge(value) self.boost=value end -- Original script, not our policy.
function U:SetEventTime() self.event=0 end
function U:GetEventTime(seconds) self.event=self.event+1; return self.event>seconds*60 end
function U:InitTimeCount() self.time=now end
function U:CheckTimeCount(seconds) return now-self.time>seconds*1000 end
function U:GetUnitLength(target) return GetDistance(self:GetNowPos(),objects[target]:GetNowPos()) end
function U:GetMeleeAttackLength() return 250 end
function U:GetRangedAttackLength() return 1500 end
function U:PerceiveEnemy(range) return 0 end
function U:CheckMove() return self.moving end
function U:SetAttackEndFlg(value) self.attackEnd=value end
function U:GetAttackEndFlg() return self.attackEnd end
function U:GetCurCombo() return 0 end
function U:SetSuccessionAttackFlg(value) end
function U:ShakeMoveOnOff(value) end
function U:RotationStop() end
function U:SetRotation_Y_Angle(angle,time) end
function U:QuickDash(direction) end
function U:SetJumpTime(time) end
function U:SetNpcState(state) end
function U:GetCommanderID() return 0 end
function U:UnitEnableHurt(target) return true end
function U:SearchSafetyArea(target) return self.goal end
function U:SetUseMapWeapon(value) end
local function New(id,x)
  local n={id=id,x=x,npcid=3604001,hp=1000,target=1,step=3,weapon=2,ammo=true,clear=true,
    range=true,moving=false,moves=0,goal={x=2000,y=0,z=0},event=0,time=0}
  setmetatable(n,{__index=U}); objects[id]=n
  return n
end
local function Add(file,family,n,short,ranged)
  local num=string.format('%02d',n)
  local name='VSAI_'..family..'_'..num
  local tag='VSAI_'..(short and short..num or family..'_'..num)
  dofile(SDGO_BattleAI_ReplayDir..'/'..string.format('%04d.scp',file))
  units[1+(units.count or 0)]={name=name,tag=tag,ranged=ranged}
  units.count=(units.count or 0)+1
end
Add(1379,'Gunfighter',1,'GF',true)
for n=2,10 do Add(1407+n,'Gunfighter',n,'GF',true) end
Add(1380,'Infighter',1,'IF',false)
for n=2,7 do Add(1416+n,'Infighter',n,'IF',false) end
Add(1381,'Tanker',1,nil,true)
for n=2,5 do Add(1422+n,'Tanker',n,nil,true) end
Add(1382,'Stoker',1,nil,false)
for n=2,8 do Add(1426+n,'Stoker',n,nil,false) end

local improved=0
for index=1,units.count do
  local spec=units[index]
  local latencies={}
  for mode=1,2 do
    now=0; objects={}; gen=gen+1
    SDGO_BattleAI_TestConfig={Enabled=mode-1,RangedMinDistance=0}
    dofile('mods/battle_ai/battle_ai.lua')
    _G[spec.name..'_LuaInit']()
    New(1,spec.ranged and 1000 or 100)
    local npc=New(20,0)
    local states=_G['g_NpcAI_'..spec.tag..'_Step']
    local stand=_G['_ACT_'..spec.tag..'_STAND']
    if not stand or not states.Init[stand] then stand=_G['_ACT_'..spec.tag..'_MOVE'] end
    assert(stand~=nil and states.Init[stand],spec.name..' missing idle/move fixture')
    npc.step=stand; states.Init[stand](npc)
    for t=0,6000,16 do
      now=t; _G[spec.name..'_Update'](npc)
      assert(SDGO_BattleAI.faults==0,spec.name..' policy/API fault')
      if npc.firstShot then break end
    end
    latencies[mode]=npc.firstShot or 6001
  end
  assert(latencies[2]<=1000,spec.name..' did not react within 1 second')
  assert(latencies[2]<=latencies[1],spec.name..' regressed native attack readiness')
  if latencies[2]<latencies[1] then improved=improved+1 end
  realPrint(string.format('PASS %s original idle/move %d ms -> enhanced %d ms (fixture)',spec.name,latencies[1],latencies[2]))
  -- Exercise the decorator's own transition while retaining the real native
  -- ChangeFn and Init. An enum-only stub could accept a non-firing state.
  now=0; objects={}; gen=gen+1
  SDGO_BattleAI_TestConfig={Enabled=1,RangedMinDistance=0}
  dofile('mods/battle_ai/battle_ai.lua'); _G[spec.name..'_LuaInit']()
  New(1,spec.ranged and 1000 or 100)
  local npc=New(20,0)
  local states=_G['g_NpcAI_'..spec.tag..'_Step']
  local idle=_G['_ACT_'..spec.tag..'_NONE']
  npc.step=idle; states.Init[idle](npc)
  local update=spec.name..'_Update'
  local saved=SDGO_BattleAI.originals[update]
  SDGO_BattleAI.originals[update]=function(n) return 123 end
  _G[update](npc); now=320; _G[update](npc)
  SDGO_BattleAI.originals[update]=saved
  assert(npc.firing==true,spec.name..' enhancement did not reach a firing native Init')
  if spec.ranged then
    assert(npc.weapon==UNIT_WEAPON_CHANGE_SECOND or npc.weapon==UNIT_WEAPON_CHANGE_THIRD,
      spec.name..' enhancement selected a melee weapon')
  else assert(npc.weapon==UNIT_WEAPON_CHANGE_FIRST,spec.name..' melee selected wrong weapon') end
end
assert(improved>=10,'ranged stand delay must improve')
realPrint(string.format('ALL %d ORIGINAL BYTECODE REPLAYS PASSED; %d improved; simulated weapon/motor API, no gameplay claim',units.count,improved))
