#include "ue.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_map>

using namespace lab;

namespace ue {

Layout L;
bool g_lwc = true;

// FUObjectItem flags / UObject flags we care about.
constexpr uint32_t RF_ClassDefaultObject = 0x10, RF_ArchetypeObject = 0x20;
constexpr uint32_t RF_BeginDestroyed = 0x8000, RF_FinishDestroyed = 0x10000;
constexpr uint32_t EIF_Garbage = 1u << 21;
constexpr int O_FLAGS = 0x8, O_INDEX = 0xC, O_CLASS = 0x10, O_NAME = 0x18, O_OUTER = 0x20, UFIELD_NEXT = 0x28;
constexpr int CHUNK = 64 * 1024;

static uint8_t** g_blocks = nullptr;  // FNamePool blocks
static uint8_t** g_chunks = nullptr;  // FChunkedFixedUObjectArray::Objects (chunk pointers)
static int32_t* g_num = nullptr;      // FChunkedFixedUObjectArray::NumElements
static void* g_peOriginal = nullptr;
static void* g_peTarget = nullptr;

struct Range { uint8_t* lo; uint8_t* hi; };
static std::vector<Range> g_dataRanges;
static Range g_text{};

// ------------------------------------------------------------------ module layout

static void readSections() {
    g_dataRanges.clear();
    auto base = (uint8_t*)GetModuleHandleW(nullptr);
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        Range r{base + sec->VirtualAddress, base + sec->VirtualAddress + sec->Misc.VirtualSize};
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            if (!g_text.lo || (r.hi - r.lo) > (g_text.hi - g_text.lo)) g_text = r;
        } else if (sec->Characteristics & IMAGE_SCN_MEM_WRITE) {
            g_dataRanges.push_back(r);
        }
    }
}

static bool inText(const void* p) { return p >= g_text.lo && p < g_text.hi; }

// ------------------------------------------------------------------ names

static bool looksLikeEntry(const uint8_t* e, const char* s) {
    size_t n = strlen(s);
    if (!readable(e, 2 + n)) return false;
    uint16_t h = *(const uint16_t*)e;
    return !(h & 1) && (h >> 6) == n && memcmp(e + 2, s, n) == 0;
}

static bool findNamePool() {
    for (auto& r : g_dataRanges) {
        for (uint8_t* a = r.lo; a + 8 <= r.hi; a += 8) {
            uint8_t* blk = *(uint8_t**)a;
            if (!blk || ((uintptr_t)blk & 1)) continue;
            if (!looksLikeEntry(blk, "None")) continue;
            if (!looksLikeEntry(blk + 6, "ByteProperty")) continue;
            g_blocks = (uint8_t**)a;
            log("FName pool blocks at exe+0x%llx", (unsigned long long)(a - (uint8_t*)GetModuleHandleW(nullptr)));
            return true;
        }
    }
    return false;
}

static std::unordered_map<uint64_t, std::string> g_nameCache;

std::string nameOf(int32_t idx, int32_t num) {
    if (!g_blocks || idx < 0) return {};
    uint64_t key = ((uint64_t)(uint32_t)idx << 32) | (uint32_t)num;
    auto it = g_nameCache.find(key);
    if (it != g_nameCache.end()) return it->second;
    uint32_t block = (uint32_t)idx >> 16, offset = ((uint32_t)idx & 0xFFFF) * 2;
    if (block >= 8192) return {};
    uint8_t* b = rd<uint8_t*>(&g_blocks[block]);
    if (!b || !readable(b + offset, 2)) return {};
    uint16_t h = *(uint16_t*)(b + offset);
    int len = h >> 6;
    std::string s;
    if (h & 1) {
        if (!readable(b + offset + 2, len * 2)) return {};
        s = narrow(std::wstring((wchar_t*)(b + offset + 2), len));
    } else {
        if (!readable(b + offset + 2, len)) return {};
        s.assign((char*)(b + offset + 2), len);
    }
    if (num > 0) s += "_" + std::to_string(num - 1);
    if (g_nameCache.size() < 500000) g_nameCache[key] = s;
    return s;
}

// ------------------------------------------------------------------ object array

