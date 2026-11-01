-- mission_ai.lua - client-side helpers for the co-op mission wingmen.
--
-- mission_ai.dll loads this file into the game's own Lua 5.0 state and reloads it
-- whenever the file or mod.ini changes. Keep it Lua 5.0: no '#' or '%' operators,
-- varargs through 'arg', base + string library only.
--
-- The wingmen are AI pilots the battle server adds to the room (server mod coop_ai,
-- room title "!aiN lvN"); mission_ai.dll spawns them like the AI battle mode does and
-- they run the AI battle bots' own scripts. SDGO_Wingmen() returns their object ids.
--
-- This script only:
--   * lets mission enemies target wingmen. Enemy NPC scripts pick targets through the
--     global SearchLockPlayer, which only considers human players; the wrapper below
--     lets an enemy switch to a wingman that is closer than its current target and
--     within EnemyAggroRange (EnemiesTargetWingmen=1), so wingmen draw fire.
--   * with LogCombat=1, logs each wingman's HP and target every 5 s.
--   * replaces player-only wingman searches with the native mission enemy search.
--   * mirrors real PlayerWarp stage transitions to locally simulated wingmen.

local DEFAULTS = { EnemiesTargetWingmen = 1, EnemyAggroRange = 2500, LogCombat = 0 }
local CFG = {}
for k, v in pairs(DEFAULTS) do CFG[k] = v end
for k, v in pairs(SDGO_MissionAI_Config) do CFG[k] = v end

local function Log(fmt, ...)
  SDGO_Log(string.format(fmt, unpack(arg)))
end

-- Mission state lives in a global so a hot reload keeps it.
local M = SDGO_MissionAI_State
if not M then
  M = { session = -1 }
  SDGO_MissionAI_State = M
end

local function Wingmen()
  local ids = { SDGO_Wingmen() }
  local set = {}
  for _, id in pairs(ids) do set[id] = true end
  return ids, set
end

local function PositionOf(id)
  local object = DocumentFind(id)
  if not object then return nil end
  return object:GetNowPos()
end

-- GetTickCount wraps after 2^32 milliseconds. Lua numbers retain that range
-- exactly, so compare elapsed time instead of absolute deadlines.
local function Elapsed(now, before)
  local elapsed = now - before
  if elapsed < 0 then elapsed = elapsed + 4294967296 end
  return elapsed
end

-- ---- enemies may target wingmen --------------------------------------------

local function DivertToWingman(enemyId, wingmen)
  local enemy = DocumentFind(enemyId)
  if not enemy then return end
  local here = enemy:GetNowPos()
  local current = enemy:GetAttackTarget()
  local currentDistance = CFG.EnemyAggroRange
  if current and current ~= 0 then
    local p = PositionOf(current)
    if p and GetDistance(here, p) < currentDistance then currentDistance = GetDistance(here, p) end
  end
  local best, bestDistance = 0, currentDistance
  for _, id in pairs(wingmen) do
    if CheckObejctAlive(id) then
      local p = PositionOf(id)
      if p then
        local d = GetDistance(here, p)
        if d < bestDistance then best, bestDistance = id, d end
      end
    end
  end
  if best ~= 0 then enemy:SetAttackTarget(best) end
end

-- Only while a mission is ticking; object ids are reused after it ends.
local function MissionLive()
  if not M.lastTick then return false end
  return Elapsed(SDGO_Now(), M.lastTick) < 2000
end

if not SDGO_Original_SearchLockPlayer then SDGO_Original_SearchLockPlayer = SearchLockPlayer end
function SearchLockPlayer(npcId)
  local result = SDGO_Original_SearchLockPlayer(npcId)
  if CFG.EnemiesTargetWingmen == 1 and MissionLive() then
    local ids, set = Wingmen()
    if ids[1] and not set[npcId] then pcall(DivertToWingman, npcId, ids) end
  end
  return result
end

-- Some VSAI melee families only call SearchLockOnlyPlayer. In a co-op mission
-- every human is their ally, so that search can leave a living wingman at target
-- zero forever. The ordinary mission search already selects hostile NPCs for
-- team 1; preserve that native team/range/visibility filtering and boolean result.
if not SDGO_Original_SearchLockOnlyPlayer then SDGO_Original_SearchLockOnlyPlayer = SearchLockOnlyPlayer end
function SearchLockOnlyPlayer(npcId)
  if MissionLive() then
    local _, set = Wingmen()
    if set[npcId] then return SDGO_Original_SearchLockPlayer(npcId) end
  end
  return SDGO_Original_SearchLockOnlyPlayer(npcId)
end

-- Do not add NPCs to GetNumPlayer: mission objectives and difficulty use that
-- human count. Instead mirror the actual rule-script warp, using its exact safe
-- position/direction and the same native CUnit::Warp operation. This is never a
-- distance-based combat teleport and never revives a dead participant.
local function WarpWingmen(player, position, direction)
  if not SDGO_LocalWingmen then return end
  local humans=GetNumPlayer()
  if not humans or humans<1 or player<1 or player>humans then return end
  local assigned=1
  for _,id in ipairs({SDGO_LocalWingmen()}) do
    if assigned==player and CheckObejctAlive(id) then
      local npc=DocumentFind(id)
      if npc then
        npc:Warp(position,direction)
        npc:SetAttackOnOff(false);npc:SetAttackTarget(0)
        npc:MoveStop();npc:SetJumpTime(0)
      end
    end
    assigned=assigned+1
    if assigned>humans then assigned=1 end
  end
end
if not SDGO_Original_PlayerWarp then SDGO_Original_PlayerWarp=PlayerWarp end
function PlayerWarp(player, position, direction)
  local result=SDGO_Original_PlayerWarp(player,position,direction)
  if MissionLive() then
    local ok,err=pcall(WarpWingmen,player,position,direction)
    if not ok then Log('wingman stage warp failed: %s',tostring(err)) end
  end
  return result
end

-- ---- status log --------------------------------------------------------------

local function Report(now)
  if CFG.LogCombat ~= 1 then return end
  if M.lastReport and Elapsed(now, M.lastReport) < 5000 then return end
  M.lastReport = now
  local ids = Wingmen()
  for _, id in pairs(ids) do
    local unit = DocumentFind(id)
    if unit then
      Log("wingman %d: HP %d, target %d, alive %s", id, unit:GetHp(), unit:GetAttackTarget() or 0,
          tostring(CheckObejctAlive(id)))
    else
      Log("wingman %d: gone", id)
    end
  end
end

function SDGO_MissionAI_Tick(mode, now, session)
  M.lastTick = now
  if session ~= M.session then
    M.session = session
    M.lastReport = nil
    local ids = Wingmen()
    local n = 0
    for _ in pairs(ids) do n = n + 1 end
    Log("mission %d: %d wingmen", session, n)
  end
  Report(now)
end

Log("mission_ai.lua ready (enemies target wingmen: %d, log: %d)", CFG.EnemiesTargetWingmen, CFG.LogCombat)
