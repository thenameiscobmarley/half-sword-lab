#include "lab.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <mutex>
#include <random>
#include <unordered_map>
#include <unordered_set>

#include "common.hpp"
#include "ue.hpp"

using ue::Obj;

namespace lab {

static const char* VERSION = "0.2.0";

// ------------------------------------------------------------------ shared state

static std::mutex g_mx;  // guards everything touched by the overlay thread
static Snapshot g_snap;
static std::deque<Command> g_queue;
static std::map<std::string, std::string> g_cfg;
static std::deque<std::string> g_log;
static int g_logSeq = 0;
static std::string g_status = "starting";

void setStatus(const std::string& s) {
    std::lock_guard<std::mutex> l(g_mx);
    g_status = s;
}

void note(const std::string& msg) {
    log("%s", msg.c_str());
    std::lock_guard<std::mutex> l(g_mx);
    g_logSeq++;
    g_log.push_back(msg);
    while (g_log.size() > 30) g_log.pop_front();
}

void enqueue(const Command& c) {
    std::lock_guard<std::mutex> l(g_mx);
    g_queue.push_back(c);
}

Snapshot snapshot() {
    std::lock_guard<std::mutex> l(g_mx);
    Snapshot s = g_snap;
    s.status = g_status;
    s.log.assign(g_log.begin(), g_log.end());
    return s;
}

std::map<std::string, std::string> mods() {
    std::lock_guard<std::mutex> l(g_mx);
    return g_cfg;
}

static void writeModsLocked() {
    std::string out;
    for (auto& kv : g_cfg) out += kv.first + "=" + kv.second + "\n";
    writeFileAtomic(g_bridge + L"mods.cfg", out);
}

void setMod(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> l(g_mx);
    g_cfg[key] = value;
    writeModsLocked();
}

// ------------------------------------------------------------------ helpers

static std::mt19937 g_rng{std::random_device{}()};
static double rnd(double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(g_rng); }
static double round2(double v) { return std::round(v * 100) / 100; }

static std::string idOf(Obj o) {
    char b[32];
    snprintf(b, sizeof b, "%llX", (unsigned long long)(uintptr_t)o);
    return b;
}

static std::unordered_map<std::string, Obj> g_reg;  // id -> object the GUI can target

static Obj target(const std::string& id) {
    auto it = g_reg.find(id);
    if (it != g_reg.end() && ue::alive(it->second)) return it->second;
    note("target " + id + " is gone (despawned?)");
    return nullptr;
}

static Obj findCDO(const char* nm) {
    static std::unordered_map<std::string, Obj> cache;
    auto it = cache.find(nm);
    if (it != cache.end() && ue::alive(it->second)) return it->second;
    Obj found = nullptr;
    ue::each([&](Obj o) {
        if (!found && ue::isCDO(o) && ue::name(o) == nm) found = o;
    });
    cache[nm] = found;
    return found;
}

static Obj firstInstance(const char* cls) {
    Obj k = ue::findClass(cls), found = nullptr;
    if (!k) return nullptr;
    ue::each([&](Obj o) {
        if (!found && !ue::isCDO(o) && ue::isA(o, k)) found = o;
    });
    return found;
}

// Reads "Struct.Field" style paths on an object's properties.
static bool readPath(Obj o, const std::string& path, double& out) {
    Obj scope = ue::classOf(o);
    const uint8_t* base = o;
    size_t start = 0;
    while (true) {
        size_t dot = path.find('.', start);
        ue::Prop p = ue::findProp(scope, path.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
        if (!p) return false;
        if (dot == std::string::npos) return ue::readNumber(base, p, out);
        base += p.offset;
        scope = ue::propStruct(p);
        start = dot + 1;
    }
}

// ------------------------------------------------------------------ engine access

static Obj g_engine = nullptr;

static Obj world() {
    if (!ue::alive(g_engine)) g_engine = firstInstance("GameEngine");
    Obj vp = ue::getObj(g_engine, "GameViewport");
    return ue::getObj(vp, "World");
}

static Obj playerController(Obj w) {
    Obj gi = ue::getObj(w, "OwningGameInstance");
    auto players = ue::getObjArray(gi, "LocalPlayers");
    return players.empty() ? nullptr : ue::getObj(players[0], "PlayerController");
}

static Obj statics() { return findCDO("Default__GameplayStatics"); }

struct Vec { double x = 0, y = 0, z = 0; };

static bool location(Obj a, Vec& v) {
    ue::Call c(a, "K2_GetActorLocation");
    if (!c.run()) return false;
    v = {c.retNum("ReturnValue.X"), c.retNum("ReturnValue.Y"), c.retNum("ReturnValue.Z")};
    return true;
}

static Vec nearPos(Obj a, double dist) {
    Vec l;
    location(a, l);
    double ang = rnd(0, 6.283);
    return {l.x + std::cos(ang) * dist, l.y + std::sin(ang) * dist, l.z + 60};
}

static Obj meshOf(Obj a) { return ue::getObj(a, "Mesh"); }

static std::unordered_map<std::string, Obj> g_touchScale, g_touchMass, g_touchSpeed;
static std::unordered_map<std::string, double> g_massKnown;

static void setScale(Obj o, double s) {
    g_touchScale[idOf(o)] = o;
    ue::Call a(o, "SetActorScale3D");
    if (a) { a.vec("NewScale3D", s, s, s).run(); return; }
    ue::Call c(o, "SetRelativeScale3D");
    if (c) c.vec("NewScale3D", s, s, s).run();
}

static double getScale(Obj o) {
    ue::Call a(o, "GetActorScale3D");
    if (a && a.run()) return a.retNum("ReturnValue.X", 1);
    double v = 1;
    readPath(o, "RelativeScale3D.X", v);
    return v;
}

static void setMass(Obj o, double s) {
    std::string id = idOf(o);
    g_touchMass[id] = o;
    g_massKnown[id] = s;
    if (Obj m = meshOf(o)) {
        ue::Call c(m, "SetAllMassScale");
        if (c) { c.num("InMassScale", s).run(); return; }
    }
    Obj root = ue::getObj(o, "RootComponent");
    ue::Call c(root ? root : o, "SetMassScale");
    if (c) c.num("InMassScale", s).run();
}

static void setSpeed(Obj o, double v) {
    g_touchSpeed[idOf(o)] = o;
    ue::setNum(o, "CustomTimeDilation", v);
}

static double getSpeed(Obj o) {
    double v = 1;
    ue::getNum(o, "CustomTimeDilation", v);
    return v;
}

static void launch(Obj a, double power) {
    Vec v{rnd(-0.3, 0.3) * power, rnd(-0.3, 0.3) * power, power};
    if (Obj m = meshOf(a)) {
        ue::Call c(m, "SetAllPhysicsLinearVelocity");
        c.vec("NewVel", v.x, v.y, v.z).flag("bAddToCurrent", true).run();
    }
    ue::Call c(a, "LaunchCharacter");
    c.vec("LaunchVelocity", v.x, v.y, v.z).flag("bXYOverride", false).flag("bZOverride", true).run();
}

static void teleport(Obj a, const Vec& l) {
    ue::Call c(a, "K2_SetActorLocation");
    c.vec("NewLocation", l.x, l.y, l.z).flag("bSweep", false).flag("bTeleport", true).run();
    if (Obj m = meshOf(a)) {
        ue::Call w(m, "K2_SetWorldLocation");
        w.vec("NewLocation", l.x, l.y, l.z).flag("bSweep", false).flag("bTeleport", true).run();
    }
}

static void destroy(Obj o) {
    ue::Call a(o, "K2_DestroyActor");
    if (a) { a.run(); return; }
    ue::Call c(o, "K2_DestroyComponent");
    if (c) c.obj("Object", o).run();
}

static void setHidden(Obj o, bool h) {
    ue::Call a(o, "SetActorHiddenInGame");
    if (a) { a.flag("bNewHidden", h).run(); return; }
    ue::Call c(o, "SetVisibility");
    if (c) c.flag("bNewVisibility", !h).flag("bPropagateToChildren", true).run();
}

static Obj spawnClass(Obj cls, const Vec& l) {
    Obj gs = statics(), w = world();
    if (!gs || !w || !cls) return nullptr;
    auto transform = [&](ue::Call& c) {
        c.num("SpawnTransform.Rotation.W", 1)
            .vec("SpawnTransform.Translation", l.x, l.y, l.z)
            .vec("SpawnTransform.Scale3D", 1, 1, 1);
        if (c.has("TransformScaleMethod")) c.num("TransformScaleMethod", 1);
    };
    ue::Call b(gs, "BeginDeferredActorSpawnFromClass");
    b.obj("WorldContextObject", w).obj("ActorClass", cls).num("CollisionHandlingOverride", 2);
    transform(b);
    if (!b.run()) return nullptr;
    Obj a = b.retObj();
    if (!a) return nullptr;
    ue::Call f(gs, "FinishSpawningActor");
    f.obj("Actor", a);
    transform(f);
    f.run();
    return a;
}

// ------------------------------------------------------------------ scanning

static const std::initializer_list<const char*> MERC = {"merc", "hire", "ally", "companion", "follower", "recruit", "squad", "retinue"};
static const std::initializer_list<const char*> DEAD = {"dead", "killed"};
static const std::initializer_list<const char*> WEAPON = {"weapon", "sword", "axe", "mace", "spear", "hammer", "dagger", "knife", "halberd", "shield", "buckler", "falchion", "messer", "estoc", "poleaxe", "flail", "club", "pick", "glaive", "bill", "longsword", "zweih"};
static const std::initializer_list<const char*> ARMOR = {"armor", "armour", "helm", "gauntlet", "cuirass", "breastplate", "greave", "mail", "plate", "gambeson", "brigandine", "pauldron", "sabaton", "vambrace", "coif", "bascinet", "sallet", "gorget", "cuisse"};
static const std::initializer_list<const char*> CHARW = {"character", "willie", "npc", "enemy", "opponent", "merc", "bandit", "knight", "fighter", "ai_"};
static const std::initializer_list<const char*> COIN = {"gold", "coin", "money", "cash", "currency", "florin", "denar", "silver", "wealth", "funds", "purse"};

// Numeric/bool properties declared by game classes (engine base classes skipped), cached per class.
static std::unordered_map<Obj, std::vector<ue::Prop>> g_propCache;

static const std::vector<ue::Prop>& gameProps(Obj cls) {
    auto it = g_propCache.find(cls);
    if (it != g_propCache.end()) return it->second;
    std::vector<ue::Prop> out;
    for (Obj s = cls; s && out.size() < 300; s = ue::superOf(s)) {
        std::string path = ue::pathName(s);
        if (path.rfind("/Script/Engine.", 0) == 0 || path.rfind("/Script/CoreUObject.", 0) == 0) break;
        ue::forProps(s, [&](const ue::Prop& p) {
            ue::Kind k = ue::kindOf(p);
            if ((k == ue::Kind::Float || k == ue::Kind::Int || k == ue::Kind::Bool) && p.dim == 1) out.push_back(p);
            return out.size() < 300;
        }, false);
    }
    return g_propCache[cls] = out;
}

static std::vector<PropView> readProps(Obj o, std::initializer_list<const char*> filter = {}) {
    std::vector<PropView> out;
    for (auto& p : gameProps(ue::classOf(o))) {
        if (filter.size() && !hasWord(p.name, filter)) continue;
        ue::Kind k = ue::kindOf(p);
        if (k == ue::Kind::Bool) out.push_back({p.name, "bool", ue::readBool(o, p) ? 1.0 : 0.0});
        else {
            double v;
            if (ue::readNumber(o, p, v)) out.push_back({p.name, k == ue::Kind::Float ? "float" : "int", v});
        }
    }
    return out;
}

struct Char { Obj obj; CharView v; };

static std::vector<Char> g_chars;
static Obj g_player = nullptr;
static std::vector<ClassView> g_weaponClasses, g_armorClasses, g_charClasses;
static std::unordered_map<Obj, int> g_classKind;  // 0 none, 1 weapon, 2 armor
static std::unordered_map<std::string, std::vector<GearView>> g_gear;

static int classKind(Obj cls) {
    auto it = g_classKind.find(cls);
    if (it != g_classKind.end()) return it->second;
    std::string n = ue::name(cls);
    int k = hasWord(n, ARMOR) ? 2 : hasWord(n, WEAPON) ? 1 : 0;
    return g_classKind[cls] = k;
}

static void scanClasses() {
    g_weaponClasses.clear();
    g_armorClasses.clear();
    g_charClasses.clear();
    Obj bpgc = ue::findClass("BlueprintGeneratedClass");
    ue::each([&](Obj o) {
        if (ue::classOf(o) != bpgc) return;
        std::string n = ue::name(o);
        if (n.rfind("SKEL_", 0) == 0 || n.rfind("REINST_", 0) == 0) return;
        ClassView c{n, ue::pathName(o)};
        if (hasWord(n, ARMOR)) g_armorClasses.push_back(c);
        else if (hasWord(n, WEAPON)) g_weaponClasses.push_back(c);
        else if (hasWord(n, CHARW)) g_charClasses.push_back(c);
    });
    note("class scan: " + std::to_string(g_weaponClasses.size()) + " weapons, " +
         std::to_string(g_armorClasses.size()) + " armor, " + std::to_string(g_charClasses.size()) + " characters");
}

static void scanWorld(bool withGear) {
    Obj w = world();
    Obj pc = playerController(w);
    g_player = ue::getObj(pc, "Pawn");
    std::string pid = g_player ? idOf(g_player) : "";
    Obj charClass = ue::findClass("Character");
    Obj primClass = ue::findClass("PrimitiveComponent");
    std::vector<Char> chars;
    std::unordered_map<Obj, size_t> byObj;
    std::vector<Obj> gearActors, comps;

    ue::each([&](Obj o) {
        if (ue::isCDO(o)) return;
        Obj cls = ue::classOf(o);
        if (ue::isClassA(cls, charClass)) {
            Obj level = ue::outerOf(o), ow = ue::outerOf(level);
            if (w && ow != w && ue::getObj(ow, "OwningGameInstance")) return;  // leftover from an old world
            Char c{o, {}};
            c.v.id = idOf(o);
            c.v.name = ue::name(o);
            c.v.cls = ue::name(cls);
            byObj[o] = chars.size();
            chars.push_back(c);
        } else if (withGear) {
            if (classKind(cls)) gearActors.push_back(o);
            else if (primClass && ue::isClassA(cls, primClass)) comps.push_back(o);
        }
    });

    int playerTeam = INT32_MIN;
    if (g_player)
        for (auto& p : readProps(g_player, {"team"}))
            if (p.t == "int") { playerTeam = (int)p.v; break; }

    for (auto& c : chars) {
        g_reg[c.v.id] = c.obj;
        c.v.props = readProps(c.obj);
        if (c.v.id == pid) c.v.role = "player";
        else {
            c.v.role = hasWord(c.v.cls + " " + c.v.name, MERC) ? "mercenary" : "opponent";
            for (auto& p : c.v.props) {
                if (p.t == "bool" && p.v != 0 && hasWord(p.n, MERC)) c.v.role = "mercenary";
                if (p.t == "int" && playerTeam != INT32_MIN && hasWord(p.n, {"team"}) && (int)p.v == playerTeam)
                    c.v.role = "mercenary";
            }
        }
        for (auto& p : c.v.props)
            if (p.t == "bool" && p.v != 0 && hasWord(p.n, DEAD)) c.v.dead = true;
        c.v.scale = getScale(c.obj);
        c.v.speed = getSpeed(c.obj);
        auto mk = g_massKnown.find(c.v.id);
        c.v.mass = mk != g_massKnown.end() ? mk->second : 1;
    }

    if (withGear) {
        g_gear.clear();
        for (Obj a : gearActors) {
            ue::Call c(a, "GetAttachParentActor");
            Obj parent = c.run() ? c.retObj() : nullptr;
            if (!parent) parent = ue::getObj(a, "Owner");
            if (!parent || !byObj.count(parent)) continue;
            std::string id = idOf(a);
            g_reg[id] = a;
            int k = classKind(ue::classOf(a));
            g_gear[idOf(parent)].push_back({id, ue::name(a), ue::name(ue::classOf(a)), k == 2 ? "armor" : "weapon", "actor", getScale(a)});
        }
        for (Obj comp : comps) {
            Obj owner = ue::outerOf(comp);
            if (!byObj.count(owner)) continue;
            std::string n = ue::name(comp);
            Obj asset = ue::getObj(comp, "StaticMesh");
            if (!asset) asset = ue::getObj(comp, "SkeletalMeshAsset");
            if (!asset) asset = ue::getObj(comp, "SkeletalMesh");
            std::string an = asset ? ue::name(asset) : "";
            std::string both = n + " " + an;
            const char* kind = hasWord(both, ARMOR) ? "armor" : hasWord(both, WEAPON) ? "weapon" : nullptr;
            if (!kind) continue;
            std::string id = idOf(comp);
            g_reg[id] = comp;
            g_gear[idOf(owner)].push_back({id, n, an.empty() ? ue::name(ue::classOf(comp)) : an, kind, "component", getScale(comp)});
        }
    }
    for (auto& c : chars) c.v.gear = g_gear[c.v.id];
    g_chars = std::move(chars);
}

static std::vector<CoinView> economy() {
    std::vector<CoinView> out;
    Obj w = world();
    Obj pc = playerController(w);
    std::vector<std::pair<std::string, Obj>> owners = {
        {"GameInstance", ue::getObj(w, "OwningGameInstance")},
        {"GameState", ue::getObj(w, "GameState")},
        {"GameMode", ue::getObj(w, "AuthorityGameMode")},
        {"PlayerState", ue::getObj(pc, "PlayerState")},
        {"PlayerController", pc},
        {"Player", g_player},
    };
    if (Obj sg = ue::findClass("SaveGame"))
        ue::each([&](Obj o) {
            if (!ue::isCDO(o) && ue::isA(o, sg)) owners.push_back({"Save: " + ue::name(o), o});
        });
    for (auto& [label, o] : owners) {
        if (!o) continue;
        std::string id = idOf(o);
        g_reg[id] = o;
        for (auto& p : readProps(o, COIN)) out.push_back({label, id, p.n, p.t, p.v});
    }
    return out;
}

// ------------------------------------------------------------------ mods

static std::map<std::string, std::string> g_cfgCopy;
static uint64_t g_cfgStamp = 0;
static std::string g_cfgText, g_panicText;
static bool g_panicHold = false;

static void readCfg() {
    uint64_t st = fileStamp(g_bridge + L"mods.cfg");
    if (st && st != g_cfgStamp) {
        g_cfgStamp = st;
        std::string text;
        if (readFile(g_bridge + L"mods.cfg", text)) {
            std::lock_guard<std::mutex> l(g_mx);
            g_cfg.clear();
            size_t pos = 0;
            while (pos < text.size()) {
                size_t end = text.find('\n', pos);
                if (end == std::string::npos) end = text.size();
                std::string line = text.substr(pos, end - pos);
                pos = end + 1;
                size_t eq = line.find('=');
                if (eq != std::string::npos) {
                    std::string v = line.substr(eq + 1);
                    while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
                    g_cfg[line.substr(0, eq)] = v;
                }
            }
            g_cfgText = text;
        }
    }
    std::lock_guard<std::mutex> l(g_mx);
    g_cfgCopy = g_cfg;
}

static bool on(const char* k) { auto it = g_cfgCopy.find(k); return it != g_cfgCopy.end() && it->second == "1"; }
static double num(const char* k, double d) {
    auto it = g_cfgCopy.find(k);
    if (it == g_cfgCopy.end()) return d;
    char* end;
    double v = strtod(it->second.c_str(), &end);
    return end != it->second.c_str() ? v : d;
}
static std::string str(const char* k, const char* d) { auto it = g_cfgCopy.find(k); return it != g_cfgCopy.end() ? it->second : d; }

using Store = std::unordered_map<std::string, double>;
static Store g_modScale, g_modMass, g_modSpeed, g_randomSize, g_randomSpeed, g_growth;
static std::unordered_map<std::string, double> g_godFloor;
static double g_dilationApplied = NAN, g_gravityApplied = NAN, g_defaultGravity = NAN;
static bool g_gravitySet = false, g_dilationSet = false;

template <class Want, class Setter>
static void drive(Store& store, Want want, Setter setter, double neutral) {
    std::unordered_set<std::string> live;
    for (auto& c : g_chars) {
        live.insert(c.v.id);
        double v;
        bool has = want(c, v);
        auto it = store.find(c.v.id);
        if (has) {
            if (it == store.end() || it->second != v) { setter(c.obj, v); store[c.v.id] = v; }
        } else if (it != store.end()) {
            setter(c.obj, neutral);
            store.erase(it);
        }
    }
    for (auto it = store.begin(); it != store.end();) it = live.count(it->first) ? std::next(it) : store.erase(it);
}

static Obj worldSettings() {
    Obj level = ue::getObj(world(), "PersistentLevel");
    Obj ws = ue::getObj(level, "WorldSettings");
    return ws ? ws : firstInstance("WorldSettings");
}

static void setGravity(bool custom, double g) {
    Obj ws = worldSettings();
    if (!ws) return;
    if (std::isnan(g_defaultGravity)) {
        g_defaultGravity = -980;
        ue::getNum(ws, "WorldGravityZ", g_defaultGravity);
    }
    if (!custom) {
        ue::setBool(ws, "bGlobalGravitySet", false);
        g = g_defaultGravity;
    } else {
        ue::setBool(ws, "bGlobalGravitySet", true);
        ue::setNum(ws, "GlobalGravityZ", g);
    }
    ue::setNum(ws, "WorldGravityZ", g);
    ue::setBool(ws, "bWorldGravitySet", true);
}

static void setDilation(double v) {
    ue::Call c(statics(), "SetGlobalTimeDilation");
    c.obj("WorldContextObject", world()).num("TimeDilation", v).run();
}

static void applyMods() {
    if (g_panicHold && g_cfgText == g_panicText) return;
    g_panicHold = false;
    auto opp = [](const Char& c) { return c.v.role == "opponent" && !c.v.dead; };

    if (!on("random_sizes")) g_randomSize.clear();
    if (!on("ever_growing") && !on("growth_spurt")) g_growth.clear();
    drive(g_modScale, [&](const Char& c, double& v) {
        if (c.v.role == "player") {
            if (!on("growth_spurt")) return false;
            double& g = g_growth[c.v.id];
            g = std::min((g ? g : 1) * 1.01, num("growth_cap", 2.5));
            v = round2(g);
            return true;
        }
        if (!opp(c)) return false;
        if (on("ever_growing")) {
            double& g = g_growth[c.v.id];
            g = std::min((g ? g : 1) * 1.015, 3.0);
            v = round2(g);
            return true;
        }
        if (on("random_sizes")) {
            auto it = g_randomSize.find(c.v.id);
            if (it == g_randomSize.end())
                it = g_randomSize.emplace(c.v.id, round2(rnd(num("random_min", 0.4), num("random_max", 2.2)))).first;
            v = it->second;
            return true;
        }
        if (on("opp_scale_on")) { v = num("opp_scale", 1.8); return true; }
        return false;
    }, setScale, 1);

    drive(g_modMass, [&](const Char& c, double& v) {
        if (c.v.role == "player" && on("anvil")) { v = 4; return true; }
        if (opp(c) && on("feather_opp")) { v = 0.15; return true; }
        return false;
    }, setMass, 1);

    bool matrix = on("matrix");
    if (!on("caffeine")) g_randomSpeed.clear();
    drive(g_modSpeed, [&](const Char& c, double& v) {
        if (c.v.role == "player") { if (!matrix) return false; v = 1 / 0.3; return true; }
        if (!opp(c)) return false;
        if (on("freeze_opp")) { v = 0.02; return true; }
        if (on("hyper_opp")) { v = 2.5; return true; }
        if (on("caffeine")) {
            auto it = g_randomSpeed.find(c.v.id);
            if (it == g_randomSpeed.end()) it = g_randomSpeed.emplace(c.v.id, round2(rnd(0.5, 1.8))).first;
            v = it->second;
            return true;
        }
        return false;
    }, setSpeed, 1);

    double dil = matrix ? 0.3 : on("slowmo") ? num("slowmo_value", 0.4) : 1;
    if (!g_dilationSet || dil != g_dilationApplied) {
        if (g_dilationSet || dil != 1) setDilation(dil);
        g_dilationApplied = dil;
        g_dilationSet = true;
    }

    std::string g = str("gravity", "normal");
    static const std::map<std::string, double> presets = {
        {"moon", -162}, {"mars", -371}, {"zero", 0}, {"heavy", -2940}, {"reverse", 250}};
    bool custom = g != "normal";
    double gv = g == "custom" ? num("gravity_custom", -980) : custom && presets.count(g) ? presets.at(g) : 0;
    if (!custom) gv = NAN;
    bool same = (std::isnan(gv) && std::isnan(g_gravityApplied)) || gv == g_gravityApplied;
    if (!g_gravitySet || !same) {
        if (g_gravitySet || custom) setGravity(custom, gv);
        g_gravityApplied = gv;
        g_gravitySet = true;
    }

    if (on("god_mode") && g_player) {
        for (auto& p : readProps(g_player, {"health", "blood", "conscious", "stamina", "energy", "life", "vital"})) {
            if (p.t == "bool" || hasWord(p.n, {"damage", "pain", "bleed", "loss", "drain", "wound", "injur", "fatigue", "max"})) continue;
            auto it = g_godFloor.find(p.n);
            if (it == g_godFloor.end() || p.v > it->second) g_godFloor[p.n] = p.v;
            else if (p.v < it->second) ue::setNum(g_player, p.n.c_str(), it->second);
        }
    } else {
        g_godFloor.clear();
    }

    if (g_player) {
        double factor = on("giant_weapons") ? num("weapon_factor", 2.5) : 0;
        for (auto& gv2 : g_gear[idOf(g_player)]) {
            if (gv2.kind != "weapon") continue;
            Obj o = g_reg[gv2.id];
            if (!ue::alive(o)) continue;
            double want = factor ? factor : 1;
            if (std::fabs(gv2.scale - want) > 0.01 && (factor || g_touchScale.count(gv2.id))) {
                setScale(o, want);
                gv2.scale = want;
            }
        }
    }
}

static void panicReset() {
    for (auto& [id, o] : g_touchScale) if (ue::alive(o)) setScale(o, 1);
    for (auto& [id, o] : g_touchMass) if (ue::alive(o)) setMass(o, 1);
    for (auto& [id, o] : g_touchSpeed) if (ue::alive(o)) setSpeed(o, 1);
    g_touchScale.clear();
    g_touchMass.clear();
    g_touchSpeed.clear();
    g_massKnown.clear();
    g_modScale.clear(); g_modMass.clear(); g_modSpeed.clear();
    g_randomSize.clear(); g_randomSpeed.clear(); g_growth.clear(); g_godFloor.clear();
    setDilation(1);
    g_dilationApplied = 1;
    setGravity(false, 0);
    g_gravityApplied = NAN;
    g_panicText = g_cfgText;
    g_panicHold = true;
    note("panic reset: everything the Lab changed is back to normal (mods paused until you change one)");
}

// ------------------------------------------------------------------ dump

static void dumpObject(FILE* f, const std::string& label, Obj o) {
    if (!o) { fprintf(f, "== %s: <none>\n\n", label.c_str()); return; }
    fprintf(f, "== %s: %s\n", label.c_str(), ue::fullName(o).c_str());
    for (Obj c = ue::classOf(o); c; c = ue::superOf(c)) {
        std::string cn = ue::name(c);
        if (cn == "Actor" || cn == "Object") break;
        fprintf(f, "  -- class %s\n", ue::fullName(c).c_str());
        ue::forProps(c, [&](const ue::Prop& p) {
            std::string v;
            double d;
            switch (ue::kindOf(p)) {
                case ue::Kind::Float: case ue::Kind::Int:
                    if (ue::readNumber(o, p, d)) { char b[64]; snprintf(b, sizeof b, "%.9g", d); v = b; }
                    break;
                case ue::Kind::Bool: v = ue::readBool(o, p) ? "true" : "false"; break;
                case ue::Kind::Object: case ue::Kind::WeakObject: {
                    Obj r = ue::readObject(o, p);
                    v = r ? ue::fullName(r) : "nil";
                    break;
                }
                case ue::Kind::Str: case ue::Kind::Name: v = ue::readString(o, p); break;
                case ue::Kind::Array: v = "len=" + std::to_string(rd<int32_t>(o + p.offset + 8)); break;
                case ue::Kind::Struct: v = ue::name(ue::propStruct(p)); break;
                default: break;
            }
            fprintf(f, "    %-34s %-20s +0x%-5x %s\n", p.name.c_str(), p.type.c_str(), p.offset, v.c_str());
            return true;
        }, false);
    }
    fprintf(f, "\n");
}

static void dump() {
    FILE* f = _wfopen((g_bridge + L"dump.txt").c_str(), L"w");
    if (!f) { note("dump: cannot write bridge/dump.txt"); return; }
    Obj w = world();
    Obj pc = playerController(w);
    fprintf(f, "Half Sword Lab %s dump\n", VERSION);
    fprintf(f, "layout super=0x%x children=0x%x childProps=0x%x ffNext=0x%x ffName=0x%x offset=0x%x propSize=0x%x funcFlags=0x%x pe=0x%x\n\n",
            ue::L.super, ue::L.children, ue::L.childProps, ue::L.ffNext, ue::L.ffName, ue::L.propOffset,
            ue::L.propSize, ue::L.funcFlags, ue::L.peIndex);
    dumpObject(f, "Player pawn", g_player);
    dumpObject(f, "PlayerController", pc);
    dumpObject(f, "PlayerState", ue::getObj(pc, "PlayerState"));
    dumpObject(f, "GameInstance", ue::getObj(w, "OwningGameInstance"));
    dumpObject(f, "GameMode", ue::getObj(w, "AuthorityGameMode"));
    dumpObject(f, "GameState", ue::getObj(w, "GameState"));
    if (Obj sg = ue::findClass("SaveGame"))
        ue::each([&](Obj o) { if (!ue::isCDO(o) && ue::isA(o, sg)) dumpObject(f, "SaveGame", o); });
    std::unordered_set<std::string> done;
    for (auto& c : g_chars)
        if (c.v.role != "player" && done.insert(c.v.cls).second) dumpObject(f, "Character (" + c.v.role + ")", c.obj);
    fprintf(f, "== Loaded blueprint classes\n");
    Obj bpgc = ue::findClass("BlueprintGeneratedClass");
    ue::each([&](Obj o) { if (ue::classOf(o) == bpgc) fprintf(f, "  %s\n", ue::pathName(o).c_str()); });
    fclose(f);
    note("dump written to bridge/dump.txt");
}

// ------------------------------------------------------------------ commands

static long long g_lastCmd = -1;

static std::vector<std::pair<long long, Command>> readCommandFile() {
    std::vector<std::pair<long long, Command>> out;
    std::string text;
    if (!readFile(g_bridge + L"commands.txt", text)) return out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> parts;
        size_t s = 0;
        while (true) {
            size_t t = line.find('\t', s);
            parts.push_back(line.substr(s, t == std::string::npos ? std::string::npos : t - s));
            if (t == std::string::npos) break;
            s = t + 1;
        }
        if (parts.size() < 2) continue;
        char* e;
        long long id = strtoll(parts[0].c_str(), &e, 10);
        if (e == parts[0].c_str()) continue;
        out.push_back({id, {parts[1], std::vector<std::string>(parts.begin() + 2, parts.end())}});
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.first < b.first; });
    return out;
}