static bool findObjectArray() {
    for (auto& r : g_dataRanges) {
        for (uint8_t* a = r.lo; a + 32 <= r.hi; a += 8) {
            int32_t maxE = *(int32_t*)(a + 16), numE = *(int32_t*)(a + 20);
            int32_t maxC = *(int32_t*)(a + 24), numC = *(int32_t*)(a + 28);
            if (maxE < 0x10000 || maxE > 0x10000000 || numE < 0x400 || numE > maxE) continue;
            if (maxC != (maxE + CHUNK - 1) / CHUNK) continue;
            if (numC < (numE + CHUNK - 1) / CHUNK || numC > maxC) continue;
            auto chunks = *(uint8_t***)a;
            if (!readable(chunks, 8 * numC) || !readable(chunks[0], 64)) continue;
            for (int size : {0x18, 0x10, 0x20, 0x28}) {
                if (!readable(chunks[0], size * 256)) continue;
                int good = 0;
                for (int i = 1; i < 128; i++) {
                    Obj o = *(Obj*)(chunks[0] + i * size);
                    if (o && readable(o, 0x28) && *(int32_t*)(o + O_INDEX) == i) good++;
                }
                if (good >= 100) {
                    g_chunks = chunks;
                    g_num = (int32_t*)(a + 20);
                    L.itemSize = size;
                    log("GUObjectArray at exe+0x%llx, %d objects, item size 0x%x",
                        (unsigned long long)(a - (uint8_t*)GetModuleHandleW(nullptr)), numE, size);
                    return true;
                }
            }
        }
    }
    return false;
}

int32_t count() { return g_num ? *g_num : 0; }

static uint8_t* item(int32_t i) {
    if (!g_chunks || i < 0 || i >= *g_num) return nullptr;
    uint8_t* chunk = g_chunks[i / CHUNK];
    return chunk ? chunk + (i % CHUNK) * L.itemSize : nullptr;
}

Obj at(int32_t i) {
    uint8_t* it = item(i);
    if (!it) return nullptr;
    Obj o = *(Obj*)it;
    if (!o) return nullptr;
    if (*(uint32_t*)(it + 8) & EIF_Garbage) return nullptr;
    if (*(uint32_t*)(o + O_FLAGS) & (RF_BeginDestroyed | RF_FinishDestroyed)) return nullptr;
    return o;
}

bool alive(Obj o) {
    if (!o || !readable(o, 0x28)) return false;
    return at(*(int32_t*)(o + O_INDEX)) == o;
}

std::string name(Obj o) {
    if (!readable(o + O_NAME, 8)) return {};
    return nameOf(*(int32_t*)(o + O_NAME), *(int32_t*)(o + O_NAME + 4));
}
Obj classOf(Obj o) { return rd<Obj>(o + O_CLASS); }
Obj outerOf(Obj o) { return rd<Obj>(o + O_OUTER); }

std::string pathName(Obj o) {
    std::vector<Obj> chain;
    for (Obj p = o; p && chain.size() < 32; p = outerOf(p)) chain.push_back(p);
    // chain is innermost-first; depth 0 is the package.
    std::string s;
    for (size_t depth = 0; depth < chain.size(); depth++) {
        if (depth == 1) s += ".";
        else if (depth == 2) s += ":";
        else if (depth > 2) s += ".";
        s += name(chain[chain.size() - 1 - depth]);
    }
    return s;
}

std::string fullName(Obj o) { return name(classOf(o)) + " " + pathName(o); }
bool isCDO(Obj o) { return rd<uint32_t>(o + O_FLAGS) & (RF_ClassDefaultObject | RF_ArchetypeObject); }
Obj superOf(Obj s) { return L.super > 0 ? rd<Obj>(s + L.super) : nullptr; }

bool isClassA(Obj k, Obj base) {
    for (int d = 0; k && d < 64; d++, k = superOf(k))
        if (k == base) return true;
    return false;
}
bool isA(Obj o, Obj klass) { return o && klass && isClassA(classOf(o), klass); }

static Obj findNative(const char* pkg, const char* nm) {
    Obj found = nullptr;
    each([&](Obj o) {
        if (!found && name(o) == nm && name(outerOf(o)) == pkg) found = o;
    });
    return found;
}

static std::unordered_map<std::string, Obj> g_classCache;
static Obj g_classClass = nullptr;

