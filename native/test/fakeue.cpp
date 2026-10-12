// A fake "UE5 game" for testing the Lab DLL without Half Sword.
//
// It lays out its objects exactly like a UE5 shipping build does (GUObjectArray chunks, the FName
// pool, UClass/UFunction objects, FField/FProperty chains, a UObject vtable whose ProcessEvent does
// the usual FunctionFlags tests) and implements the engine functions the Lab calls. It imports
// version.dll, so with WINEDLLOVERRIDES=version=n,b the Lab's proxy gets loaded like in the game.
//
// Env: HSL_SECONDS (run time, default 25), HSL_MENU_AT (seconds; press '\' then), HSL_NO_GFX=1.
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

using Obj = uint8_t*;

// ------------------------------------------------------------------ engine statics (found by shape)

struct FNameEntryAllocator {
    uint64_t lock;
    uint32_t currentBlock;
    uint32_t cursor;
    uint8_t* blocks[8192];
};
FNameEntryAllocator NamePoolData;

struct FUObjectItem { Obj obj; int32_t flags, cluster, serial, pad; };
struct FChunkedFixedUObjectArray { FUObjectItem** objects; FUObjectItem* prealloc; int32_t maxE, numE, maxC, numC; };
struct FUObjectArray { int32_t firstGC, lastNonGC, maxNotGC; bool open; FChunkedFixedUObjectArray objObjects; };
FUObjectArray GUObjectArray;

// ------------------------------------------------------------------ names

static std::unordered_map<std::string, int32_t> g_names;

static int32_t nameIdx(const std::string& s) {
    auto it = g_names.find(s);
    if (it != g_names.end()) return it->second;
    uint32_t need = (uint32_t)((2 + s.size() + 1) & ~1u);
    if (NamePoolData.cursor + need > 131072) {
        NamePoolData.currentBlock++;
        NamePoolData.cursor = 0;
        NamePoolData.blocks[NamePoolData.currentBlock] = (uint8_t*)calloc(131072, 1);
    }
    uint8_t* e = NamePoolData.blocks[NamePoolData.currentBlock] + NamePoolData.cursor;
    *(uint16_t*)e = (uint16_t)(s.size() << 6);
    memcpy(e + 2, s.data(), s.size());
    int32_t idx = (int32_t)((NamePoolData.currentBlock << 16) | (NamePoolData.cursor / 2));
    NamePoolData.cursor += need;
    return g_names[s] = idx;
}

// ------------------------------------------------------------------ objects

static void* g_vtable[0x60];
static Obj ClassClass, PackageClass, FunctionClass, ScriptStructClass, BPGC;

static Obj newObj(Obj cls, const std::string& name, Obj outer, size_t size = 0x400, int number = 0) {
    auto& a = GUObjectArray.objObjects;
    int32_t idx = a.numE;
    if (idx / 65536 >= a.numC) {
        a.objects[a.numC] = (FUObjectItem*)calloc(65536, sizeof(FUObjectItem));
        a.numC++;
    }
    Obj o = (Obj)calloc(1, size);
    *(void***)o = g_vtable;
    *(int32_t*)(o + 0xC) = idx;
    *(Obj*)(o + 0x10) = cls;
    *(int32_t*)(o + 0x18) = nameIdx(name);
    *(int32_t*)(o + 0x1C) = number;
    *(Obj*)(o + 0x20) = outer;
    a.objects[idx / 65536][idx % 65536].obj = o;
    a.numE = idx + 1;
    return o;
}

static std::string objName(Obj o) {
    int32_t idx = *(int32_t*)(o + 0x18);
    uint8_t* e = NamePoolData.blocks[idx >> 16] + (idx & 0xFFFF) * 2;
    return std::string((char*)e + 2, *(uint16_t*)e >> 6);
}

static std::map<std::string, Obj> g_pkgs;
static Obj pkg(const std::string& n) {
    auto& p = g_pkgs[n];
    if (!p) p = newObj(PackageClass, n, nullptr);
    return p;
}

static Obj makeStruct(Obj meta, const char* name, const char* package, Obj super) {
    Obj o = newObj(meta, name, pkg(package));
    *(Obj*)(o + 0x40) = super;
    return o;
}
static Obj makeClass(const char* name, const char* package, Obj super) { return makeStruct(ClassClass, name, package, super); }

static std::map<std::string, uint8_t*> g_fieldClasses;
static uint8_t* fieldClass(const std::string& t) {
    auto& f = g_fieldClasses[t];
    if (!f) {
        f = (uint8_t*)calloc(0x40, 1);
        *(int32_t*)f = nameIdx(t);
    }
    return f;
}

constexpr uint64_t CPF_Parm = 0x80, CPF_OutParm = 0x100, CPF_ReturnParm = 0x400;