static double toNum(const std::string& s, double d) {
    char* e;
    double v = strtod(s.c_str(), &e);
    return e != s.c_str() ? v : d;
}

static void run(const Command& c) {
    const auto& a = c.args;
    auto arg = [&](size_t i) { return i < a.size() ? a[i] : std::string(); };
    const std::string& v = c.verb;

    if (v == "set") {
        Obj o = target(arg(0));
        if (!o) return;
        ue::Prop p = ue::findProp(ue::classOf(o), arg(1));
        if (!p) { note("set: no stat called " + arg(1)); return; }
        if (ue::kindOf(p) == ue::Kind::Bool) ue::writeBool(o, p, arg(2) == "true" || arg(2) == "1");
        else ue::writeNumber(o, p, toNum(arg(2), 0));
        note("set " + arg(1) + " = " + arg(2));
    } else if (v == "scale") {
        if (Obj o = target(arg(0))) { setScale(o, toNum(arg(1), 1)); note("scale -> " + arg(1)); }
    } else if (v == "mass") {
        if (Obj o = target(arg(0))) { setMass(o, toNum(arg(1), 1)); note("weight x" + arg(1)); }
    } else if (v == "speed") {
        if (Obj o = target(arg(0))) { setSpeed(o, toNum(arg(1), 1)); note("speed x" + arg(1)); }
    } else if (v == "launch") {
        double power = toNum(arg(1), 1500);
        if (arg(0) == "all") {
            for (auto& ch : g_chars) if (ch.v.role != "player") launch(ch.obj, power);
            note("launched everyone");
        } else if (Obj o = target(arg(0))) { launch(o, power); note("launched " + ue::name(o)); }
    } else if (v == "bring") {
        Obj o = target(arg(0));
        if (o && g_player) { teleport(o, nearPos(g_player, 160)); note("brought " + ue::name(o) + " to you"); }
    } else if (v == "visit") {
        Obj o = target(arg(0));
        if (o && g_player) { teleport(g_player, nearPos(o, 160)); note("teleported to " + ue::name(o)); }
    } else if (v == "clone") {
        Obj o = target(arg(0));
        if (!o) return;
        int n = std::min(12, std::max(1, (int)toNum(arg(1), 1))), made = 0;
        for (int i = 0; i < n; i++) if (spawnClass(ue::classOf(o), nearPos(o, rnd(200, 400)))) made++;
        note("cloned " + ue::name(o) + " x" + std::to_string(made));
    } else if (v == "spawn") {
        Obj cls = ue::findByPath(arg(0));
        if (!cls) { note("spawn: class not loaded: " + arg(0)); return; }
        Obj around = !arg(2).empty() ? target(arg(2)) : g_player;
        if (!around) around = g_player;
        if (!around) return;
        int n = std::min(12, std::max(1, (int)toNum(arg(1), 1))), made = 0;
        for (int i = 0; i < n; i++) if (spawnClass(cls, nearPos(around, rnd(150, 270)))) made++;
        note("spawned " + ue::name(cls) + " x" + std::to_string(made));
    } else if (v == "vanish") {
        if (Obj o = target(arg(0))) { destroy(o); g_reg.erase(arg(0)); note("vanished " + ue::name(o)); }
    } else if (v == "hide") {
        if (Obj o = target(arg(0))) setHidden(o, true);
    } else if (v == "show") {
        if (Obj o = target(arg(0))) setHidden(o, false);
    } else if (v == "reset") {
        if (Obj o = target(arg(0))) {
            setScale(o, 1);
            setSpeed(o, 1);
            if (meshOf(o)) setMass(o, 1);
            note("reset " + ue::name(o));
        }
    } else if (v == "scan_classes") {
        scanClasses();
    } else if (v == "dump") {
        dump();
    } else if (v == "panic") {
        panicReset();
    } else {
        note("unknown command " + v);
    }
}