Obj findClass(const std::string& shortName) {
    auto it = g_classCache.find(shortName);
    if (it != g_classCache.end() && alive(it->second)) return it->second;
    Obj best = nullptr;
    each([&](Obj o) {
        if (isCDO(o) || name(o) != shortName) return;
        if (!isA(o, g_classClass)) return;
        // Prefer engine (/Script/...) classes when a blueprint shares the name.
        if (!best || pathName(o).rfind("/Script/", 0) == 0) best = o;
    });
    if (best) g_classCache[shortName] = best;
    return best;
}

Obj findByPath(const std::string& path) {
    Obj found = nullptr;
    each([&](Obj o) {
        if (!found && !isCDO(o) && pathName(o) == path) found = o;
    });
    return found;
}

// ------------------------------------------------------------------ properties

static std::string fieldName(uint8_t* f) {
    if (!readable(f + L.ffName, 8)) return {};
    return nameOf(*(int32_t*)(f + L.ffName), *(int32_t*)(f + L.ffName + 4));
}

Prop describe(uint8_t* f) {
    Prop p;
    if (!f || !readable(f, L.propSize)) return p;
    p.p = f;
    p.name = fieldName(f);
    uint8_t* fclass = rd<uint8_t*>(f + 8);
    if (fclass && readable(fclass, 8)) p.type = nameOf(*(int32_t*)fclass, *(int32_t*)(fclass + 4));
    p.dim = *(int32_t*)(f + L.propBase);
    p.size = *(int32_t*)(f + L.propBase + 4);
    p.flags = *(uint64_t*)(f + L.propBase + 8);
    p.offset = *(int32_t*)(f + L.propOffset);
    return p;
}

void forProps(Obj st, const std::function<bool(const Prop&)>& fn, bool supers) {
    for (int d = 0; st && d < 32; d++) {
        uint8_t* f = rd<uint8_t*>(st + L.childProps);
        for (int k = 0; f && k < 4096; k++) {
            Prop p = describe(f);
            if (!p) break;
            if (!fn(p)) return;
            f = rd<uint8_t*>(f + L.ffNext);
        }
        if (!supers) return;
        st = superOf(st);
    }
}

Prop findProp(Obj st, const std::string& nm) {
    Prop out;
    forProps(st, [&](const Prop& p) {
        if (p.name != nm) return true;
        out = p;
        return false;
    });
    return out;
}

Kind kindOf(const Prop& p) {
    const std::string& t = p.type;
    if (t == "FloatProperty" || t == "DoubleProperty") return Kind::Float;
    if (t == "IntProperty" || t == "Int64Property" || t == "Int16Property" || t == "Int8Property" ||
        t == "UInt16Property" || t == "UInt32Property" || t == "UInt64Property" || t == "ByteProperty" ||
        t == "EnumProperty")
        return Kind::Int;
    if (t == "BoolProperty") return Kind::Bool;
    if (t == "ObjectProperty" || t == "ObjectPtrProperty" || t == "ClassProperty" || t == "ClassPtrProperty")
        return Kind::Object;
    if (t == "WeakObjectProperty") return Kind::WeakObject;
    if (t == "StructProperty") return Kind::Struct;
    if (t == "StrProperty") return Kind::Str;
    if (t == "NameProperty") return Kind::Name;
    if (t == "TextProperty") return Kind::Text;
    if (t == "ArrayProperty") return Kind::Array;
    return Kind::Other;
}

Obj propStruct(const Prop& p) { return rd<Obj>(p.p + L.propSize); }
Prop arrayInner(const Prop& p) { return describe(rd<uint8_t*>(p.p + L.propSize)); }

bool readNumber(const uint8_t* base, const Prop& p, double& out) {
    const uint8_t* a = base + p.offset;
    if (!readable(a, p.size > 0 ? p.size : 1)) return false;
    const std::string& t = p.type;
    if (t == "FloatProperty") out = *(const float*)a;
    else if (t == "DoubleProperty") out = *(const double*)a;
    else if (t == "IntProperty") out = *(const int32_t*)a;
    else if (t == "Int64Property") out = (double)*(const int64_t*)a;
    else if (t == "Int16Property") out = *(const int16_t*)a;
    else if (t == "Int8Property") out = *(const int8_t*)a;
    else if (t == "UInt16Property") out = *(const uint16_t*)a;
    else if (t == "UInt32Property") out = *(const uint32_t*)a;
    else if (t == "UInt64Property") out = (double)*(const uint64_t*)a;
    else if (t == "ByteProperty") out = *a;
    else if (t == "EnumProperty") {
        if (p.size == 1) out = *a;
        else if (p.size == 2) out = *(const int16_t*)a;
        else if (p.size == 4) out = *(const int32_t*)a;
        else out = (double)*(const int64_t*)a;
    } else return false;
    return true;
}