static uint8_t* addProp(Obj owner, const char* name, const char* type, int32_t offset, int32_t size,
                        void* extra = nullptr, uint64_t flags = 0, uint8_t boolMask = 0xFF) {
    uint8_t* f = (uint8_t*)calloc(0x100, 1);
    *(uint8_t**)(f + 0x8) = fieldClass(type);
    *(Obj*)(f + 0x10) = owner;
    *(int32_t*)(f + 0x28) = nameIdx(name);
    *(int32_t*)(f + 0x38) = 1;
    *(int32_t*)(f + 0x3C) = size;
    *(uint64_t*)(f + 0x40) = flags;
    *(int32_t*)(f + 0x4C) = offset;
    if (!strcmp(type, "BoolProperty")) {
        f[0x78] = 1;
        f[0x79] = 0;
        f[0x7A] = boolMask;
        f[0x7B] = boolMask;
    } else {
        *(void**)(f + 0x78) = extra;
    }
    uint8_t** pp = (uint8_t**)(owner + 0x50);
    while (*pp) pp = (uint8_t**)(*pp + 0x20);
    *pp = f;
    return f;
}

static Obj addFunc(Obj cls, const char* name, uint32_t flags) {
    Obj fn = newObj(FunctionClass, name, cls, 0x100);
    *(uint32_t*)(fn + 0xB0) = flags;
    Obj* pp = (Obj*)(cls + 0x48);
    while (*pp) pp = (Obj*)(*pp + 0x28);
    *pp = fn;
    return fn;
}

static void param(Obj fn, const char* name, const char* type, int32_t off, int32_t size, void* extra = nullptr, bool ret = false) {
    addProp(fn, name, type, off, size, extra, CPF_Parm | (ret ? CPF_ReturnParm | CPF_OutParm : 0));
}

constexpr uint32_t NATIVE_CALLABLE = 0x400 | 0x1 | 0x20000 | 0x04000000, F_PURE = 0x10000000, F_STATIC = 0x2000;

// ------------------------------------------------------------------ world

static Obj Vector, Quat, Transform, ActorClass, Character, WillieBP, Sword, Helmet, Level, World, WS, GI;
static Obj g_player, g_merc, g_opp, g_swordObj, g_helmObj, g_mercHelmet, g_level;
static float g_dilation = 1;
static int g_spawned = 0;

static void setVec(Obj o, int off, double x, double y, double z) {
    double* d = (double*)(o + off);
    d[0] = x; d[1] = y; d[2] = z;
}

static Obj makeActor(Obj cls, const char* name, int num, double x) {
    Obj a = newObj(cls, name, g_level, 0x400, num);
    *(float*)(a + 0x68) = 1;           // CustomTimeDilation
    setVec(a, 0x1E0, 1, 1, 1);         // scale
    setVec(a, 0x1C0, x, 0, 100);       // location
    return a;
}

// ------------------------------------------------------------------ ProcessEvent

static std::map<Obj, std::string> g_fnNames;

static void dispatch(Obj self, Obj fn, uint8_t* p) {
    const std::string& n = g_fnNames[fn];
    if (n == "K2_DestroyActor") *(uint32_t*)(self + 0x8) |= 0x8000;
    else if (n == "GetActorScale3D") memcpy(p, self + 0x1E0, 24);
    else if (n == "SetActorScale3D") memcpy(self + 0x1E0, p, 24);
    else if (n == "K2_GetActorLocation") memcpy(p, self + 0x1C0, 24);
    else if (n == "K2_SetActorLocation") { memcpy(self + 0x1C0, p, 24); p[0xA9] = 1; }
    else if (n == "SetActorHiddenInGame") self[0x78] = p[0] ? (self[0x78] | 2) : (self[0x78] & ~2);
    else if (n == "GetAttachParentActor") *(Obj*)p = *(Obj*)(self + 0x70);
    else if (n == "LaunchCharacter") *(double*)(self + 0x1A0) = ((double*)p)[2];
    else if (n == "SetAllMassScale") *(float*)(self + 0x1F8) = *(float*)p;
    else if (n == "SetRelativeScale3D") memcpy(self + 0x140, p, 24);
    else if (n == "SetVisibility") self[0x1A8] = p[0];
    else if (n == "SetGlobalTimeDilation") g_dilation = *(float*)(p + 8);
    else if (n == "BeginDeferredActorSpawnFromClass") {
        Obj cls = *(Obj*)(p + 8);
        double* t = (double*)(p + 0x10 + 0x20);
        Obj a = makeActor(cls, ("Spawned_" + std::to_string(g_spawned)).c_str(), 0, t[0]);
        setVec(a, 0x1C0, t[0], t[1], t[2]);
        g_spawned++;
        *(Obj*)(p + 0x88) = a;
    } else if (n == "FinishSpawningActor") *(Obj*)(p + 0x78) = *(Obj*)p;
}