// ------------------------------------------------------------------ state.json

static std::string esc(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return "\"" + o + "\"";
}

static std::string jnum(double v) {
    if (!std::isfinite(v)) return "null";
    char b[64];
    snprintf(b, sizeof b, "%.9g", v);
    return b;
}

static std::string propsJson(const std::vector<PropView>& ps) {
    std::string s = "[";
    for (size_t i = 0; i < ps.size(); i++) {
        auto& p = ps[i];
        s += (i ? "," : "") + std::string("{\"n\":") + esc(p.n) + ",\"t\":" + esc(p.t) + ",\"v\":" +
             (p.t == "bool" ? (p.v != 0 ? "true" : "false") : jnum(p.v)) + "}";
    }
    return s + "]";
}

static std::string classesJson(const std::vector<ClassView>& cs) {
    std::string s = "[";
    for (size_t i = 0; i < cs.size(); i++)
        s += (i ? "," : "") + std::string("{\"name\":") + esc(cs[i].name) + ",\"path\":" + esc(cs[i].path) + "}";
    return s + "]";
}

static void writeState(const std::vector<CoinView>& eco, const std::string& map, double gravity) {
    std::string s = "{\"version\":" + esc(VERSION) + ",\"t\":" + std::to_string((long long)time(nullptr)) +
                    ",\"ack\":" + std::to_string(g_lastCmd) + ",\"world\":{\"map\":" + esc(map) +
                    ",\"gravity\":" + jnum(gravity) + ",\"paused\":" + (g_panicHold ? "true" : "false") +
                    "},\"characters\":[";
    for (size_t i = 0; i < g_chars.size(); i++) {
        auto& c = g_chars[i].v;
        s += (i ? "," : "") + std::string("{\"id\":") + esc(c.id) + ",\"name\":" + esc(c.name) + ",\"class\":" +
             esc(c.cls) + ",\"role\":" + esc(c.role) + ",\"dead\":" + (c.dead ? "true" : "false") +
             ",\"scale\":" + jnum(c.scale) + ",\"mass\":" + jnum(c.mass) + ",\"speed\":" + jnum(c.speed) +
             ",\"props\":" + propsJson(c.props) + ",\"gear\":[";
        for (size_t j = 0; j < c.gear.size(); j++) {
            auto& g = c.gear[j];
            s += (j ? "," : "") + std::string("{\"id\":") + esc(g.id) + ",\"name\":" + esc(g.name) + ",\"class\":" +
                 esc(g.cls) + ",\"kind\":" + esc(g.kind) + ",\"src\":" + esc(g.src) + ",\"scale\":" + jnum(g.scale) + "}";
        }
        s += "]}";
    }
    s += "],\"economy\":[";
    for (size_t i = 0; i < eco.size(); i++) {
        auto& e = eco[i];
        s += (i ? "," : "") + std::string("{\"owner\":") + esc(e.owner) + ",\"owner_id\":" + esc(e.ownerId) +
             ",\"n\":" + esc(e.n) + ",\"t\":" + esc(e.t) + ",\"v\":" + (e.t == "bool" ? (e.v != 0 ? "true" : "false") : jnum(e.v)) + "}";
    }
    s += "],\"classes\":{\"weapons\":" + classesJson(g_weaponClasses) + ",\"armor\":" + classesJson(g_armorClasses) +
         ",\"characters\":" + classesJson(g_charClasses) + "},\"log\":[";
    {
        std::lock_guard<std::mutex> l(g_mx);
        int seq = g_logSeq - (int)g_log.size();
        bool first = true;
        for (auto& m : g_log) {
            s += (first ? "" : ",") + std::string("{\"seq\":") + std::to_string(++seq) + ",\"msg\":" + esc(m) + "}";
            first = false;
        }
    }
    s += "]}";
    writeFileAtomic(g_bridge + L"state.json", s);
}