bool writeNumber(uint8_t* base, const Prop& p, double v) {
    uint8_t* a = base + p.offset;
    if (!readable(a, p.size > 0 ? p.size : 1)) return false;
    const std::string& t = p.type;
    if (t == "FloatProperty") *(float*)a = (float)v;
    else if (t == "DoubleProperty") *(double*)a = v;
    else if (t == "IntProperty") *(int32_t*)a = (int32_t)v;
    else if (t == "Int64Property") *(int64_t*)a = (int64_t)v;
    else if (t == "Int16Property") *(int16_t*)a = (int16_t)v;
    else if (t == "Int8Property") *(int8_t*)a = (int8_t)v;
    else if (t == "UInt16Property") *(uint16_t*)a = (uint16_t)v;
    else if (t == "UInt32Property") *(uint32_t*)a = (uint32_t)v;
    else if (t == "UInt64Property") *(uint64_t*)a = (uint64_t)v;
    else if (t == "ByteProperty") *a = (uint8_t)v;
    else if (t == "EnumProperty") {
        if (p.size == 1) *a = (uint8_t)v;
        else if (p.size == 2) *(int16_t*)a = (int16_t)v;
        else if (p.size == 4) *(int32_t*)a = (int32_t)v;
        else *(int64_t*)a = (int64_t)v;
    } else return false;
    return true;
}

// FBoolProperty: FieldSize, ByteOffset, ByteMask, FieldMask right after FProperty.
bool readBool(const uint8_t* base, const Prop& p) {
    const uint8_t* b = p.p + L.propSize;
    uint8_t byteOffset = b[1], fieldMask = b[3];
    return (rd<uint8_t>(base + p.offset + byteOffset) & fieldMask) != 0;
}

void writeBool(uint8_t* base, const Prop& p, bool v) {
    const uint8_t* b = p.p + L.propSize;
    uint8_t byteOffset = b[1], fieldMask = b[3];
    uint8_t* a = base + p.offset + byteOffset;
    if (!readable(a, 1)) return;
    *a = v ? (*a | fieldMask) : (*a & ~fieldMask);
}

Obj readObject(const uint8_t* base, const Prop& p) {
    Kind k = kindOf(p);
    if (k == Kind::Object) return rd<Obj>(base + p.offset);
    if (k == Kind::WeakObject) {
        int32_t idx = rd<int32_t>(base + p.offset, -1);
        return idx > 0 ? at(idx) : nullptr;
    }
    return nullptr;
}

std::string readString(const uint8_t* base, const Prop& p) {
    Kind k = kindOf(p);
    if (k == Kind::Name) return nameOf(rd<int32_t>(base + p.offset), rd<int32_t>(base + p.offset + 4));
    if (k == Kind::Str) {
        wchar_t* data = rd<wchar_t*>(base + p.offset);
        int32_t n = rd<int32_t>(base + p.offset + 8);
        if (data && n > 0 && n < 4096 && readable(data, n * 2)) return narrow(std::wstring(data, n - 1));
    }
    return {};
}

Obj getObj(Obj o, const char* prop) {
    if (!o) return nullptr;
    Prop p = findProp(classOf(o), prop);
    return p ? readObject(o, p) : nullptr;
}

bool getNum(Obj o, const char* prop, double& out) {
    if (!o) return false;
    Prop p = findProp(classOf(o), prop);
    return p && readNumber(o, p, out);
}

bool setNum(Obj o, const char* prop, double v) {
    if (!o) return false;
    Prop p = findProp(classOf(o), prop);
    return p && writeNumber(o, p, v);
}

bool setBool(Obj o, const char* prop, bool v) {
    if (!o) return false;
    Prop p = findProp(classOf(o), prop);
    if (!p || kindOf(p) != Kind::Bool) return false;
    writeBool(o, p, v);
    return true;
}