extern "C" __attribute__((noinline)) void FakeProcessEvent(Obj self, Obj fn, void* parms) {
    unsigned char native, outParms;
    // The two tests every UE ProcessEvent does on UFunction::FunctionFlags (FUNC_Native, FUNC_HasOutParms).
    __asm__ volatile("testl $0x400, 0xB0(%%rdx)\n\tsetnz %0\n\ttestl $0x400000, 0xB0(%%rdx)\n\tsetnz %1"
                     : "=q"(native), "=q"(outParms)
                     : "d"(fn)
                     : "cc");
    (void)outParms;
    if (native) dispatch(self, fn, (uint8_t*)parms);
}

extern "C" __attribute__((noinline)) void VtDummy() { __asm__ volatile(""); }

// ------------------------------------------------------------------ build the fake engine

static void build() {
    NamePoolData.blocks[0] = (uint8_t*)calloc(131072, 1);
    nameIdx("None");
    nameIdx("ByteProperty");
    auto& a = GUObjectArray.objObjects;
    a.maxE = 0x100000;
    a.maxC = a.maxE / 65536;
    a.objects = (FUObjectItem**)calloc(a.maxC, sizeof(void*));
    for (auto& v : g_vtable) v = (void*)VtDummy;
    g_vtable[0x4C] = (void*)FakeProcessEvent;

    ClassClass = newObj(nullptr, "Class", nullptr);
    *(Obj*)(ClassClass + 0x10) = ClassClass;
    PackageClass = newObj(ClassClass, "Package", nullptr);
    *(Obj*)(ClassClass + 0x20) = pkg("/Script/CoreUObject");
    *(Obj*)(PackageClass + 0x20) = pkg("/Script/CoreUObject");
    for (auto& [n, p] : g_pkgs) *(Obj*)(p + 0x10) = PackageClass;
    Obj Object = makeClass("Object", "/Script/CoreUObject", nullptr);
    *(Obj*)(ClassClass + 0x40) = Object;
    *(Obj*)(PackageClass + 0x40) = Object;
    FunctionClass = makeClass("Function", "/Script/CoreUObject", Object);
    ScriptStructClass = makeClass("ScriptStruct", "/Script/CoreUObject", Object);
    BPGC = makeClass("BlueprintGeneratedClass", "/Script/Engine", ClassClass);

    Vector = makeStruct(ScriptStructClass, "Vector", "/Script/CoreUObject", nullptr);
    addProp(Vector, "X", "DoubleProperty", 0, 8);
    addProp(Vector, "Y", "DoubleProperty", 8, 8);
    addProp(Vector, "Z", "DoubleProperty", 16, 8);
    Quat = makeStruct(ScriptStructClass, "Quat", "/Script/CoreUObject", nullptr);
    addProp(Quat, "X", "DoubleProperty", 0, 8);
    addProp(Quat, "Y", "DoubleProperty", 8, 8);
    addProp(Quat, "Z", "DoubleProperty", 16, 8);
    addProp(Quat, "W", "DoubleProperty", 24, 8);
    Transform = makeStruct(ScriptStructClass, "Transform", "/Script/CoreUObject", nullptr);
    addProp(Transform, "Rotation", "StructProperty", 0, 32, Quat);
    addProp(Transform, "Translation", "StructProperty", 0x20, 24, Vector);
    addProp(Transform, "Scale3D", "StructProperty", 0x40, 24, Vector);
    Obj TickFn = makeStruct(ScriptStructClass, "ActorTickFunction", "/Script/Engine", nullptr);

    // Components
    Obj ActorComp = makeClass("ActorComponent", "/Script/Engine", Object);
    Obj Scene = makeClass("SceneComponent", "/Script/Engine", ActorComp);
    Obj Prim = makeClass("PrimitiveComponent", "/Script/Engine", Scene);
    Obj MeshComp = makeClass("MeshComponent", "/Script/Engine", Prim);
    Obj Skinned = makeClass("SkinnedMeshComponent", "/Script/Engine", MeshComp);
    Obj SkelMesh = makeClass("SkeletalMeshComponent", "/Script/Engine", Skinned);
    Obj StaticMeshComp = makeClass("StaticMeshComponent", "/Script/Engine", MeshComp);
    Obj StaticMesh = makeClass("StaticMesh", "/Script/Engine", Object);
    addProp(Scene, "RelativeScale3D", "StructProperty", 0x140, 24, Vector);
    addProp(StaticMeshComp, "StaticMesh", "ObjectProperty", 0x180, 8, StaticMesh);
    Obj f;
    f = addFunc(ActorComp, "K2_DestroyComponent", NATIVE_CALLABLE); param(f, "Object", "ObjectProperty", 0, 8, Object);
    f = addFunc(Scene, "SetRelativeScale3D", NATIVE_CALLABLE); param(f, "NewScale3D", "StructProperty", 0, 24, Vector);
    f = addFunc(Scene, "SetVisibility", NATIVE_CALLABLE); param(f, "bNewVisibility", "BoolProperty", 0, 1); param(f, "bPropagateToChildren", "BoolProperty", 1, 1);
    f = addFunc(Scene, "K2_SetWorldLocation", NATIVE_CALLABLE); param(f, "NewLocation", "StructProperty", 0, 24, Vector); param(f, "bSweep", "BoolProperty", 0x18, 1); param(f, "bTeleport", "BoolProperty", 0xA8, 1);
    f = addFunc(Prim, "SetMassScale", NATIVE_CALLABLE); param(f, "BoneName", "NameProperty", 0, 8); param(f, "InMassScale", "FloatProperty", 8, 4);
    f = addFunc(SkelMesh, "SetAllMassScale", NATIVE_CALLABLE); param(f, "InMassScale", "FloatProperty", 0, 4);
    f = addFunc(SkelMesh, "SetAllPhysicsLinearVelocity", NATIVE_CALLABLE); param(f, "NewVel", "StructProperty", 0, 24, Vector); param(f, "bAddToCurrent", "BoolProperty", 0x18, 1);

    // Actors
    ActorClass = makeClass("Actor", "/Script/Engine", Object);
    addProp(ActorClass, "PrimaryActorTick", "StructProperty", 0x28, 0x30, TickFn);
    addProp(ActorClass, "RootComponent", "ObjectProperty", 0x60, 8, Scene);
    addProp(ActorClass, "CustomTimeDilation", "FloatProperty", 0x68, 4);
    addProp(ActorClass, "Owner", "ObjectProperty", 0x70, 8, ActorClass);
    addProp(ActorClass, "bHidden", "BoolProperty", 0x78, 1, nullptr, 0, 0x02);
    addFunc(ActorClass, "ReceiveTick", 0x08000800);
    addFunc(ActorClass, "K2_DestroyActor", NATIVE_CALLABLE);
    f = addFunc(ActorClass, "GetActorScale3D", NATIVE_CALLABLE | F_PURE | 0x400000); param(f, "ReturnValue", "StructProperty", 0, 24, Vector, true);
    f = addFunc(ActorClass, "SetActorScale3D", NATIVE_CALLABLE); param(f, "NewScale3D", "StructProperty", 0, 24, Vector);
    f = addFunc(ActorClass, "K2_GetActorLocation", NATIVE_CALLABLE | F_PURE | 0x400000); param(f, "ReturnValue", "StructProperty", 0, 24, Vector, true);
    f = addFunc(ActorClass, "K2_SetActorLocation", NATIVE_CALLABLE | 0x400000);
    param(f, "NewLocation", "StructProperty", 0, 24, Vector); param(f, "bSweep", "BoolProperty", 0x18, 1);
    param(f, "bTeleport", "BoolProperty", 0xA8, 1); param(f, "ReturnValue", "BoolProperty", 0xA9, 1, nullptr, true);
    f = addFunc(ActorClass, "SetActorHiddenInGame", NATIVE_CALLABLE); param(f, "bNewHidden", "BoolProperty", 0, 1);
    f = addFunc(ActorClass, "GetAttachParentActor", NATIVE_CALLABLE | F_PURE | 0x400000); param(f, "ReturnValue", "ObjectProperty", 0, 8, ActorClass, true);
    Obj Pawn = makeClass("Pawn", "/Script/Engine", ActorClass);
    Character = makeClass("Character", "/Script/Engine", Pawn);
    addProp(Character, "Mesh", "ObjectProperty", 0x80, 8, SkelMesh);
    f = addFunc(Character, "LaunchCharacter", NATIVE_CALLABLE);
    param(f, "LaunchVelocity", "StructProperty", 0, 24, Vector); param(f, "bXYOverride", "BoolProperty", 0x18, 1); param(f, "bZOverride", "BoolProperty", 0x19, 1);
    Obj Controller = makeClass("Controller", "/Script/Engine", ActorClass);
    addProp(Controller, "Pawn", "ObjectProperty", 0x80, 8, Pawn);
    Obj LocalPlayer = makeClass("LocalPlayer", "/Script/Engine", Object);
    Obj PC = makeClass("PlayerController", "/Script/Engine", Controller);
    addProp(PC, "Player", "ObjectProperty", 0x88, 8, LocalPlayer);
    addProp(LocalPlayer, "PlayerController", "ObjectProperty", 0x30, 8, PC);
    Obj WSClass = makeClass("WorldSettings", "/Script/Engine", ActorClass);
    addProp(WSClass, "WorldGravityZ", "FloatProperty", 0x100, 4);
    addProp(WSClass, "GlobalGravityZ", "FloatProperty", 0x104, 4);
    addProp(WSClass, "bGlobalGravitySet", "BoolProperty", 0x108, 1, nullptr, 0, 0x01);
    addProp(WSClass, "bWorldGravitySet", "BoolProperty", 0x108, 1, nullptr, 0, 0x02);
    Obj LevelClass = makeClass("Level", "/Script/Engine", Object);
    addProp(LevelClass, "WorldSettings", "ObjectProperty", 0x30, 8, WSClass);
    Obj GIClass = makeClass("GameInstance", "/Script/Engine", Object);
    uint8_t* lp = addProp(GIClass, "LocalPlayers", "ArrayProperty", 0x38, 16);
    uint8_t* inner = (uint8_t*)calloc(0x100, 1);
    *(uint8_t**)(inner + 0x8) = fieldClass("ObjectProperty");
    *(int32_t*)(inner + 0x28) = nameIdx("LocalPlayers");
    *(int32_t*)(inner + 0x3C) = 8;
    *(Obj*)(inner + 0x78) = LocalPlayer;
    *(uint8_t**)(lp + 0x78) = inner;
    Obj WorldClass = makeClass("World", "/Script/Engine", Object);
    addProp(WorldClass, "PersistentLevel", "ObjectProperty", 0x30, 8, LevelClass);
    addProp(WorldClass, "OwningGameInstance", "ObjectProperty", 0x38, 8, GIClass);
    Obj GVC = makeClass("GameViewportClient", "/Script/Engine", Object);
    addProp(GVC, "World", "ObjectProperty", 0x30, 8, WorldClass);
    Obj Engine = makeClass("Engine", "/Script/Engine", Object);
    Obj GameEngine = makeClass("GameEngine", "/Script/Engine", Engine);
    addProp(Engine, "GameViewport", "ObjectProperty", 0x30, 8, GVC);
    Obj Statics = makeClass("GameplayStatics", "/Script/Engine", Object);
    f = addFunc(Statics, "SetGlobalTimeDilation", NATIVE_CALLABLE | F_STATIC);
    param(f, "WorldContextObject", "ObjectProperty", 0, 8, Object); param(f, "TimeDilation", "FloatProperty", 8, 4);
    f = addFunc(Statics, "BeginDeferredActorSpawnFromClass", NATIVE_CALLABLE | F_STATIC | 0x400000);
    param(f, "WorldContextObject", "ObjectProperty", 0, 8, Object); param(f, "ActorClass", "ClassProperty", 8, 8, ClassClass);
    param(f, "SpawnTransform", "StructProperty", 0x10, 0x60, Transform); param(f, "CollisionHandlingOverride", "EnumProperty", 0x70, 1);
    param(f, "Owner", "ObjectProperty", 0x78, 8, ActorClass); param(f, "TransformScaleMethod", "EnumProperty", 0x80, 1);
    param(f, "ReturnValue", "ObjectProperty", 0x88, 8, ActorClass, true);
    f = addFunc(Statics, "FinishSpawningActor", NATIVE_CALLABLE | F_STATIC | 0x400000);
    param(f, "Actor", "ObjectProperty", 0, 8, ActorClass); param(f, "SpawnTransform", "StructProperty", 0x10, 0x60, Transform);
    param(f, "TransformScaleMethod", "EnumProperty", 0x70, 1); param(f, "ReturnValue", "ObjectProperty", 0x78, 8, ActorClass, true);
    Obj SaveGame = makeClass("SaveGame", "/Script/Engine", Object);
    (void)SaveGame;

    // Blueprint (game) classes
    WillieBP = makeStruct(BPGC, "Willie_BP_C", "/Game/Characters/Willie_BP", Character);
    addProp(WillieBP, "Health", "FloatProperty", 0x100, 4);
    addProp(WillieBP, "Blood", "FloatProperty", 0x104, 4);
    addProp(WillieBP, "Team", "IntProperty", 0x108, 4);
    addProp(WillieBP, "Is Hired", "BoolProperty", 0x10C, 1);
    addProp(WillieBP, "Is Dead", "BoolProperty", 0x10D, 1);
    addProp(WillieBP, "Consciousness", "FloatProperty", 0x110, 4);
    Sword = makeStruct(BPGC, "BP_Longsword_C", "/Game/Weapons/BP_Longsword", ActorClass);
    Helmet = makeStruct(BPGC, "BP_Armor_Sallet_C", "/Game/Armor/BP_Armor_Sallet", ActorClass);
    Obj HSGI = makeStruct(BPGC, "HS_GameInstance_C", "/Game/Core/HS_GameInstance", GIClass);
    addProp(HSGI, "Gold", "IntProperty", 0x100, 4);

    // Fill the object array like a real engine would.
    for (int i = 0; i < 25000; i++) newObj(Object, "Filler", pkg("/Engine/Transient"), 0x40, i + 1);

    // CDOs
    newObj(Statics, "Default__GameplayStatics", pkg("/Script/Engine"));
    *(uint32_t*)(GUObjectArray.objObjects.objects[0][GUObjectArray.objObjects.numE - 1].obj + 0x8) = 0x10;

    // World
    World = newObj(WorldClass, "Arena", pkg("/Game/Maps/Arena"));
    g_level = Level = newObj(LevelClass, "PersistentLevel", World);
    *(Obj*)(World + 0x30) = Level;
    WS = newObj(WSClass, "WorldSettings", Level);
    *(float*)(WS + 0x100) = -980;
    *(Obj*)(Level + 0x30) = WS;
    GI = newObj(HSGI, "HS_GameInstance_C", pkg("/Engine/Transient"), 0x400, 1);
    *(int32_t*)(GI + 0x100) = 240;
    *(Obj*)(World + 0x38) = GI;
    Obj lpObj = newObj(LocalPlayer, "LocalPlayer", GI, 0x400, 1);
    Obj* arr = (Obj*)calloc(4, sizeof(Obj));
    arr[0] = lpObj;
    *(Obj**)(GI + 0x38) = arr;
    *(int32_t*)(GI + 0x40) = 1;
    *(int32_t*)(GI + 0x44) = 4;
    Obj pc = newObj(PC, "PlayerController", Level, 0x400, 1);
    *(Obj*)(lpObj + 0x30) = pc;
    *(Obj*)(pc + 0x88) = lpObj;
    Obj eng = newObj(GameEngine, "GameEngine", pkg("/Engine/Transient"), 0x400, 1);
    Obj vp = newObj(GVC, "GameViewportClient", eng, 0x400, 1);
    *(Obj*)(eng + 0x30) = vp;
    *(Obj*)(vp + 0x30) = World;

    auto character = [&](int num, double x, float hp, int team, bool hired) {
        Obj c = makeActor(WillieBP, "Willie_BP_C", num, x);
        *(float*)(c + 0x100) = hp;
        *(float*)(c + 0x104) = 5000;
        *(int32_t*)(c + 0x108) = team;
        c[0x10C] = hired;
        *(float*)(c + 0x110) = 1;
        Obj mesh = newObj(SkelMesh, "CharacterMesh0", c);
        *(float*)(mesh + 0x1F8) = 1;
        *(Obj*)(c + 0x80) = mesh;
        return c;
    };
    g_player = character(1, 0, 100, 0, false);
    g_merc = character(2, 300, 80, 0, true);
    g_opp = character(3, 600, 120, 1, false);
    *(Obj*)(pc + 0x80) = g_player;
    g_swordObj = makeActor(Sword, "BP_Longsword_C", 1, 0);
    *(Obj*)(g_swordObj + 0x70) = g_player;
    g_helmObj = makeActor(Helmet, "BP_Armor_Sallet_C", 1, 600);
    *(Obj*)(g_helmObj + 0x70) = g_opp;
    g_mercHelmet = newObj(StaticMeshComp, "Helmet_Bascinet", g_merc);
    setVec(g_mercHelmet, 0x140, 1, 1, 1);
    *(Obj*)(g_mercHelmet + 0x180) = newObj(StaticMesh, "SM_Bascinet", pkg("/Game/Armor/SM_Bascinet"));

    for (int i = 0; i < GUObjectArray.objObjects.numE; i++) {
        Obj o = GUObjectArray.objObjects.objects[i / 65536][i % 65536].obj;
        if (*(Obj*)(o + 0x10) == FunctionClass) g_fnNames[o] = objName(o);
    }
}


