-- sim_mission.lua - drives mission_ai.lua against a mock of the game's script API.
--
-- Run from the repository root with a Lua 5.0 interpreter that opens only the base and
-- string libraries, like the game (see tools/lua50_host.c):
--   lua50_host mods/mission_ai/test/sim_mission.lua
--
-- The wingmen themselves are spawned and driven by the game (mission_ai.dll + the AI
-- battle bots' scripts), so this only checks what the script adds: mission enemies
-- may pick a nearby wingman through the SearchLockPlayer wrapper, and the status log.

SDGO_MissionAI_Config = { EnemiesTargetWingmen = 1, EnemyAggroRange = 2500, LogCombat = 1 }

local failures = 0
local function Check(cond, what)
  if cond then
    print("  ok   " .. what)
  else
    print("  FAIL " .. what)
    failures = failures + 1
  end
end

W = nil
local logLines, logCount = {}, 0
function SDGO_Log(msg) logCount = logCount + 1 logLines[logCount] = msg end
local function LogHas(text)
  for i = 1, logCount do if string.find(logLines[i], text, 1, true) then return true end end
  return false
end

local function Sqrt(x)
  if x <= 0 then return 0 end
  local r = x
  for _ = 1, 40 do r = 0.5 * (r + x / r) end
  return r
end
function vector3(x, y, z) return { x = x, y = y, z = z } end
function GetDistance(a, b)
  local dx, dy, dz = a.x - b.x, a.y - b.y, a.z - b.z
  return Sqrt(dx * dx + dy * dy + dz * dz)
end
function CheckObejctAlive(id) local o = W.objects[id] return o ~= nil and o.alive end
function SDGO_Now() return W.now end
function SDGO_Wingmen() return unpack(W.wingmen) end
function SDGO_LocalWingmen() return unpack(W.localWingmen or W.wingmen) end
function GetNumPlayer() return W.humans or 1 end
function PlayerWarp(player,pos,dir)
  W.humanWarps=(W.humanWarps or 0)+1
  return 97
end

local Unit = {}
Unit.__index = Unit
function DocumentFind(id) if not W.objects[id] then return nil end return setmetatable({ _id = id }, Unit) end
function Unit:GetNowPos() local p = W.objects[self._id].pos return vector3(p.x, p.y, p.z) end
function Unit:GetAttackTarget() return W.objects[self._id].target end
function Unit:SetAttackTarget(id) W.objects[self._id].target = id end
function Unit:GetHp() return W.objects[self._id].hp end
function Unit:Warp(pos,dir)
  local unit=W.objects[self._id]
  Check(W.humanWarps>0,'native human warp runs before wingman mirror')
  unit.pos=pos;unit.direction=dir;unit.warps=(unit.warps or 0)+1
end
function Unit:SetAttackOnOff(value) W.objects[self._id].attack=value end
function Unit:MoveStop() W.objects[self._id].stopped=true end
function Unit:SetJumpTime(value) W.objects[self._id].jump=value end

-- like the C++ search behind the global SearchLockPlayer: enemies only look at humans
function SearchLockPlayer(id)
  local me = W.objects[id]
  if me.enemy then me.target = W.player end
  if not me.enemy and W.objects[20] and W.objects[20].alive then me.target=20 end
  return me.target~=0
end
function SearchLockOnlyPlayer(id)
  W.onlyPlayerCalls=(W.onlyPlayerCalls or 0)+1
  return false
end

local function NewWorld(wingmen)
  W = { now = 1000, player = 1, wingmen = wingmen, objects = {
    [1] = { pos = vector3(0, 0, 0), alive = true, hp = 9000, target = 0 },
    [20] = { pos = vector3(2000, 0, 0), alive = true, hp = 3000, target = 0, enemy = true },
  } }
  for i, id in pairs(wingmen) do
    W.objects[id] = { pos = vector3(1500 + 100 * i, 0, 0), alive = true, hp = 14000, target = 0 }
  end
end

NewWorld({})
dofile("mods/mission_ai/mission_ai.lua")
local mode = {}

print("scenario 1: an enemy closer to a wingman than to the player attacks the wingman")
NewWorld({ 1001, 1002 })
SDGO_MissionAI_Tick(mode, W.now, 1)
SearchLockPlayer(20)
Check(W.objects[20].target == 1002, "enemy 20 switched to the nearest wingman (" .. tostring(W.objects[20].target) .. ")")
Check(LogHas("mission 1: 2 wingmen"), "the mission's wingmen were logged")
Check(LogHas("wingman 1001: HP 14000"), "status line for each wingman")

print("scenario 2: wingmen's own searches are left alone")
W.objects[1001].target = 20
SearchLockPlayer(1001)
Check(W.objects[1001].target == 20, "a wingman keeps the target its own AI chose")

print("scenario 3: no diversion outside a running mission or without wingmen")
NewWorld({})
SDGO_MissionAI_Tick(mode, W.now, 2)
SearchLockPlayer(20)
Check(W.objects[20].target == 1, "without wingmen the enemy keeps the player")
NewWorld({ 1001 })
SDGO_MissionAI_Tick(mode, W.now, 3)
W.now = W.now + 5000
SearchLockPlayer(20)
Check(W.objects[20].target == 1, "after the mission stopped ticking nothing is diverted")

print("scenario 4: a dead wingman is not picked")
NewWorld({ 1001 })
SDGO_MissionAI_Tick(mode, W.now, 4)
W.objects[1001].alive = false
SearchLockPlayer(20)
Check(W.objects[20].target == 1, "enemy stays on the player")

print("scenario 5: hot reload keeps working")
dofile("mods/mission_ai/mission_ai.lua")
NewWorld({ 1001 })
SDGO_MissionAI_Tick(mode, W.now, 5)
SearchLockPlayer(20)
Check(W.objects[20].target == 1001, "reloaded wrapper still diverts (" .. tostring(W.objects[20].target) .. ")")

print("scenario 6: a closer human and out-of-range wingmen keep native targeting")
NewWorld({ 1001 })
W.objects[20].pos = vector3(100, 0, 0)
SDGO_MissionAI_Tick(mode, W.now, 6)
SearchLockPlayer(20)
Check(W.objects[20].target == 1, "a closer human stays the target")
W.objects[20].pos = vector3(10000, 0, 0)
SearchLockPlayer(20)
Check(W.objects[20].target == 1, "wingmen beyond EnemyAggroRange are ignored")

print("scenario 7: disabled diversion and missing wingmen remain safe")
SDGO_MissionAI_Config.EnemiesTargetWingmen = 0
dofile("mods/mission_ai/mission_ai.lua")
NewWorld({ 1001 })
SDGO_MissionAI_Tick(mode, W.now, 7)
SearchLockPlayer(20)
Check(W.objects[20].target == 1, "disabled configuration preserves native search")
SDGO_MissionAI_Config.EnemiesTargetWingmen = 1
dofile("mods/mission_ai/mission_ai.lua")
W.objects[1001] = nil
SearchLockPlayer(20)
Check(W.objects[20].target == 1, "a removed wingman is not dereferenced")

print("scenario 8: DWORD clock wrap does not keep an expired mission alive")
NewWorld({ 1001 })
W.now = 4294967000
SDGO_MissionAI_Tick(mode, W.now, 8)
W.now = 100
SearchLockPlayer(20)
Check(W.objects[20].target == 1001, "recent mission remains live across clock wrap")
W.now = 3000
SearchLockPlayer(20)
Check(W.objects[20].target == 1, "expired mission stops diversion after clock wrap")

print("scenario 9: status reporting resumes after clock wrap")
NewWorld({ 1001 })
W.now = 4294967000
SDGO_MissionAI_Tick(mode, W.now, 9)
local beforeWrap = logCount
W.now = 5500
SDGO_MissionAI_Tick(mode, W.now, 9)
Check(logCount == beforeWrap + 1, "one status report after the reporting interval")
SDGO_MissionAI_Tick(mode, W.now + 1, 9)
Check(logCount == beforeWrap + 1, "status reports remain throttled")

print('scenario 10: player-only melee search uses mission enemies only for wingmen')
NewWorld({1001})
SDGO_MissionAI_Tick(mode,W.now,10)
Check(SearchLockOnlyPlayer(1001)==true and W.objects[1001].target==20,
  'living melee wingman acquires a mission NPC and preserves native search result')
SearchLockOnlyPlayer(20)
Check(W.onlyPlayerCalls==1,'mission enemies retain their original player-only search')
W.now=W.now+5000
SearchLockOnlyPlayer(1001)
Check(W.onlyPlayerCalls==2,'expired mission cannot redirect reused object ids')
dofile('mods/mission_ai/mission_ai.lua')
SDGO_MissionAI_Tick(mode,W.now,10)
W.objects[1001].target=0
Check(SearchLockOnlyPlayer(1001)==true and W.objects[1001].target==20,
  'hot reload does not recurse or restore the broken player-only search')

print('scenario 11: actual stage warp carries only local living wingmen')
NewWorld({1001,1002,1003})
W.localWingmen={1001,1002}
W.objects[1002].alive=false
W.objects[1001].target=20
SDGO_MissionAI_Tick(mode,W.now,11)
local p=vector3(5000,300,-2000)
local d=vector3(0,0,-1)
Check(PlayerWarp(1,p,d)==97,'native PlayerWarp result is preserved')
Check(W.objects[1001].pos==p and W.objects[1001].direction==d,
  'wingman uses the exact rule-script destination and direction')
Check(W.objects[1001].target==0 and W.objects[1001].attack==false and
  W.objects[1001].stopped and W.objects[1001].jump==0,'old target and locomotion are released after warp')
Check(not W.objects[1002].warps and not W.objects[1003].warps,'dead and remote wingmen are never warped')
W.now=W.now+5000
PlayerWarp(1,p,d)
Check(W.objects[1001].warps==1,'outside a live mission only the original player warp executes')

print('scenario 12: each stage loop distributes wingmen across actual human destinations once')
NewWorld({1001,1002,1003})
W.humans=2
SDGO_MissionAI_Tick(mode,W.now,12)
local p2=vector3(5200,300,-2000)
PlayerWarp(1,p,d);PlayerWarp(2,p2,d)
Check(W.objects[1001].pos==p and W.objects[1002].pos==p2 and W.objects[1003].pos==p,
  'wingmen follow assigned human slots without changing GetNumPlayer')
Check(W.objects[1001].warps==1 and W.objects[1002].warps==1 and W.objects[1003].warps==1,
  'one native player loop does not repeatedly warp the whole squad')
Check(GetNumPlayer()==2,'mission objective and difficulty human counts remain native')
print("")
if failures == 0 then
  print("ALL SCENARIOS PASSED")
else
  print(failures .. " CHECK(S) FAILED")
  error("simulation failed")
end
