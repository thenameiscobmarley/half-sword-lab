-- Half Sword Lab: in-game half of the toolkit (UE4SS Lua mod).
-- Talks to the terminal GUI through three files in ./bridge:
--   state.json   written here every second (characters, stats, gear, coins)
--   commands.txt written by the GUI: "id<TAB>verb<TAB>arg..." lines
--   mods.cfg     written by the GUI: key=value toggles, applied every tick
-- Nothing here hardcodes memory offsets; stats are found through Unreal
-- reflection, so the mod survives game updates.

local VERSION = "0.1.0"
local TICK_MS = 500

------------------------------------------------------------------ helpers

local function try(f, ...)
  local ok, r = pcall(f, ...)
  if ok then return r end
  return nil
end

local function valid(o)
  return o ~= nil and try(function() return o:IsValid() end) == true
end

local function addr(o) return string.format("%X", o:GetAddress()) end
local function oname(o) return try(function() return o:GetFName():ToString() end) or "?" end
local function cname(o) return try(function() return o:GetClass():GetFName():ToString() end) or "?" end
local function fullname(o) return try(function() return o:GetFullName() end) or "" end

local function hasWord(s, words)
  s = string.lower(s or "")
  for _, w in ipairs(words) do
    if string.find(s, w, 1, true) then return true end
  end
  return false
end

local function findAll(cls)
  return try(function() return FindAllOf(cls) end) or {}
end

------------------------------------------------------------------ paths

local function modDir()
  local src = (debug.getinfo(1, "S").source or ""):gsub("^@", "")
  local dir = src:match("^(.*)[/\\][Ss]cripts[/\\][^/\\]+$")
  if dir then return dir end
  for _, d in ipairs({ "ue4ss/Mods/HalfSwordLab", "Mods/HalfSwordLab" }) do
    local f = io.open(d .. "/enabled.txt", "r")
    if f then f:close() return d end
  end
  return "Mods/HalfSwordLab"
end

local BRIDGE = modDir() .. "/bridge/"

------------------------------------------------------------------ log