// ------------------------------------------------------------------ backups (in-game)

static void makeDirs(const std::wstring& path) {
    for (size_t i = path.find(L'\\', 3); ; i = path.find(L'\\', i + 1)) {
        CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
        if (i == std::wstring::npos) break;
    }
}

static bool copyTree(const std::wstring& from, const std::wstring& to) {
    makeDirs(to);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((from + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok &= copyTree(from + L"\\" + n, to + L"\\" + n);
        else ok &= CopyFileW((from + L"\\" + n).c_str(), (to + L"\\" + n).c_str(), FALSE) != 0;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

std::string backupSaves(const std::string& reason) {
    auto fail = [](const std::string& m) {
        note("backup failed: " + m);
        return "Backup failed: " + m;
    };
    std::string ini;
    readFile(g_home + L"lab.ini", ini);
    std::wstring dest = widen(iniGet(ini, "backup_dir", ""));
    if (dest.empty()) dest = g_home + L"backups";
    // ...\<Project>\Binaries\Win64\HalfSwordLab\ -> <Project>
    std::wstring p = g_home.substr(0, g_home.size() - 1);
    for (int i = 0; i < 3; i++) p = p.substr(0, p.find_last_of(L'\\'));
    std::wstring project = p.substr(p.find_last_of(L'\\') + 1);
    wchar_t local[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return fail("LOCALAPPDATA not set");
    std::wstring saves = std::wstring(local) + L"\\" + project + L"\\Saved\\SaveGames";
    if (GetFileAttributesW(saves.c_str()) == INVALID_FILE_ATTRIBUTES) return fail("no save folder yet: " + narrow(saves));
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[64];
    swprintf(stamp, 64, L"saves-%04d%02d%02d-%02d%02d%02d-%ls", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
             widen(reason).c_str());
    CreateDirectoryW(dest.c_str(), nullptr);
    std::wstring out = dest + L"\\" + stamp;
    if (!copyTree(saves, out + L"\\" + project + L"\\Saved\\SaveGames")) return fail("could not copy " + narrow(saves) + " to " + narrow(out));
    char created[64];
    snprintf(created, sizeof created, "%04d-%02d-%02dT%02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    writeFileAtomic(out + L".json", std::string("{\"kind\":\"saves\",\"reason\":") + esc(reason) +
                                        ",\"created\":" + esc(created) + ",\"format\":\"dir\",\"source\":" +
                                        esc(narrow(std::wstring(local))) + "}");
    note("saves backed up: " + narrow(stamp));
    return "Backed up to " + narrow(out);
}

// ------------------------------------------------------------------ main tick

static DWORD g_lastScan = 0, g_lastState = 0, g_lastGear = 0, g_lastClasses = 0;
static bool g_started = false;

void start() {
    CreateDirectoryW(g_bridge.c_str(), nullptr);
    // Skip commands left over from an earlier session.
    for (auto& [id, c] : readCommandFile()) g_lastCmd = std::max(g_lastCmd, id);
    if (g_lastCmd < 0) g_lastCmd = 0;
    g_started = true;
    note(std::string("Half Sword Lab ") + VERSION + " ready");
    setStatus("ready");
}

void onGameThread() {
    if (!g_started) return;
    resetReadCache();
    DWORD now = GetTickCount();

    // Commands: file bridge + overlay queue, every call (the hook throttles us to ~60/s).
    for (auto& [id, c] : readCommandFile()) {
        if (id <= g_lastCmd) continue;
        g_lastCmd = id;
        run(c);
    }
    std::deque<Command> local;
    {
        std::lock_guard<std::mutex> l(g_mx);
        local.swap(g_queue);
    }
    for (auto& c : local) run(c);

    if (now - g_lastScan < 500) return;
    g_lastScan = now;
    if (!g_lastClasses || now - g_lastClasses > 30000) { scanClasses(); g_lastClasses = now; }
    bool gear = !g_lastGear || now - g_lastGear > 3000;
    if (gear) g_lastGear = now;
    scanWorld(gear);
    readCfg();
    applyMods();

    if (now - g_lastState < 1000) return;
    g_lastState = now;
    auto eco = economy();
    Obj w = world();
    double gravity = NAN;
    ue::getNum(worldSettings(), "WorldGravityZ", gravity);
    std::string map = w ? ue::name(w) : "";
    writeState(eco, map, gravity);

    Snapshot s;
    s.ready = true;
    s.map = map;
    s.gravity = gravity;
    s.paused = g_panicHold;
    for (auto& c : g_chars) s.chars.push_back(c.v);
    s.economy = eco;
    s.weapons = g_weaponClasses;
    s.armor = g_armorClasses;
    std::lock_guard<std::mutex> l(g_mx);
    g_snap = std::move(s);
}

}  // namespace lab