std::vector<Obj> getObjArray(Obj o, const char* prop) {
    std::vector<Obj> out;
    if (!o) return out;
    Prop p = findProp(classOf(o), prop);
    if (!p || kindOf(p) != Kind::Array) return out;
    Prop inner = arrayInner(p);
    if (kindOf(inner) != Kind::Object) return out;
    Obj* data = rd<Obj*>(o + p.offset);
    int32_t n = rd<int32_t>(o + p.offset + 8);
    if (!data || n <= 0 || n > 100000 || !readable(data, n * sizeof(Obj))) return out;
    out.assign(data, data + n);
    return out;
}

// ------------------------------------------------------------------ functions

static std::unordered_map<std::string, Obj> g_funcCache;

Obj findFunc(Obj klass, const std::string& nm) {
    std::string key = std::to_string((uintptr_t)klass) + ":" + nm;
    auto it = g_funcCache.find(key);
    if (it != g_funcCache.end()) return it->second;
    Obj found = nullptr;
    for (Obj s = klass; s && !found; s = superOf(s)) {
        Obj f = rd<Obj>(s + L.children);
        for (int k = 0; f && k < 8192; k++, f = rd<Obj>(f + UFIELD_NEXT))
            if (name(f) == nm) { found = f; break; }
    }
    g_funcCache[key] = found;
    return found;
}

using PEFn = void (*)(Obj, Obj, void*);

void processEvent(Obj obj, Obj fn, void* parms) {
    PEFn pe = (PEFn)g_peOriginal;
    if (!pe) {
        void** vt = rd<void**>(obj);
        pe = (PEFn)rd<void*>(vt + L.peIndex);
    }
    if (pe) pe(obj, fn, parms);
}

void* processEventTarget() { return g_peTarget; }
void setProcessEventOriginal(void* fn) { g_peOriginal = fn; }

Call::Call(Obj target, const char* func) : target_(target) {
    memset(buf_, 0, sizeof buf_);
    if (target && L.peIndex >= 0) fn_ = findFunc(classOf(target), func);
}

Call::Call(Obj target, Obj fn) : target_(target), fn_(fn) { memset(buf_, 0, sizeof buf_); }

bool Call::resolve(const std::string& path, Prop& leaf, int& off) {
    if (!fn_) return false;
    size_t start = 0;
    Obj scope = fn_;
    bool first = true;
    off = 0;
    while (start <= path.size()) {
        size_t dot = path.find('.', start);
        std::string part = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        leaf = first ? findProp(scope, part) : findProp(scope, part);
        if (!leaf) return false;
        off += leaf.offset;
        first = false;
        if (dot == std::string::npos) break;
        if (kindOf(leaf) != Kind::Struct) return false;
        scope = propStruct(leaf);
        start = dot + 1;
    }
    return off + (leaf.size > 0 ? leaf.size : 1) <= (int)sizeof buf_;
}

bool Call::has(const std::string& path) {
    Prop p;
    int off;
    return resolve(path, p, off);
}

Call& Call::num(const std::string& path, double v) {
    Prop p;
    int off;
    if (resolve(path, p, off)) {
        Prop at = p;
        at.offset = 0;
        writeNumber(buf_ + off, at, v);
    }
    return *this;
}

Call& Call::flag(const std::string& path, bool v) {
    Prop p;
    int off;
    if (resolve(path, p, off) && kindOf(p) == Kind::Bool) {
        Prop at = p;
        at.offset = 0;
        writeBool(buf_ + off, at, v);
    }
    return *this;
}

Call& Call::obj(const std::string& path, Obj v) {
    Prop p;
    int off;
    if (resolve(path, p, off) && kindOf(p) == Kind::Object) *(Obj*)(buf_ + off) = v;
    return *this;
}

Call& Call::vec(const std::string& path, double x, double y, double z) {
    return num(path + ".X", x).num(path + ".Y", y).num(path + ".Z", z);
}

bool Call::run() {
    if (!*this || !alive(target_)) return false;
    processEvent(target_, fn_, buf_);
    return true;
}

double Call::retNum(const std::string& path, double def) {
    Prop p;
    int off;
    double v = def;
    if (resolve(path, p, off)) {
        Prop at = p;
        at.offset = 0;
        readNumber(buf_ + off, at, v);
    }
    return v;
}

