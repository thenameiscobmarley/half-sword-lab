// Minimal Unreal Engine 5 reflection, found at runtime.
//
// Nothing here is a hardcoded address or a per-game offset table:
//  * GUObjectArray and the FName pool are found by their *shape* in the exe's data sections.
//  * UStruct / FProperty / UFunction field offsets are calibrated against engine classes that
//    every UE game has (Actor, Pawn, SceneComponent, K2_DestroyActor ...).
//  * ProcessEvent is found in the UObject vtable by the flag tests it always performs.
// That is what lets the Lab survive game updates.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "common.hpp"

namespace ue {

using Obj = uint8_t*;

struct Layout {
    int itemSize = 0x18;   // FUObjectItem
    int super = -1;        // UStruct::SuperStruct
    int children = -1;     // UStruct::Children (UField*, functions)
    int childProps = -1;   // UStruct::ChildProperties (FField*)
    int ffNext = -1;       // FField::Next
    int ffName = -1;       // FField::NamePrivate
    int propBase = -1;     // FProperty::ArrayDim
    int propOffset = -1;   // FProperty::Offset_Internal
    int propSize = -1;     // sizeof(FProperty): subclass fields start here
    int funcFlags = -1;    // UFunction::FunctionFlags
    int peIndex = -1;      // UObject vtable index of ProcessEvent
};
extern Layout L;
extern bool g_lwc;  // engine uses double-precision vectors (UE5 large world coordinates)

bool init(std::string& why);  // false = engine not ready yet (or not a UE game)
void* processEventTarget();   // address to hook
void setProcessEventOriginal(void* fn);

// ---- objects
int32_t count();
Obj at(int32_t i);
bool alive(Obj o);
std::string nameOf(int32_t idx, int32_t num);
std::string name(Obj o);
Obj classOf(Obj o);
Obj outerOf(Obj o);
std::string pathName(Obj o);
std::string fullName(Obj o);
bool isCDO(Obj o);
Obj superOf(Obj s);
bool isClassA(Obj klass, Obj base);
bool isA(Obj o, Obj klass);
Obj findClass(const std::string& shortName);
Obj findByPath(const std::string& path);
template <class F>
void each(F f) {
    int32_t n = count();
    for (int32_t i = 0; i < n; i++)
        if (Obj o = at(i)) f(o);
}

// ---- properties
struct Prop {
    uint8_t* p = nullptr;
    std::string name, type;
    int32_t offset = 0, size = 0, dim = 1;
    uint64_t flags = 0;
    explicit operator bool() const { return p != nullptr; }
};
enum class Kind { None, Float, Int, Bool, Object, WeakObject, Struct, Str, Name, Text, Array, Other };

Prop describe(uint8_t* field);
Prop findProp(Obj st, const std::string& name);
void forProps(Obj st, const std::function<bool(const Prop&)>& f, bool supers = true);
Kind kindOf(const Prop& p);
Obj propStruct(const Prop& p);  // StructProperty -> struct, Object/Class -> class
Prop arrayInner(const Prop& p);

bool readNumber(const uint8_t* base, const Prop& p, double& out);
bool writeNumber(uint8_t* base, const Prop& p, double v);
bool readBool(const uint8_t* base, const Prop& p);
void writeBool(uint8_t* base, const Prop& p, bool v);
Obj readObject(const uint8_t* base, const Prop& p);
std::string readString(const uint8_t* base, const Prop& p);

Obj getObj(Obj o, const char* prop);
bool getNum(Obj o, const char* prop, double& out);
bool setNum(Obj o, const char* prop, double v);
bool setBool(Obj o, const char* prop, bool v);
std::vector<Obj> getObjArray(Obj o, const char* prop);

// ---- functions
Obj findFunc(Obj klass, const std::string& name);
void processEvent(Obj obj, Obj fn, void* parms);

// Builds a parameter block by name ("NewScale3D.X", "SpawnTransform.Translation.Z") and calls it.
class Call {
public:
    Call(Obj target, const char* func);
    Call(Obj target, Obj fn);
    explicit operator bool() const { return fn_ != nullptr && target_ != nullptr; }
    bool has(const std::string& path);
    Call& num(const std::string& path, double v);
    Call& flag(const std::string& path, bool v);
    Call& obj(const std::string& path, Obj v);
    Call& vec(const std::string& path, double x, double y, double z);
    bool run();
    double retNum(const std::string& path, double def = 0);
    Obj retObj(const std::string& path = "ReturnValue");

private:
    bool resolve(const std::string& path, Prop& leaf, int& off);
    Obj target_ = nullptr, fn_ = nullptr;
    alignas(16) uint8_t buf_[2048];
};

}  // namespace ue