// ------------------------------------------------------------------ D3D12 renderer (UE5's default API)

struct D12 {
    ID3D12Device* dev = nullptr;
    ID3D12CommandQueue* q = nullptr;
    IDXGISwapChain3* sc = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    ID3D12Resource* bb[2] = {};
    ID3D12CommandAllocator* alloc[2] = {};
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE ev = nullptr;
    UINT64 fv = 0, frameFence[2] = {};
    UINT inc = 0;

    bool init(HWND hwnd) {
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) return false;
        D3D12_COMMAND_QUEUE_DESC qd{};
        dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
        IDXGIFactory4* fac = nullptr;
        CreateDXGIFactory1(IID_PPV_ARGS(&fac));
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        IDXGISwapChain1* sc1 = nullptr;
        if (FAILED(fac->CreateSwapChainForHwnd(q, hwnd, &sd, nullptr, nullptr, &sc1))) return false;
        sc1->QueryInterface(IID_PPV_ARGS(&sc));
        sc1->Release();
        fac->Release();
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));
        inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for (UINT i = 0; i < 2; i++) {
            sc->GetBuffer(i, IID_PPV_ARGS(&bb[i]));
            D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += i * inc;
            dev->CreateRenderTargetView(bb[i], nullptr, h);
            dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc[i]));
        }
        dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0], nullptr, IID_PPV_ARGS(&list));
        list->Close();
        dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return true;
    }
    void wait(UINT64 v) {
        if (fence->GetCompletedValue() < v) { fence->SetEventOnCompletion(v, ev); WaitForSingleObject(ev, 2000); }
    }
    static void barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER x{};
        x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        x.Transition.pResource = r;
        x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        x.Transition.StateBefore = a;
        x.Transition.StateAfter = b;
        l->ResourceBarrier(1, &x);
    }
    void frame() {
        UINT i = sc->GetCurrentBackBufferIndex();
        wait(frameFence[i]);
        alloc[i]->Reset();
        list->Reset(alloc[i], nullptr);
        barrier(list, bb[i], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += i * inc;
        float c[4] = {0.16f, 0.10f, 0.12f, 1};
        list->ClearRenderTargetView(h, c, 0, nullptr);
        barrier(list, bb[i], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        list->Close();
        ID3D12CommandList* ls[] = {list};
        q->ExecuteCommandLists(1, ls);
        q->Signal(fence, ++fv);
        frameFence[i] = fv;
        sc->Present(1, 0);
    }
    // Copies the buffer that was just presented (overlay included) into a BMP.
    void grab(UINT i, const char* path) {
        wait(fv);
        D3D12_RESOURCE_DESC rd = bb[i]->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
        UINT64 total = 0;
        dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total);
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* rb = nullptr;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)))) return;
        alloc[i]->Reset();
        list->Reset(alloc[i], nullptr);
        barrier(list, bb[i], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}, src{bb[i], D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        dst.PlacedFootprint = fp;
        src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier(list, bb[i], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        list->Close();
        ID3D12CommandList* ls[] = {list};
        q->ExecuteCommandLists(1, ls);
        q->Signal(fence, ++fv);
        wait(fv);
        uint8_t* data = nullptr;
        rb->Map(0, nullptr, (void**)&data);
        int w = (int)rd.Width, h = (int)rd.Height, row = w * 4;
        FILE* f = fopen(path, "wb");
        if (f && data) {
            BITMAPFILEHEADER fh{0x4D42, (DWORD)(54 + row * h), 0, 0, 54};
            BITMAPINFOHEADER ih{sizeof ih, w, -h, 1, 32, BI_RGB, (DWORD)(row * h), 0, 0, 0, 0};
            fwrite(&fh, sizeof fh, 1, f);
            fwrite(&ih, sizeof ih, 1, f);
            std::vector<uint8_t> line(row);
            for (int y = 0; y < h; y++) {
                memcpy(line.data(), data + fp.Offset + y * fp.Footprint.RowPitch, row);
                for (int x = 0; x < w; x++) std::swap(line[x * 4], line[x * 4 + 2]);
                fwrite(line.data(), 1, row, f);
            }
            printf("d3d12 screenshot written\n");
        }
        if (f) fclose(f);
        rb->Unmap(0, nullptr);
        rb->Release();
    }
};