Obj Call::retObj(const std::string& path) {
    Prop p;
    int off;
    return resolve(path, p, off) && kindOf(p) == Kind::Object ? *(Obj*)(buf_ + off) : nullptr;
}

// ------------------------------------------------------------------ calibration

static bool walkFields(uint8_t* f, int nextOff, int nameOff, std::initializer_list<const char*> want) {
    int found = 0;
    for (int k = 0; f && k < 512 && readable(f, nameOff + 8); k++) {
        std::string n = nameOf(*(int32_t*)(f + nameOff), *(int32_t*)(f + nameOff + 4));
        for (const char* w : want)
            if (n == w) found++;
        f = rd<uint8_t*>(f + nextOff);
    }
    return found == (int)want.size();
}

// `test dword ptr [reg+disp32], imm32` or `test byte ptr [reg+disp32], imm8` on FunctionFlags.
static bool testsFlag(const uint8_t* code, size_t len, int flagsOff, uint32_t flag) {
    int shift = 0;
    while (shift < 4 && !((flag >> (shift * 8)) & 0xFF)) shift++;
    uint8_t byteVal = (flag >> (shift * 8)) & 0xFF;
    for (size_t i = 0; i + 10 <= len; i++) {
        uint8_t op = code[i], modrm = code[i + 1];
        if ((modrm & 0xF8) != 0x80 || (modrm & 7) == 4) continue;  // [reg+disp32], no SIB
        int32_t disp = *(const int32_t*)(code + i + 2);
        if (op == 0xF7 && disp == flagsOff && *(const uint32_t*)(code + i + 6) == flag) return true;
        if (op == 0xF6 && disp == flagsOff + shift && code[i + 6] == byteVal) return true;
    }
    return false;
}

// Length of the function starting at fn, from the exe's .pdata unwind table, so the flag scan
// can't run into the next function. Leaf functions have no entry: stop at their first ret.
static size_t functionLength(const uint8_t* fn) {
    auto base = (uint8_t*)GetModuleHandleW(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    auto table = (RUNTIME_FUNCTION*)(base + dir.VirtualAddress);
    size_t n = dir.Size / sizeof(RUNTIME_FUNCTION);
    DWORD rva = (DWORD)(fn - base);
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (table[mid].EndAddress <= rva) lo = mid + 1;
        else hi = mid;
    }
    if (lo < n && table[lo].BeginAddress == rva) return std::min<size_t>(table[lo].EndAddress - rva, 0x2000);
    for (size_t i = 0; i < 0x400; i++)
        if (fn[i] == 0xC3) return i + 1;
    return 0x400;
}

static int findProcessEventIndex(Obj anyObject) {
    void** vt = rd<void**>(anyObject);
    for (int i = 0; i < 0x200 && readable(vt + i, 8); i++) {
        uint8_t* fn = (uint8_t*)vt[i];
        if (!inText(fn)) break;
        if (fn[0] == 0xE9) fn = fn + 5 + *(int32_t*)(fn + 1);  // follow a jump thunk
        if (!inText(fn) || !readable(fn, 16)) continue;
        size_t len = functionLength(fn);
        if (!readable(fn, len)) continue;
        if (testsFlag(fn, len, L.funcFlags, 0x400) && testsFlag(fn, len, L.funcFlags, 0x400000)) {
            g_peTarget = fn;
            return i;
        }
    }
    return -1;
}