local logs, logSeq = {}, 0
local function log(msg)
  logSeq = logSeq + 1
  logs[#logs + 1] = { seq = logSeq, msg = msg }
  if #logs > 30 then table.remove(logs, 1) end
  print("[HalfSwordLab] " .. msg .. "\n")
end

------------------------------------------------------------------ json

local ARRAY = {}
local function arr(t) return setmetatable(t or {}, ARRAY) end
local ESC = { ['"'] = '\\"', ['\\'] = '\\\\', ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t' }
local function esc(s)
  return (s:gsub('[%c"\\]', function(c) return ESC[c] or string.format("\\u%04x", c:byte()) end))
end

local function enc(v)
  local t = type(v)
  if t == "nil" then return "null" end
  if t == "boolean" then return v and "true" or "false" end
  if t == "number" then
    if v ~= v or v == math.huge or v == -math.huge then return "null" end
    if math.type and math.type(v) == "integer" then return tostring(v) end
    return string.format("%.9g", v)
  end
  if t == "string" then return '"' .. esc(v) .. '"' end
  if t == "table" then
    local out = {}
    if getmetatable(v) == ARRAY or v[1] ~= nil then
      for i = 1, #v do out[i] = enc(v[i]) end
      return "[" .. table.concat(out, ",") .. "]"
    end
    for k, val in pairs(v) do out[#out + 1] = '"' .. esc(tostring(k)) .. '":' .. enc(val) end
    return "{" .. table.concat(out, ",") .. "}"
  end
  return '"' .. esc(tostring(v)) .. '"'
end

------------------------------------------------------------------ engine access

local function playerController()
  for _, pc in ipairs(findAll("PlayerController")) do
    if valid(pc) and try(function() return pc.Player:IsValid() end) then return pc end
  end
end

local function playerPawn()
  local pc = playerController()
  if not pc then return nil end
  local p = try(function() return pc.Pawn end)
  if valid(p) then return p end
end

local function getWorld()
  local pc = playerController()
  if pc then return try(function() return pc:GetWorld() end) end
end

local statics
local function gameplayStatics()
  if not valid(statics) then statics = StaticFindObject("/Script/Engine.Default__GameplayStatics") end
  return statics
end

local function vec(v) return { X = v.X, Y = v.Y, Z = v.Z } end

local function location(a)
  return try(function() return vec(a:K2_GetActorLocation()) end)
end

local function mesh(a)
  local m = try(function() return a.Mesh end)
  if valid(m) then return m end
end

------------------------------------------------------------------ reflection

local NUMERIC = {
  FloatProperty = "float", DoubleProperty = "float",
  IntProperty = "int", Int64Property = "int", Int16Property = "int", UInt32Property = "int",
  ByteProperty = "int", BoolProperty = "bool",
}
local ENGINE = { ["/Script/Engine"] = true, ["/Script/CoreUObject"] = true }

-- Numeric/bool properties declared by game classes (engine base classes skipped).
local propCache = {}
local function propsOf(obj)
  local cls = try(function() return obj:GetClass() end)
  if not valid(cls) then return {} end
  local key = fullname(cls)
  if propCache[key] then return propCache[key] end
  local list, c, depth = {}, cls, 0
  while valid(c) and depth < 12 do
    local pkg = fullname(c):match("%s(/Script/[^%.]+)")
    if pkg and ENGINE[pkg] then break end
    pcall(function()
      c:ForEachProperty(function(p)
        local pt = p:GetClass():GetFName():ToString()
        if NUMERIC[pt] then list[#list + 1] = { n = p:GetFName():ToString(), t = NUMERIC[pt] } end
      end)
    end)
    c = try(function() return c:GetSuperStruct() end)
    depth = depth + 1
  end
  propCache[key] = list
  return list
end

local function readProps(obj, filter)
  local out = arr()
  for _, p in ipairs(propsOf(obj)) do
    if not filter or hasWord(p.n, filter) then
      local v = try(function() return obj[p.n] end)
      if type(v) == "number" or type(v) == "boolean" then
        out[#out + 1] = { n = p.n, t = p.t, v = v }
      end
      if #out >= 300 then break end
    end
  end
  return out
end

local function convert(t, s)
  if t == "bool" then return s == "true" or s == "1" end
  local n = tonumber(s)
  if n and t == "int" then n = math.floor(n) end
  return n
end

local function propType(obj, name)
  for _, p in ipairs(propsOf(obj)) do
    if p.n == name then return p.t end
  end
end

------------------------------------------------------------------ registry & tracking

local reg = {}                 -- address -> UObject (everything the GUI can target)
local touchedScale, touchedMass, touchedTdil = {}, {}, {}
local massKnown = {}           -- address -> mass scale we set

local function setScale(a, s)
  touchedScale[addr(a)] = a
  if not pcall(function() a:SetActorScale3D({ X = s, Y = s, Z = s }) end) then
    pcall(function() a:SetRelativeScale3D({ X = s, Y = s, Z = s }) end)
  end
end

local function getScale(a)
  return try(function() return a:GetActorScale3D().X end)
      or try(function() return a.RelativeScale3D.X end) or 1
end

local function setMass(a, s)
  local id = addr(a)
  touchedMass[id] = a
  massKnown[id] = s
  local m = mesh(a)
  if m and pcall(function() m:SetAllMassScale(s) end) then return end
  local root = try(function() return a.RootComponent end) or a
  pcall(function() root:SetMassScale(FName("None"), s) end)
end

local function setTdil(a, v)
  touchedTdil[addr(a)] = a
  pcall(function() a.CustomTimeDilation = v end)
end

local function getTdil(a) return try(function() return a.CustomTimeDilation end) or 1 end

------------------------------------------------------------------ characters

local MERC = { "merc", "hire", "ally", "companion", "follower", "recruit", "squad", "retinue" }
local DEAD = { "dead", "killed" }

local function scanCharacters()
  local player = playerPawn()
  local pid = valid(player) and addr(player) or nil
  local seen, chars = {}, {}
  local list = findAll("Character")
  if #list == 0 then list = findAll("Pawn") end
  for _, a in ipairs(list) do
    if valid(a) then
      local id = addr(a)
      local name = oname(a)
      if not seen[id] and not name:find("^Default__") then
        seen[id] = true
        reg[id] = a
        chars[#chars + 1] = { obj = a, id = id, name = name, class = cname(a) }
      end
    end
  end

  local playerTeam
  if player then
    for _, p in ipairs(readProps(player, { "team" })) do
      if p.t == "int" then playerTeam = p.v break end
    end
  end

  for _, c in ipairs(chars) do
    c.props = readProps(c.obj)
    if c.id == pid then
      c.role = "player"
    else
      c.role = "opponent"
      if hasWord(c.class .. " " .. c.name, MERC) then c.role = "mercenary" end
      for _, p in ipairs(c.props) do
        local n = p.n
        if p.t == "bool" and p.v == true and hasWord(n, MERC) then c.role = "mercenary" end
        if p.t == "int" and playerTeam and hasWord(n, { "team" }) and p.v == playerTeam then c.role = "mercenary" end
      end
    end
    for _, p in ipairs(c.props) do
      if p.t == "bool" and p.v == true and hasWord(p.n, DEAD) then c.dead = true end
    end
  end
  return chars, player
end

------------------------------------------------------------------ classes (what can be spawned)

local classes = { weapons = arr(), armor = arr(), characters = arr() }
local WEAPON = { "weapon", "sword", "axe", "mace", "spear", "hammer", "dagger", "knife", "halberd", "shield",
  "buckler", "falchion", "messer", "estoc", "poleaxe", "flail", "club", "pick", "glaive", "bill", "longsword", "zweih" }
local ARMOR = { "armor", "armour", "helm", "gauntlet", "cuirass", "breastplate", "greave", "mail", "plate",
  "gambeson", "brigandine", "pauldron", "sabaton", "vambrace", "coif", "bascinet", "sallet", "gorget", "cuisse" }
local CHAR = { "character", "willie", "npc", "enemy", "opponent", "merc", "bandit", "knight", "fighter", "ai_" }

local function scanClasses()
  classes = { weapons = arr(), armor = arr(), characters = arr() }
  for _, c in ipairs(findAll("BlueprintGeneratedClass")) do
    if valid(c) then
      local name = oname(c)
      if not name:find("^SKEL_") and not name:find("^REINST_") then
        local path = fullname(c):match("%s(.+)$") or name
        local entry = { name = name, path = path }
        if hasWord(name, ARMOR) then classes.armor[#classes.armor + 1] = entry
        elseif hasWord(name, WEAPON) then classes.weapons[#classes.weapons + 1] = entry
        elseif hasWord(name, CHAR) then classes.characters[#classes.characters + 1] = entry end
      end
    end
  end
  log(string.format("class scan: %d weapons, %d armor, %d characters",
    #classes.weapons, #classes.armor, #classes.characters))
end

------------------------------------------------------------------ equipment

local equip = {} -- char address -> list of gear items

local function gearKind(name)
  if hasWord(name, ARMOR) then return "armor" end
  if hasWord(name, WEAPON) then return "weapon" end
end

local function scanEquipment(chars)
  local byId = {}
  for _, c in ipairs(chars) do byId[c.id] = c; equip[c.id] = arr() end

  -- Gear spawned as separate actors and attached to (or owned by) a character.
  local scanned = 0
  for _, group in ipairs({ classes.weapons, classes.armor }) do
    for _, e in ipairs(group) do
      if scanned >= 80 then break end
      scanned = scanned + 1
      for _, a in ipairs(findAll(e.name)) do
        if valid(a) then
          local parent = try(function() return a:GetAttachParentActor() end)
          if not valid(parent) then parent = try(function() return a:GetOwner() end) end
          local pid = valid(parent) and addr(parent)
          if pid and byId[pid] then
            local id = addr(a)
            reg[id] = a
            local list = equip[pid]
            list[#list + 1] = { id = id, name = oname(a), class = e.name, kind = gearKind(e.name) or "weapon",
              src = "actor", scale = getScale(a) }
          end
        end
      end
    end
  end

  -- Gear that lives as mesh components on the character itself.
  for _, c in ipairs(chars) do
    local comps = try(function() return c.obj.BlueprintCreatedComponents end)
    if comps then
      pcall(function()
        comps:ForEach(function(_, el)
          local comp = el:get()
          if valid(comp) then
            local cn = oname(comp)
            local asset = try(function() return oname(comp.StaticMesh) end)
                or try(function() return oname(comp.SkeletalMeshAsset) end)
                or try(function() return oname(comp.SkeletalMesh) end) or ""
            local kind = gearKind(cn .. " " .. asset)
            if kind then
              local id = addr(comp)
              reg[id] = comp
              local list = equip[c.id]
              list[#list + 1] = { id = id, name = cn, class = asset ~= "" and asset or cname(comp), kind = kind,
                src = "component", scale = getScale(comp) }
            end
          end
        end)
      end)
    end
  end
end

------------------------------------------------------------------ coins / economy

local COIN = { "gold", "coin", "money", "cash", "currency", "florin", "denar", "silver", "wealth", "funds", "purse" }

local function economy(player)
  local out = arr()
  local pc = playerController()
  local world = getWorld()
  local owners = {
    { "GameInstance", world and try(function() return world.OwningGameInstance end) },
    { "GameState", world and try(function() return world.GameState end) },
    { "GameMode", world and try(function() return world.AuthorityGameMode end) },
    { "PlayerState", pc and try(function() return pc.PlayerState end) },
    { "PlayerController", pc },
    { "Player", player },
  }
  for _, s in ipairs(findAll("SaveGame")) do owners[#owners + 1] = { "Save: " .. oname(s), s } end
  for _, o in ipairs(owners) do
    local obj = o[2]
    if valid(obj) then
      local id = addr(obj)
      reg[id] = obj
      for _, p in ipairs(readProps(obj, COIN)) do
        out[#out + 1] = { owner = o[1], owner_id = id, n = p.n, t = p.t, v = p.v }
      end
    end
  end
  return out
end

------------------------------------------------------------------ spawning

local function loadClass(path)
  local cls = try(function() return StaticFindObject(path) end)
  if not valid(cls) then cls = try(function() return LoadAsset(path) end) end
  if valid(cls) then return cls end
end

local function spawnClass(cls, loc)
  local gs, world = gameplayStatics(), getWorld()
  if not (valid(gs) and valid(world) and valid(cls)) then return nil end
  local xf = { Rotation = { X = 0, Y = 0, Z = 0, W = 1 }, Translation = loc, Scale3D = { X = 1, Y = 1, Z = 1 } }
  local a = try(function() return gs:BeginDeferredActorSpawnFromClass(world, cls, xf, 2, nil, 0) end)
  if not valid(a) then a = try(function() return gs:BeginDeferredActorSpawnFromClass(world, cls, xf, 2, nil) end) end
  if not valid(a) then return nil end
  if not pcall(function() gs:FinishSpawningActor(a, xf, 0) end) then
    pcall(function() gs:FinishSpawningActor(a, xf) end)
  end
  return a
end

local function near(a, dist)
  local loc = location(a) or { X = 0, Y = 0, Z = 0 }
  local ang = math.random() * math.pi * 2
  return { X = loc.X + math.cos(ang) * dist, Y = loc.Y + math.sin(ang) * dist, Z = loc.Z + 60 }
end

------------------------------------------------------------------ actions

local function launch(a, power)
  local v = { X = (math.random() - 0.5) * power * 0.6, Y = (math.random() - 0.5) * power * 0.6, Z = power }
  local m = mesh(a)
  if m then pcall(function() m:SetAllPhysicsLinearVelocity(v, true) end) end
  pcall(function() a:LaunchCharacter(v, false, true) end)
end

local function teleport(a, loc)
  pcall(function() a:K2_SetActorLocation(loc, false, {}, true) end)
  local m = mesh(a)
  if m then pcall(function() m:K2_SetWorldLocation(loc, false, {}, true) end) end
end

local function destroy(o)
  if not pcall(function() o:K2_DestroyActor() end) then
    pcall(function() o:K2_DestroyComponent(o) end)
  end
end

local function setHidden(o, hidden)
  if not pcall(function() o:SetActorHiddenInGame(hidden) end) then
    pcall(function() o:SetVisibility(not hidden, true) end)
  end
end

local function dumpObject(f, label, obj)
  if not valid(obj) then f:write("== " .. label .. ": <none>\n\n") return end
  f:write("== " .. label .. ": " .. fullname(obj) .. "\n")
  local c, depth = try(function() return obj:GetClass() end), 0
  while valid(c) and depth < 16 do
    local cn = oname(c)
    if cn == "Actor" or cn == "Object" then break end
    f:write("  -- class " .. fullname(c) .. "\n")
    pcall(function()
      c:ForEachProperty(function(p)
        local pn = p:GetFName():ToString()
        local pt = p:GetClass():GetFName():ToString()
        local v = ""
        if NUMERIC[pt] then
          v = tostring(try(function() return obj[pn] end))
        elseif pt == "ObjectProperty" or pt == "ClassProperty" or pt == "WeakObjectProperty" then
          local o = try(function() return obj[pn] end)
          v = valid(o) and fullname(o) or "nil"
        elseif pt == "StrProperty" or pt == "NameProperty" or pt == "TextProperty" then
          v = try(function() return obj[pn]:ToString() end) or ""
        elseif pt == "ArrayProperty" then
          v = "len=" .. tostring(try(function() return obj[pn]:GetArrayNum() end))
        end
        f:write(string.format("    %-34s %-20s %s\n", pn, pt, v))
      end)
    end)
    c = try(function() return c:GetSuperStruct() end)
    depth = depth + 1
  end
  f:write("\n")
end

local function dump(chars, player)
  local f = io.open(BRIDGE .. "dump.txt", "w")
  if not f then log("dump: cannot write " .. BRIDGE) return end
  local pc, world = playerController(), getWorld()
  f:write("Half Sword Lab dump " .. VERSION .. " " .. os.date() .. "\n\n")
  dumpObject(f, "Player pawn", player)
  dumpObject(f, "PlayerController", pc)
  dumpObject(f, "PlayerState", pc and try(function() return pc.PlayerState end))
  dumpObject(f, "GameInstance", world and try(function() return world.OwningGameInstance end))
  dumpObject(f, "GameMode", world and try(function() return world.AuthorityGameMode end))
  dumpObject(f, "GameState", world and try(function() return world.GameState end))
  for _, s in ipairs(findAll("SaveGame")) do dumpObject(f, "SaveGame", s) end
  local done = {}
  for _, c in ipairs(chars) do
    if not done[c.class] and c.role ~= "player" then
      done[c.class] = true
      dumpObject(f, "Character (" .. c.role .. ")", c.obj)
    end
  end
  f:write("== Loaded blueprint classes\n")
  for _, c in ipairs(findAll("BlueprintGeneratedClass")) do
    if valid(c) then f:write("  " .. fullname(c) .. "\n") end
  end
  f:close()
  log("dump written to bridge/dump.txt")
end

------------------------------------------------------------------ mods

local cfg, cfgText, panicText = {}, "", nil
local function readCfg()
  local f = io.open(BRIDGE .. "mods.cfg", "r")
  if not f then return end
  local text = f:read("a") or ""
  f:close()
  if text == cfgText then return end
  cfgText = text
  cfg = {}
  for line in text:gmatch("[^\r\n]+") do
    local k, v = line:match("^%s*([%w_]+)%s*=%s*(.-)%s*$")
    if k then cfg[k] = v end
  end
end
local function on(k) return cfg[k] == "1" end
local function num(k, d) return tonumber(cfg[k]) or d end

local modScale, modMass, modTdil = {}, {}, {}   -- address -> value a mod applied
local gravityApplied, dilationApplied, defaultGravity = nil, nil, nil
local GRAVITY = { normal = nil, moon = -162, mars = -371, zero = 0, heavy = -2940, reverse = 250 }

local function worldSettings()
  local w = getWorld()
  local ws = w and try(function() return w.PersistentLevel.WorldSettings end)
  if valid(ws) then return ws end
  for _, s in ipairs(findAll("WorldSettings")) do
    if valid(s) then return s end
  end
end

local function setGravity(g)
  local ws = worldSettings()
  if not ws then return end
  if defaultGravity == nil then defaultGravity = try(function() return ws.WorldGravityZ end) or -980 end
  if g == nil then
    pcall(function() ws.bGlobalGravitySet = false end)
    g = defaultGravity
  else
    pcall(function() ws.bGlobalGravitySet = true; ws.GlobalGravityZ = g end)
  end
  pcall(function() ws.WorldGravityZ = g; ws.bWorldGravitySet = true end)
end

local function setDilation(v)
  local gs, w = gameplayStatics(), getWorld()
  if valid(gs) and w then pcall(function() gs:SetGlobalTimeDilation(w, v) end) end
end

-- Apply `want(c)` (a value or nil) to every character through `setter`, remembering
-- what was applied so switching the mod off puts things back.
local function drive(store, chars, want, setter, neutral)
  local alive = {}
  for _, c in ipairs(chars) do
    local v = want(c)
    alive[c.id] = true
    if v ~= nil then
      if store[c.id] ~= v then setter(c.obj, v); store[c.id] = v end
    elseif store[c.id] ~= nil then
      setter(c.obj, neutral); store[c.id] = nil
    end
  end
  for id in pairs(store) do if not alive[id] then store[id] = nil end end
end

local randomSize, randomSpeed, growth = {}, {}, {}
local godFloor = {}

local function applyMods(chars, player)
  if panicText and cfgText == panicText then return end
  panicText = nil

  local function opp(c) return c.role == "opponent" and not c.dead end

  -- Size
  if not on("random_sizes") then randomSize = {} end
  if not on("ever_growing") and not on("growth_spurt") then growth = {} end
  drive(modScale, chars, function(c)
    if c.role == "player" then
      if on("growth_spurt") then
        growth[c.id] = math.min((growth[c.id] or 1) * 1.01, num("growth_cap", 2.5))
        return math.floor(growth[c.id] * 100 + 0.5) / 100
      end
      return nil
    end
    if not opp(c) then return nil end
    if on("ever_growing") then
      growth[c.id] = math.min((growth[c.id] or 1) * 1.015, 3)
      return math.floor(growth[c.id] * 100 + 0.5) / 100
    end
    if on("random_sizes") then
      if not randomSize[c.id] then
        local lo, hi = num("random_min", 0.4), num("random_max", 2.2)
        randomSize[c.id] = math.floor((lo + math.random() * (hi - lo)) * 100 + 0.5) / 100
      end
      return randomSize[c.id]
    end
    if on("opp_scale_on") then return num("opp_scale", 1.8) end
  end, setScale, 1)

  -- Weight
  drive(modMass, chars, function(c)
    if c.role == "player" and on("anvil") then return 4 end
    if opp(c) and on("feather_opp") then return 0.15 end
  end, setMass, 1)

  -- Speed (per actor), Matrix mode keeps the player at full speed
  local matrix = on("matrix")
  if not on("caffeine") then randomSpeed = {} end
  drive(modTdil, chars, function(c)
    if c.role == "player" then return matrix and (1 / 0.3) or nil end
    if not opp(c) then return nil end
    if on("freeze_opp") then return 0.02 end
    if on("hyper_opp") then return 2.5 end
    if on("caffeine") then
      randomSpeed[c.id] = randomSpeed[c.id] or math.floor((0.5 + math.random() * 1.3) * 100 + 0.5) / 100
      return randomSpeed[c.id]
    end
  end, setTdil, 1)

  -- World time
  local dil = matrix and 0.3 or (on("slowmo") and num("slowmo_value", 0.4)) or nil
  if dil ~= dilationApplied then
    setDilation(dil or 1)
    dilationApplied = dil
  end

  -- Gravity
  local g = cfg.gravity or "normal"
  local gv = g == "custom" and num("gravity_custom", -980) or GRAVITY[g]
  if gv ~= gravityApplied then
    setGravity(gv)
    gravityApplied = gv
  end

  -- Stubborn: survival stats never drop below their best value
  if on("god_mode") and player then
    local pid = addr(player)
    godFloor[pid] = godFloor[pid] or {}
    local floors = godFloor[pid]
    for _, p in ipairs(readProps(player, { "health", "blood", "conscious", "stamina", "energy", "life", "vital" })) do
      if p.t ~= "bool" and not hasWord(p.n, { "damage", "pain", "bleed", "loss", "drain", "wound", "injur", "fatigue", "max" }) then
        if not floors[p.n] or p.v > floors[p.n] then floors[p.n] = p.v
        elseif p.v < floors[p.n] then pcall(function() player[p.n] = floors[p.n] end) end
      end
    end
  else
    godFloor = {}
  end

  -- Giant weapons for the player
  if player then
    local factor = on("giant_weapons") and num("weapon_factor", 2.5) or nil
    for _, item in ipairs(equip[addr(player)] or {}) do
      local o = reg[item.id]
      if item.kind == "weapon" and valid(o) then
        local want = factor or 1
        if math.abs(getScale(o) - want) > 0.01 and (factor or touchedScale[item.id]) then setScale(o, want) end
      end
    end
  end
end

local function panicReset()
  for _, store in ipairs({ touchedScale, touchedMass, touchedTdil }) do
    for id, o in pairs(store) do
      if valid(o) then
        if store == touchedScale then setScale(o, 1)
        elseif store == touchedMass then setMass(o, 1)
        else setTdil(o, 1) end
      end
      store[id] = nil
    end
  end
  modScale, modMass, modTdil, randomSize, randomSpeed, growth, godFloor = {}, {}, {}, {}, {}, {}, {}
  setDilation(1); dilationApplied = nil
  setGravity(nil); gravityApplied = nil
  panicText = cfgText
  log("panic reset: everything this mod changed is back to normal (mods paused until you change one)")
end

------------------------------------------------------------------ commands

local lastCmd

local function readCommands()
  local f = io.open(BRIDGE .. "commands.txt", "r")
  if not f then return {} end
  local list = {}
  for line in f:lines() do
    local parts = {}
    for p in (line .. "\t"):gmatch("([^\t]*)\t") do parts[#parts + 1] = p end
    local id = tonumber(parts[1])
    if id then list[#list + 1] = { id = id, verb = parts[2], args = { table.unpack(parts, 3) } } end
  end
  f:close()
  table.sort(list, function(a, b) return a.id < b.id end)
  return list
end

local function target(id)
  local o = reg[id]
  if valid(o) then return o end
  log("target " .. tostring(id) .. " is gone (despawned?)")
end

local handlers = {}

function handlers.set(a)
  local o = target(a[1]); if not o then return end
  local t = propType(o, a[2]) or "float"
  local v = convert(t, a[3])
  if v == nil then return log("set: bad value " .. tostring(a[3])) end
  if pcall(function() o[a[2]] = v end) then log("set " .. a[2] .. " = " .. tostring(v))
  else log("set " .. a[2] .. " failed") end
end

function handlers.scale(a)
  local o = target(a[1]); local s = tonumber(a[2])
  if o and s then setScale(o, s); log("scale -> " .. s) end
end

function handlers.mass(a)
  local o = target(a[1]); local s = tonumber(a[2])
  if o and s then setMass(o, s); log("weight x" .. s) end
end

function handlers.speed(a)
  local o = target(a[1]); local s = tonumber(a[2])
  if o and s then setTdil(o, s); log("speed x" .. s) end
end

function handlers.launch(a, chars)
  local power = tonumber(a[2]) or 1500
  if a[1] == "all" then
    for _, c in ipairs(chars) do if c.role ~= "player" then launch(c.obj, power) end end
    log("launched everyone")
  else
    local o = target(a[1]); if o then launch(o, power); log("launched") end
  end
end

function handlers.bring(a, _, player)
  local o = target(a[1])
  if o and player then teleport(o, near(player, 160)); log("brought " .. oname(o) .. " to you") end
end

function handlers.visit(a, _, player)
  local o = target(a[1])
  if o and player then teleport(player, near(o, 160)); log("teleported to " .. oname(o)) end
end

function handlers.clone(a)
  local o = target(a[1]); if not o then return end
  local n, made = math.min(tonumber(a[2]) or 1, 12), 0
  local cls = try(function() return o:GetClass() end)
  for _ = 1, n do
    if spawnClass(cls, near(o, 200 + math.random() * 200)) then made = made + 1 end
  end
  log(string.format("cloned %s x%d", oname(o), made))
end

function handlers.spawn(a, _, player)
  local cls = loadClass(a[1]); if not cls then return log("spawn: class not found " .. tostring(a[1])) end
  local around = (a[3] and a[3] ~= "" and target(a[3])) or player
  if not around then return end
  local n, made = math.min(tonumber(a[2]) or 1, 12), 0
  for _ = 1, n do
    if spawnClass(cls, near(around, 150 + math.random() * 120)) then made = made + 1 end
  end
  log(string.format("spawned %s x%d", oname(cls), made))
end

function handlers.vanish(a)
  local o = target(a[1]); if o then destroy(o); reg[a[1]] = nil; log("vanished " .. oname(o)) end
end

function handlers.hide(a) local o = target(a[1]); if o then setHidden(o, true) end end
function handlers.show(a) local o = target(a[1]); if o then setHidden(o, false) end end

function handlers.reset(a)
  local o = target(a[1]); if not o then return end
  setScale(o, 1); setTdil(o, 1)
  if mesh(o) then setMass(o, 1) end
  log("reset " .. oname(o))
end

function handlers.scan_classes() scanClasses() end
function handlers.dump(_, chars, player) dump(chars, player) end
function handlers.panic() panicReset() end

local function runCommands(chars, player)
  for _, c in ipairs(readCommands()) do
    if c.id > lastCmd then
      lastCmd = c.id
      local h = handlers[c.verb]
      if h then
        local ok, err = pcall(h, c.args, chars, player)
        if not ok then log(c.verb .. " failed: " .. tostring(err)) end
      else
        log("unknown command " .. tostring(c.verb))
      end
    end
  end
end

------------------------------------------------------------------ state

local function writeState(chars, player)
  local out = arr()
  for _, c in ipairs(chars) do
    out[#out + 1] = {
      id = c.id, name = c.name, class = c.class, role = c.role, dead = c.dead or false,
      scale = getScale(c.obj), mass = massKnown[c.id] or 1, speed = getTdil(c.obj),
      props = c.props, gear = equip[c.id] or arr(),
    }
  end
  local world = getWorld()
  local ws = worldSettings()
  local state = {
    version = VERSION, t = os.time(), ack = lastCmd,
    world = {
      map = world and oname(world) or "",
      gravity = ws and try(function() return ws.WorldGravityZ end) or nil,
      paused = panicText ~= nil,
    },
    characters = out, economy = economy(player), classes = classes, log = arr(logs),
  }
  local tmp = BRIDGE .. "state.tmp"
  local f = io.open(tmp, "w")
  if not f then return end
  f:write(enc(state))
  f:close()
  os.remove(BRIDGE .. "state.json")
  os.rename(tmp, BRIDGE .. "state.json")
end

------------------------------------------------------------------ main loop

local tick = 0
local function step()
  tick = tick + 1
  if lastCmd == nil then
    -- Skip commands left over from an earlier session.
    lastCmd = 0
    for _, c in ipairs(readCommands()) do lastCmd = math.max(lastCmd, c.id) end
  end
  readCfg()
  local chars, player = scanCharacters()
  if tick % 60 == 2 then scanClasses() end
  if tick % 6 == 3 then scanEquipment(chars) end
  applyMods(chars, player)
  runCommands(chars, player)
  if tick % 2 == 0 then writeState(chars, player) end
end

math.randomseed(os.time())
log("Half Sword Lab " .. VERSION .. " loaded, bridge: " .. BRIDGE)

LoopAsync(TICK_MS, function()
  ExecuteInGameThread(function()
    local ok, err = pcall(step)
    if not ok then log("tick error: " .. tostring(err)) end
  end)
  return false
end)

RegisterKeyBind(Key.F8, function()
  ExecuteInGameThread(function() pcall(panicReset) end)
end)