// ------------------------------------------------------------------ main loop

static double envNum(const char* n, double d) {
    char b[64];
    return GetEnvironmentVariableA(n, b, sizeof b) ? atof(b) : d;
}

static void report() {
    auto vec = [](Obj o, int off) { return ((double*)(o + off))[0]; };
    FILE* f = fopen("fake_report.txt", "w");
    FILE* outs[] = {stdout, f};
    for (FILE* o : outs) {
        if (!o) continue;
        fprintf(o, "player_scale=%.3f\nmerc_scale=%.3f\nopp_scale=%.3f\n", vec(g_player, 0x1E0), vec(g_merc, 0x1E0), vec(g_opp, 0x1E0));
        fprintf(o, "opp_mass=%.3f\nplayer_mass=%.3f\n", *(float*)(*(Obj*)(g_opp + 0x80) + 0x1F8), *(float*)(*(Obj*)(g_player + 0x80) + 0x1F8));
        fprintf(o, "opp_speed=%.3f\nplayer_speed=%.3f\n", *(float*)(g_opp + 0x68), *(float*)(g_player + 0x68));
        fprintf(o, "gravity=%.1f\nglobal_set=%d\ndilation=%.2f\n", *(float*)(WS + 0x100), WS[0x108] & 1, g_dilation);
        fprintf(o, "gold=%d\nplayer_health=%.1f\nspawned=%d\n", *(int32_t*)(GI + 0x100), *(float*)(g_player + 0x100), g_spawned);
        fprintf(o, "sword_scale=%.3f\nhelmet_hidden=%d\nmerc_helmet_scale=%.3f\nopp_launched=%.0f\n", vec(g_swordObj, 0x1E0),
                (g_helmObj[0x78] & 2) ? 1 : 0, vec(g_mercHelmet, 0x140), *(double*)(g_opp + 0x1A0));
        fflush(o);
    }
    if (f) fclose(f);
}