static bool calibrate(std::string& why) {
    Obj object = findNative("/Script/CoreUObject", "Object");
    g_classClass = findNative("/Script/CoreUObject", "Class");
    Obj actor = findNative("/Script/Engine", "Actor");
    Obj pawn = findNative("/Script/Engine", "Pawn");
    Obj scene = findNative("/Script/Engine", "SceneComponent");
    if (!object || !g_classClass || !actor || !pawn || !scene) {
        why = "engine classes not loaded yet";
        return false;
    }

    for (int off = 0x30; off <= 0x80 && L.super < 0; off += 8)
        if (rd<Obj>(actor + off) == object && rd<Obj>(pawn + off) == actor) L.super = off;
    if (L.super < 0) { why = "SuperStruct offset not found"; return false; }

    for (int off = L.super + 8; off <= L.super + 0x40 && L.children < 0; off += 8) {
        Obj f = rd<Obj>(actor + off);
        for (int k = 0; f && k < 4096 && readable(f, 0x30); k++, f = rd<Obj>(f + UFIELD_NEXT))
            if (name(f) == "K2_DestroyActor") { L.children = off; break; }
    }
    if (L.children < 0) { why = "UStruct::Children offset not found"; return false; }

    for (int off = L.super + 8; off <= L.super + 0x48 && L.childProps < 0; off += 8) {
        if (off == L.children) continue;
        uint8_t* first = rd<uint8_t*>(actor + off);
        for (int nameOff : {0x28, 0x20}) {
            for (int nextOff : {0x20, 0x18}) {
                if (nextOff >= nameOff) continue;
                if (walkFields(first, nextOff, nameOff, {"PrimaryActorTick", "RootComponent"})) {
                    L.childProps = off;
                    L.ffName = nameOff;
                    L.ffNext = nextOff;
                    break;
                }
            }
            if (L.childProps >= 0) break;
        }
    }
    if (L.childProps < 0) { why = "ChildProperties layout not found"; return false; }

    // FProperty fields start right after FField (name + flags + padding).
    L.propBase = L.ffName + 0x10;
    L.propSize = 0x200;  // generous until calibrated, so describe() can read
    uint8_t *tick = nullptr, *root = nullptr;
    for (uint8_t* f = rd<uint8_t*>(actor + L.childProps); f && readable(f, 0x80); f = rd<uint8_t*>(f + L.ffNext)) {
        std::string n = fieldName(f);
        if (n == "PrimaryActorTick") tick = f;
        if (n == "RootComponent") root = f;
    }
    for (int off = L.propBase + 0x8; off <= L.propBase + 0x30 && L.propOffset < 0; off += 4) {
        int32_t a = rd<int32_t>(tick + off), b = rd<int32_t>(root + off);
        if (a == 0x28 && b > 0x28 && b < 0x4000 && b % 8 == 0) L.propOffset = off;
    }
    if (L.propOffset < 0) { why = "Offset_Internal not found"; return false; }
    L.propSize = -1;
    for (int off = L.propBase + 0x18; off <= L.propBase + 0x90 && L.propSize < 0; off += 8)
        if (rd<Obj>(root + off) == scene) L.propSize = off;
    if (L.propSize < 0) { why = "sizeof(FProperty) not found"; return false; }

    Obj destroy = findFunc(actor, "K2_DestroyActor");
    Obj getScale = findFunc(actor, "GetActorScale3D");
    g_funcCache.clear();
    for (int off = 0x80; off <= 0x120 && L.funcFlags < 0; off += 4) {
        uint32_t a = rd<uint32_t>(destroy + off), b = rd<uint32_t>(getScale + off);
        auto callable = [](uint32_t f) { return (f & 0x400) && (f & 0x04000000) && (f & 0x20000); };
        if (callable(a) && callable(b) && (b & 0x10000000) && !(a & 0x10000000)) L.funcFlags = off;
    }
    if (L.funcFlags < 0) { why = "UFunction::FunctionFlags not found"; return false; }

    L.peIndex = findProcessEventIndex(object);
    if (L.peIndex < 0) why = "ProcessEvent not found (stats editing still works, actions disabled)";

    Obj vector = findNative("/Script/CoreUObject", "Vector");
    Prop x = vector ? findProp(vector, "X") : Prop{};
    g_lwc = !x || x.type == "DoubleProperty";

    log("layout: super=0x%x children=0x%x childProps=0x%x ffNext=0x%x ffName=0x%x propBase=0x%x "
        "offset=0x%x propSize=0x%x funcFlags=0x%x processEvent=vtable[0x%x] lwc=%d",
        L.super, L.children, L.childProps, L.ffNext, L.ffName, L.propBase, L.propOffset, L.propSize,
        L.funcFlags, L.peIndex, (int)g_lwc);
    return true;
}

bool init(std::string& why) {
    readSections();
    if (!g_blocks && !findNamePool()) { why = "FName pool not found yet"; return false; }
    if (!g_chunks && !findObjectArray()) { why = "object array not found yet"; return false; }
    if (count() < 20000) { why = "engine still starting"; return false; }
    return calibrate(why);
}

}  // namespace ue
