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

local Unit = {}
Unit.__index = Unit
function DocumentFind(id) if not W.objects[id] then return nil end return setmetatable({ _id = id }, Unit) end
function Unit:GetNowPos() local p = W.objects[self._id].pos return vector3(p.x, p.y, p.z) end
function Unit:GetAttackTarget() return W.objects[self._id].target end
function Unit:SetAttackTarget(id) W.objects[self._id].target = id end
function Unit:GetHp() return W.objects[self._id].hp end

-- like the C++ search behind the global SearchLockPlayer: enemies only look at humans
function SearchLockPlayer(id)
  local me = W.objects[id]
  if me.enemy then me.target = W.player end
  return me.target
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

print("")
if failures == 0 then
  print("ALL SCENARIOS PASSED")
else
  print(failures .. " CHECK(S) FAILED")
  error("simulation failed")
end