int main() {
    DWORD h = 0;
    GetFileVersionInfoSizeW(L"kernel32.dll", &h);  // makes the exe import version.dll, like UE does
    build();
    printf("fake engine built: %d objects\n", GUObjectArray.objObjects.numE);
    fflush(stdout);

    double seconds = envNum("HSL_SECONDS", 25), menuAt = envNum("HSL_MENU_AT", -1);
    bool gfx = envNum("HSL_NO_GFX", 0) == 0;
    HWND hwnd = nullptr;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    if (gfx) {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"FakeUE";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        hwnd = CreateWindowExW(0, wc.lpszClassName, L"Fake UE5 game (Half Sword Lab test)", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               100, 100, 1280, 760, nullptr, nullptr, wc.hInstance, nullptr);
    }
    D12 d12;
    bool use12 = gfx && envNum("HSL_D3D12", 0) != 0 && d12.init(hwnd);
    if (gfx && use12) printf("d3d12: ok\n");
    if (gfx && !use12) {
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 2;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hwnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
        printf("d3d11: 0x%lx\n", hr);
        fflush(stdout);
    }

    Obj tick = nullptr;
    for (auto& [o, n] : g_fnNames)
        if (n == "ReceiveTick") tick = o;
    using PE = void (*)(Obj, Obj, void*);
    DWORD start = GetTickCount();
    bool menuSent = false;
    while ((GetTickCount() - start) / 1000.0 < seconds) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        uint8_t parms[64] = {};
        ((PE)g_vtable[0x4C])(g_player, tick, parms);  // the engine ticking blueprints
        double now = (GetTickCount() - start) / 1000.0;
        if (menuAt >= 0 && !menuSent && now > menuAt && hwnd) {
            PostMessageW(hwnd, WM_KEYDOWN, VK_OEM_5, 1);
            menuSent = true;
        }
        static int clickStep = 0;
        char click[64];
        if (menuSent && hwnd && GetEnvironmentVariableA("HSL_CLICK", click, sizeof click)) {
            int x = atoi(click), y = atoi(strchr(click, ',') ? strchr(click, ',') + 1 : "0");
            LPARAM pos = MAKELPARAM(x, y);
            if (clickStep == 0 && now > menuAt + 0.25) { PostMessageW(hwnd, WM_MOUSEMOVE, 0, pos); clickStep++; }
            else if (clickStep == 1 && now > menuAt + 0.35) { PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, pos); clickStep++; }
            else if (clickStep == 2 && now > menuAt + 0.45) { PostMessageW(hwnd, WM_LBUTTONUP, 0, pos); clickStep++; }
            else if (clickStep == 3 && now > menuAt + 1.5) { PostMessageW(hwnd, WM_KEYDOWN, 'Y', 1); clickStep++; }
            else if (clickStep == 4 && now > menuAt + 1.6) { PostMessageW(hwnd, WM_KEYUP, 'Y', 0xC0000001); clickStep++; }
        }
        if (use12) {
            d12.frame();
            static int menuFrames = 0;
            char shot[MAX_PATH];
            if (menuSent && ++menuFrames == 45 && GetEnvironmentVariableA("HSL_SHOT12", shot, sizeof shot))
                d12.grab((d12.sc->GetCurrentBackBufferIndex() + 1) % 2, shot);
        } else if (sc) {
            if (!rtv) {
                ID3D11Texture2D* bb;
                sc->GetBuffer(0, IID_PPV_ARGS(&bb));
                dev->CreateRenderTargetView(bb, nullptr, &rtv);
                bb->Release();
            }
            float c[4] = {0.10f, 0.12f, 0.16f, 1};
            ctx->OMSetRenderTargets(1, &rtv, nullptr);
            ctx->ClearRenderTargetView(rtv, c);
            sc->Present(1, 0);
        } else {
            Sleep(16);
        }
    }
    report();
    return 0;
}
