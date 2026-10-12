#include "overlay.hpp"

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "MinHook.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"
#include "common.hpp"
#include "imgui.h"
#include "lab.hpp"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

using namespace lab;

namespace overlay {

// ------------------------------------------------------------------ state

static unsigned g_toggleKey = VK_OEM_5;  // '\' on US/UK layouts
static std::atomic<bool> g_visible{false};
static bool g_imgui = false;
static HWND g_hwnd = nullptr;
static WNDPROC g_origWndProc = nullptr;
static DWORD g_initTick = 0;
static float g_vx = 0, g_vy = 0;    // virtual cursor (raw input) while the menu is open
static DWORD g_lastRaw = 0;

enum class Api { None, D3D11, D3D12 };
static Api g_api = Api::None;
static IDXGISwapChain* g_sc = nullptr;

// D3D11
static ID3D11Device* g_d11 = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static ID3D11RenderTargetView* g_rtv11 = nullptr;
static bool g_dx11Backend = false;

// D3D12
struct Frame {
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12Resource* rt = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    UINT64 fence = 0;
};
static ID3D12Device* g_d12 = nullptr;
static ID3D12CommandQueue* g_queue = nullptr;
static ID3D12DescriptorHeap *g_rtvHeap = nullptr, *g_srvHeap = nullptr;
static ID3D12GraphicsCommandList* g_list = nullptr;
static ID3D12Fence* g_fence = nullptr;
static HANDLE g_fenceEvent = nullptr;
static UINT64 g_fenceValue = 0;
static std::vector<Frame> g_frames;
static bool g_dx12Backend = false;
static std::vector<int> g_srvFree;
static UINT g_srvInc = 0;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ExecFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
static PresentFn oPresent = nullptr, oPresent11 = nullptr;
static Present1Fn oPresent1 = nullptr;
static ResizeFn oResize = nullptr, oResize11 = nullptr;
static ExecFn oExec = nullptr;

// ------------------------------------------------------------------ menu data

struct ModDef {
    const char* key;
    const char* title;
    const char* desc;
    std::vector<const char*> tags;
    std::vector<std::pair<const char*, const char*>> params;  // cfg key, default
};

static const std::vector<ModDef> MODS = {
    {"random_sizes", "Randomised Sizes Opponents", "Every opponent rolls a random size when it appears.", {"opponents", "chaos"}, {{"random_min", "0.4"}, {"random_max", "2.2"}}},
    {"opp_scale_on", "Opponent Sizes", "All opponents locked to one size.", {"opponents"}, {{"opp_scale", "1.8"}}},
    {"ever_growing", "Ever-Growing Opponents", "Opponents keep growing, up to 3x. Be quick.", {"opponents", "chaos"}, {}},
    {"growth_spurt", "Growth Spurt", "You slowly grow while this is on.", {"player"}, {{"growth_cap", "2.5"}}},
    {"caffeine", "Caffeine Lottery", "Each opponent gets a random speed 0.5x-1.8x.", {"opponents", "chaos"}, {}},
    {"freeze_opp", "Statues", "Opponents slowed almost to a standstill.", {"opponents"}, {}},
    {"hyper_opp", "Espresso Opponents", "Opponents move at 2.5x speed.", {"opponents", "chaos"}, {}},
    {"feather_opp", "Paper Knights", "Opponents weigh almost nothing.", {"opponents", "physics"}, {}},
    {"anvil", "Anvil Mode", "You weigh 4x as much.", {"player", "physics"}, {}},
    {"giant_weapons", "Giant Weapons", "Weapons in your hands get scaled up.", {"weapons", "player"}, {{"weapon_factor", "2.5"}}},
    {"matrix", "Matrix Mode", "World at 30% speed, you at full speed.", {"world", "player"}, {}},
    {"slowmo", "Slow Motion", "Everything in slow motion.", {"world"}, {{"slowmo_value", "0.4"}}},
    {"god_mode", "Stubborn", "Health-type stats can't drop below their best value.", {"player", "experimental"}, {}},
};
static const std::vector<std::vector<const char*>> EXCLUSIVE = {
    {"random_sizes", "opp_scale_on", "ever_growing"}, {"freeze_opp", "hyper_opp", "caffeine"}, {"matrix", "slowmo"}};
static const char* GRAVITY_KEYS[] = {"normal", "moon", "mars", "zero", "heavy", "reverse", "custom"};
static const char* GRAVITY_NAMES[] = {"Normal", "Moon (1/6 g)", "Mars", "Zero-G", "Heavy (3 g)", "Reverse (fall up!)", "Custom"};

static ImVec4 rgb(int r, int g, int b, float a = 1) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a); }
static const ImVec4 VIOLET = rgb(139, 92, 246), CYAN = rgb(34, 211, 238), MUTED = rgb(147, 147, 168), DIM = rgb(102, 102, 122);

static ImVec4 tagColor(const std::string& t) {
    if (t == "player" || t == "coins") return rgb(245, 197, 66);
    if (t == "mercenary" || t == "team") return CYAN;
    if (t == "opponent" || t == "opponents" || t == "health") return rgb(244, 63, 94);
    if (t == "modded" || t == "world") return VIOLET;
    if (t == "chaos") return rgb(232, 121, 249);
    if (t == "weapon" || t == "weapons") return rgb(251, 146, 60);
    if (t == "armor") return rgb(148, 163, 184);
    if (t == "physics") return rgb(167, 139, 250);
    if (t == "speed") return rgb(52, 211, 153);
    if (t == "experimental") return rgb(250, 204, 21);
    return DIM;
}

static std::string groupOf(const std::string& n) {
    static const std::vector<std::pair<const char*, std::vector<const char*>>> groups = {
        {"coins", {"gold", "coin", "money", "cash", "currency", "florin", "denar", "silver"}},
        {"mercenary", {"merc", "hire", "ally", "companion", "follower", "recruit"}},
        {"health", {"health", "hp", "blood", "conscious", "stamina", "energy", "life", "pain", "bleed", "dead"}},
        {"armor", {"armor", "armour", "helm", "plate", "mail", "protection"}},
        {"weapon", {"weapon", "sword", "blade", "damage", "sharp"}},
        {"team", {"team", "faction"}},
        {"speed", {"speed", "walk", "run", "sprint"}},
        {"physics", {"mass", "weight", "scale", "size", "gravity", "strength", "force"}},
    };
    std::string l = lower(n);
    for (auto& [tag, words] : groups)
        for (auto w : words)
            if (l.find(w) != std::string::npos) return tag;
    return "misc";
}

static void tags(const std::vector<std::string>& ts) {
    for (auto& t : ts) {
        ImGui::SameLine();
        ImGui::TextColored(tagColor(t), "#%s", t.c_str());
    }
}

// ------------------------------------------------------------------ "Make Backup Before Applying Changes?"

static std::function<void()> g_pending;
static std::string g_pendingLabel;
static bool g_openConfirm = false;
static std::string g_lastBackup;

static void confirm(const std::string& label, std::function<void()> fn) {
    g_pending = std::move(fn);
    g_pendingLabel = label;
    g_openConfirm = true;
}

static void cmd(const std::string& label, const std::string& verb, std::vector<std::string> args = {}) {
    confirm(label, [verb, args] { enqueue({verb, args}); });
}

static void drawConfirm() {
    if (g_openConfirm) {
        ImGui::OpenPopup("Make Backup Before Applying Changes?");
        g_openConfirm = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Make Backup Before Applying Changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(MUTED, "Change:");
        ImGui::SameLine();
        ImGui::TextColored(CYAN, "%s", g_pendingLabel.c_str());
        ImGui::Spacing();
        bool yes = ImGui::Button("Yes, back up first (Y)") || ImGui::IsKeyPressed(ImGuiKey_Y);
        ImGui::SameLine();
        bool no = ImGui::Button("No, just apply (N)") || ImGui::IsKeyPressed(ImGuiKey_N);
        ImGui::SameLine();
        bool cancel = ImGui::Button("Cancel (Esc)") || ImGui::IsKeyPressed(ImGuiKey_Escape);
        if (yes) g_lastBackup = backupSaves("in-game");
        if (yes || no) {
            if (g_pending) g_pending();
        }
        if (yes || no || cancel) {
            g_pending = nullptr;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ------------------------------------------------------------------ menu

static std::string g_selected;
static char g_statFilter[64] = "";
static float g_size = 1, g_weight = 1, g_speed = 1;
static int g_spawnPick[2] = {0, 0};

static void bodyRow(const char* label, const char* verb, float& value, float lo, float hi,
                    std::vector<std::pair<const char*, float>> presets, const CharView& c) {
    ImGui::PushID(verb);
    ImGui::TextColored(CYAN, "%-7s", label);
    ImGui::SameLine(80);
    ImGui::SetNextItemWidth(200);
    ImGui::SliderFloat("##v", &value, lo, hi, "%.2fx");
    ImGui::SameLine();
    char buf[32];
    snprintf(buf, sizeof buf, "%.2f", value);
    if (ImGui::Button("Apply")) cmd(std::string(label) + " " + buf + "x on " + c.name, verb, {c.id, buf});
    for (auto& [n, v] : presets) {
        ImGui::SameLine();
        if (ImGui::Button(n)) {
            snprintf(buf, sizeof buf, "%.2f", v);
            value = v;
            cmd(std::string(label) + " " + n + " on " + c.name, verb, {c.id, buf});
        }
    }
    ImGui::PopID();
}

static void drawCharacter(const CharView& c, const Snapshot& s) {
    std::vector<std::string> ct = {c.role};
    if (c.dead) ct.push_back("dead");
    bool modded = std::fabs(c.scale - 1) > 0.01 || std::fabs(c.mass - 1) > 0.01 || std::fabs(c.speed - 1) > 0.01;
    if (modded) ct.push_back("modded");
    ImGui::TextColored(CYAN, "%s", c.name.c_str());
    tags(ct);
    ImGui::SameLine();
    ImGui::TextColored(DIM, "  %s", c.cls.c_str());
    ImGui::TextColored(MUTED, "Size %.2fx   Weight %.2fx   Speed %.2fx", c.scale, c.mass, c.speed);

    if (!ImGui::BeginTabBar("char")) return;
    if (ImGui::BeginTabItem("Body")) {
        bodyRow("Size", "scale", g_size, 0.1f, 5, {{"Pocket", 0.35f}, {"Normal", 1}, {"Big", 1.5f}, {"Giant", 3}}, c);
        bodyRow("Weight", "mass", g_weight, 0.05f, 8, {{"Feather", 0.15f}, {"Normal", 1}, {"Heavy", 2.5f}, {"Anvil", 6}}, c);
        bodyRow("Speed", "speed", g_speed, 0.05f, 4, {{"Slug", 0.4f}, {"Normal", 1}, {"Blitz", 2.5f}}, c);
        ImGui::Spacing();
        if (ImGui::Button("Reset this character")) cmd("Reset " + c.name, "reset", {c.id});
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Stats")) {
        ImGui::SetNextItemWidth(260);
        ImGui::InputTextWithHint("##filter", "filter (health, team...)", g_statFilter, sizeof g_statFilter);
        std::string flt = lower(g_statFilter);
        if (ImGui::BeginTable("stats", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH,
                              ImVec2(0, ImGui::GetContentRegionAvail().y))) {
            ImGui::TableSetupColumn("Stat");
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 160);
            ImGui::TableSetupColumn("Tag", ImGuiTableColumnFlags_WidthFixed, 100);
            ImGui::TableHeadersRow();
            for (auto& p : c.props) {
                std::string g = groupOf(p.n);
                if (!flt.empty() && lower(p.n).find(flt) == std::string::npos && g.find(flt) == std::string::npos) continue;
                ImGui::PushID(p.n.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(p.n.c_str());
                ImGui::TableNextColumn();
                if (p.t == "bool") {
                    bool b = p.v != 0;
                    if (ImGui::Checkbox("##b", &b)) cmd(p.n + " = " + (b ? "true" : "false"), "set", {c.id, p.n, b ? "true" : "false"});
                } else {
                    double v = p.v;
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::InputDouble("##n", &v, 0, 0, p.t == "int" ? "%.0f" : "%.4g", ImGuiInputTextFlags_EnterReturnsTrue)) {
                        char buf[64];
                        snprintf(buf, sizeof buf, "%.9g", v);
                        cmd(p.n + " = " + buf, "set", {c.id, p.n, buf});
                    }
                }
                ImGui::TableNextColumn();
                ImGui::TextColored(tagColor(g), "#%s", g.c_str());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Armor & Weapons")) {
        for (auto& g : c.gear) {
            ImGui::PushID(g.id.c_str());
            ImGui::TextColored(tagColor(g.kind), "#%s", g.kind.c_str());
            ImGui::SameLine();
            ImGui::Text("%s", g.name.c_str());
            ImGui::SameLine();
            ImGui::TextColored(DIM, "%s  %.2fx", g.cls.c_str(), g.scale);
            const char* big = g.kind == "weapon" ? "3" : "2.5";
            if (ImGui::SmallButton("Giant")) cmd("Giant " + g.name, "scale", {g.id, big});
            ImGui::SameLine();
            if (ImGui::SmallButton("Tiny")) cmd("Tiny " + g.name, "scale", {g.id, "0.4"});
            ImGui::SameLine();
            if (ImGui::SmallButton("Normal")) cmd("Normal size " + g.name, "scale", {g.id, "1"});
            if (g.kind == "weapon") {
                ImGui::SameLine();
                if (ImGui::SmallButton("Feather")) cmd("Feather " + g.name, "mass", {g.id, "0.1"});
                ImGui::SameLine();
                if (ImGui::SmallButton("Heavy")) cmd("Heavy " + g.name, "mass", {g.id, "5"});
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Hide")) cmd("Hide " + g.name, "hide", {g.id});
            ImGui::SameLine();
            if (ImGui::SmallButton("Show")) cmd("Show " + g.name, "show", {g.id});
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) cmd("Remove " + g.name, "vanish", {g.id});
            ImGui::Separator();
            ImGui::PopID();
        }
        if (c.gear.empty()) ImGui::TextColored(MUTED, "No armor or weapons found on this character.");
        const std::vector<ClassView>* lists[2] = {&s.weapons, &s.armor};
        const char* labels[2] = {"Spawn weapon", "Spawn armor"};
        for (int i = 0; i < 2; i++) {
            auto& l = *lists[i];
            if (l.empty()) continue;
            ImGui::PushID(i);
            g_spawnPick[i] = std::min(g_spawnPick[i], (int)l.size() - 1);
            ImGui::SetNextItemWidth(320);
            if (ImGui::BeginCombo("##pick", l[g_spawnPick[i]].name.c_str())) {
                for (int k = 0; k < (int)l.size(); k++)
                    if (ImGui::Selectable(l[k].name.c_str(), k == g_spawnPick[i])) g_spawnPick[i] = k;
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button(labels[i])) cmd(std::string(labels[i]) + ": " + l[g_spawnPick[i]].name, "spawn", {l[g_spawnPick[i]].path, "1", c.id});
            ImGui::PopID();
        }
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Goofy")) {
        struct B { const char* label; const char* verb; const char* arg; };
        static const B buttons[] = {
            {"Launch to the sky", "launch", "2500"}, {"Clone", "clone", "1"}, {"Clone army (x6)", "clone", "6"},
            {"Bring to me", "bring", nullptr}, {"Teleport me there", "visit", nullptr}, {"Freeze", "speed", "0.02"},
            {"Hyper", "speed", "3"}, {"Ant", "scale", "0.2"}, {"Colossus", "scale", "4"},
            {"Balloon (weightless)", "mass", "0.05"}, {"Invisible", "hide", nullptr}, {"Visible", "show", nullptr},
        };
        int i = 0;
        for (auto& b : buttons) {
            if (i++ % 3) ImGui::SameLine();
            if (ImGui::Button(b.label, ImVec2(180, 34))) {
                std::vector<std::string> args = {c.id};
                if (b.arg) args.push_back(b.arg);
                cmd(std::string(b.label) + " (" + c.name + ")", b.verb, args);
            }
        }
        if (c.role != "player") {
            ImGui::PushStyleColor(ImGuiCol_Button, rgb(244, 63, 94, 0.6f));
            if (ImGui::Button("Vanish", ImVec2(180, 34))) cmd("Delete " + c.name + " from the world", "vanish", {c.id});
            ImGui::PopStyleColor();
        }
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

static void drawCharacters(const Snapshot& s) {
    static std::string sliderFor;
    ImGui::BeginChild("list", ImVec2(250, 0), ImGuiChildFlags_Borders);
    const CharView* sel = nullptr;
    for (auto& c : s.chars)
        if (c.id == g_selected) sel = &c;
    if (!sel && !s.chars.empty()) {
        sel = &s.chars[0];
        g_selected = sel->id;
    }
    if (sel && sliderFor != sel->id) {  // sliders start at the character's current values
        sliderFor = sel->id;
        g_size = (float)sel->scale;
        g_weight = (float)sel->mass;
        g_speed = (float)sel->speed;
    }
    for (int pass = 0; pass < 3; pass++) {
        const char* role = pass == 0 ? "player" : pass == 1 ? "mercenary" : "opponent";
        for (auto& c : s.chars) {
            if (c.role != role) continue;
            ImGui::PushID(c.id.c_str());
            if (ImGui::Selectable(c.name.c_str(), c.id == g_selected)) g_selected = c.id;
            ImGui::Indent(10);
            ImGui::TextColored(tagColor(c.role), "#%s", c.role.c_str());
            if (c.dead) { ImGui::SameLine(); ImGui::TextColored(DIM, "#dead"); }
            if (std::fabs(c.scale - 1) > 0.01 || std::fabs(c.mass - 1) > 0.01 || std::fabs(c.speed - 1) > 0.01) {
                ImGui::SameLine();
                ImGui::TextColored(VIOLET, "#modded");
            }
            ImGui::Unindent(10);
            ImGui::PopID();
        }
    }
    if (s.chars.empty()) ImGui::TextColored(MUTED, "No characters yet.\nStart a fight.");
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("detail", ImVec2(0, 0));
    if (sel) drawCharacter(*sel, s);
    ImGui::EndChild();
}

static void drawCoins(const Snapshot& s) {
    if (s.economy.empty()) {
        ImGui::TextColored(MUTED, "No coin values found. Use \"Dump for Claude\" in the terminal app to find them.");
        return;
    }
    if (!ImGui::BeginTable("coins", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) return;
    ImGui::TableSetupColumn("Where");
    ImGui::TableSetupColumn("Name");
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 380);
    ImGui::TableHeadersRow();
    for (auto& e : s.economy) {
        ImGui::PushID((e.ownerId + e.n).c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(MUTED, "%s", e.owner.c_str());
        ImGui::TableNextColumn();
        ImGui::TextColored(tagColor("coins"), "%s", e.n.c_str());
        ImGui::TableNextColumn();
        double v = e.v;
        ImGui::SetNextItemWidth(130);
        char buf[64];
        if (ImGui::InputDouble("##v", &v, 0, 0, "%.0f", ImGuiInputTextFlags_EnterReturnsTrue)) {
            snprintf(buf, sizeof buf, "%.0f", v);
            cmd(e.n + " = " + buf, "set", {e.ownerId, e.n, buf});
        }
        for (double add : {100.0, 1000.0}) {
            ImGui::SameLine();
            snprintf(buf, sizeof buf, "+%.0f", add);
            if (ImGui::Button(buf)) {
                char val[64];
                snprintf(val, sizeof val, "%.0f", e.v + add);
                cmd(e.n + " " + buf, "set", {e.ownerId, e.n, val});
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("99,999")) cmd(e.n + " = 99999", "set", {e.ownerId, e.n, "99999"});
        ImGui::PopID();
    }
    ImGui::EndTable();
}

static void drawMods() {
    auto cfg = mods();
    auto get = [&](const char* k, const char* d) { auto it = cfg.find(k); return it != cfg.end() ? it->second : std::string(d); };
    for (auto& m : MODS) {
        ImGui::PushID(m.key);
        bool v = get(m.key, "0") == "1";
        if (ImGui::Checkbox("##on", &v)) {
            std::string key = m.key;
            confirm(std::string(m.title) + (v ? " ON" : " off"), [key, v] {
                if (v)
                    for (auto& group : EXCLUSIVE) {
                        bool in = false;
                        for (auto k : group) in |= key == k;
                        if (in)
                            for (auto k : group)
                                if (key != k) setMod(k, "0");
                    }
                setMod(key, v ? "1" : "0");
            });
        }
        ImGui::SameLine();
        ImGui::Text("%s", m.title);
        std::vector<std::string> ts(m.tags.begin(), m.tags.end());
        tags(ts);
        for (auto& [pk, def] : m.params) {
            ImGui::SameLine();
            float f = (float)atof(get(pk, def).c_str());
            ImGui::SetNextItemWidth(70);
            if (ImGui::InputFloat(pk, &f, 0, 0, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue)) {
                char buf[32];
                snprintf(buf, sizeof buf, "%.2f", f);
                std::string key = pk, val = buf;
                confirm(std::string(m.title) + ": " + pk + " = " + buf, [key, val] { setMod(key, val); });
            }
        }
        ImGui::TextColored(DIM, "      %s", m.desc);
        ImGui::PopID();
    }
    ImGui::Separator();
    std::string g = get("gravity", "normal");
    int cur = 0;
    for (int i = 0; i < 7; i++)
        if (g == GRAVITY_KEYS[i]) cur = i;
    ImGui::Text("Gravity");
    tags({"world", "chaos"});
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    if (ImGui::Combo("##gravity", &cur, GRAVITY_NAMES, 7)) {
        std::string key = GRAVITY_KEYS[cur];
        confirm(std::string("Gravity: ") + GRAVITY_NAMES[cur], [key] { setMod("gravity", key); });
    }
    ImGui::Spacing();
    if (ImGui::Button("Launch everyone", ImVec2(180, 32))) cmd("Launch everyone", "launch", {"all", "2200"});
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, rgb(244, 63, 94, 0.6f));
    if (ImGui::Button("Panic reset (F8)", ImVec2(180, 32))) enqueue({"panic", {}});
    ImGui::PopStyleColor();
}

static void drawMenu() {
    Snapshot s = snapshot();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(900, 600), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    bool open = true;
    if (ImGui::Begin("HALF SWORD LAB", &open, ImGuiWindowFlags_NoCollapse)) {
        if (s.ready) ImGui::TextColored(rgb(52, 211, 153), "LIVE");
        else ImGui::TextColored(rgb(250, 204, 21), "%s", s.status.c_str());
        ImGui::SameLine();
        ImGui::TextColored(MUTED, "%s   %d characters   \\ closes   F8 panic reset%s", s.map.c_str(), (int)s.chars.size(),
                           s.paused ? "   (mods paused)" : "");
        if (ImGui::BeginTabBar("main")) {
            if (ImGui::BeginTabItem("Characters")) { drawCharacters(s); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Coins")) { drawCoins(s); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Mods")) { drawMods(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Backups")) {
                if (ImGui::Button("Back up saves now", ImVec2(200, 32))) g_lastBackup = backupSaves("in-game");
                if (!g_lastBackup.empty()) ImGui::TextColored(MUTED, "%s", g_lastBackup.c_str());
                ImGui::TextColored(DIM, "Every change asks first. All backups (newest first) and restore\n"
                                        "are in the Backups tab of the terminal app (run.sh).");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Log")) {
                for (auto it = s.log.rbegin(); it != s.log.rend(); ++it) ImGui::TextColored(MUTED, "%s", it->c_str());
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        drawConfirm();
    }
    ImGui::End();
    if (!open) g_visible = false;
}

static void drawToast() {
    float t = (GetTickCount() - g_initTick) / 1000.f;
    if (t > 7 || g_visible) return;
    ImGui::SetNextWindowPos(ImVec2(20, 20));
    ImGui::SetNextWindowBgAlpha(0.85f * std::min(1.f, 7 - t));
    ImGui::Begin("##toast", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                         ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextColored(VIOLET, "HALF SWORD LAB");
    ImGui::SameLine();
    ImGui::Text("loaded - press \\ to open");
    ImGui::End();
}

static void applyStyle(float scale) {
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 10;
    st.ChildRounding = 8;
    st.FrameRounding = 6;
    st.GrabRounding = 6;
    st.TabRounding = 6;
    st.PopupRounding = 8;
    st.WindowPadding = ImVec2(14, 12);
    st.ItemSpacing = ImVec2(8, 7);
    st.FramePadding = ImVec2(8, 5);
    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = rgb(236, 236, 244);
    c[ImGuiCol_TextDisabled] = MUTED;
    c[ImGuiCol_WindowBg] = rgb(8, 8, 13, 0.95f);
    c[ImGuiCol_ChildBg] = rgb(18, 18, 27, 0.9f);
    c[ImGuiCol_PopupBg] = rgb(14, 14, 22, 0.98f);
    c[ImGuiCol_Border] = rgb(139, 92, 246, 0.35f);
    c[ImGuiCol_FrameBg] = rgb(24, 24, 36);
    c[ImGuiCol_FrameBgHovered] = rgb(139, 92, 246, 0.25f);
    c[ImGuiCol_FrameBgActive] = rgb(139, 92, 246, 0.4f);
    c[ImGuiCol_TitleBg] = rgb(14, 14, 22);
    c[ImGuiCol_TitleBgActive] = rgb(24, 18, 44);
    c[ImGuiCol_Button] = rgb(24, 24, 36);
    c[ImGuiCol_ButtonHovered] = rgb(139, 92, 246, 0.45f);
    c[ImGuiCol_ButtonActive] = VIOLET;
    c[ImGuiCol_Header] = rgb(139, 92, 246, 0.3f);
    c[ImGuiCol_HeaderHovered] = rgb(139, 92, 246, 0.45f);
    c[ImGuiCol_HeaderActive] = rgb(139, 92, 246, 0.6f);
    c[ImGuiCol_Tab] = rgb(18, 18, 27);
    c[ImGuiCol_TabHovered] = rgb(139, 92, 246, 0.45f);
    c[ImGuiCol_TabSelected] = rgb(139, 92, 246, 0.6f);
    c[ImGuiCol_CheckMark] = CYAN;
    c[ImGuiCol_SliderGrab] = VIOLET;
    c[ImGuiCol_SliderGrabActive] = CYAN;
    c[ImGuiCol_TableHeaderBg] = rgb(24, 24, 36);
    c[ImGuiCol_TableRowBgAlt] = rgb(255, 255, 255, 0.03f);
    c[ImGuiCol_ModalWindowDimBg] = rgb(0, 0, 0, 0.55f);
    st.ScaleAllSizes(scale);
    st.FontScaleMain = scale;
}

// ------------------------------------------------------------------ input

static void setVisible(bool v) {
    g_visible = v;
    if (!g_imgui) return;
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = v;
    if (v) {
        RECT r;
        GetClientRect(g_hwnd, &r);
        g_vx = (r.right - r.left) / 2.f;
        g_vy = (r.bottom - r.top) / 2.f;
        ClipCursor(nullptr);
    }
}

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && !(l & (1 << 30))) {
        if (w == g_toggleKey) { setVisible(!g_visible); return 0; }
        if (w == VK_F8) enqueue({"panic", {}});
    }
    if (m == WM_CHAR && w == '\\' && g_toggleKey == VK_OEM_5) return 0;
    if (g_visible && g_imgui) {
        ImGuiIO& io = ImGui::GetIO();
        if (m == WM_INPUT) {
            alignas(8) BYTE buf[256];
            UINT size = sizeof buf;
            if (GetRawInputData((HRAWINPUT)l, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1) {
                auto* ri = (RAWINPUT*)buf;
                if (ri->header.dwType == RIM_TYPEMOUSE) {
                    auto& mo = ri->data.mouse;
                    g_lastRaw = GetTickCount();
                    if (!(mo.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                        RECT r;
                        GetClientRect(g_hwnd, &r);
                        g_vx = std::clamp(g_vx + mo.lLastX, 0.f, (float)r.right);
                        g_vy = std::clamp(g_vy + mo.lLastY, 0.f, (float)r.bottom);
                    }
                    USHORT f = mo.usButtonFlags;
                    if (f & RI_MOUSE_LEFT_BUTTON_DOWN) io.AddMouseButtonEvent(0, true);
                    if (f & RI_MOUSE_LEFT_BUTTON_UP) io.AddMouseButtonEvent(0, false);
                    if (f & RI_MOUSE_RIGHT_BUTTON_DOWN) io.AddMouseButtonEvent(1, true);
                    if (f & RI_MOUSE_RIGHT_BUTTON_UP) io.AddMouseButtonEvent(1, false);
                    if (f & RI_MOUSE_WHEEL) io.AddMouseWheelEvent(0, (short)mo.usButtonData / (float)WHEEL_DELTA);
                }
            }
            return 0;
        }
        if (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) {
            if (GetTickCount() - g_lastRaw > 1000) {
                ImGui_ImplWin32_WndProcHandler(h, m, w, l);
                if (m == WM_MOUSEMOVE) { g_vx = (float)(short)LOWORD(l); g_vy = (float)(short)HIWORD(l); }
            }
            return 0;
        }
        if ((m >= WM_KEYFIRST && m <= WM_KEYLAST) || m == WM_CHAR) {
            ImGui_ImplWin32_WndProcHandler(h, m, w, l);
            return 0;
        }
    }
    return CallWindowProcW(g_origWndProc, h, m, w, l);
}

// ------------------------------------------------------------------ rendering

static void initImGui(HWND hwnd) {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    RECT r;
    GetClientRect(hwnd, &r);
    float scale = std::max(1.f, (r.bottom - r.top) / 900.f);
    applyStyle(scale);
    ImGui_ImplWin32_Init(hwnd);
    g_hwnd = hwnd;
    g_origWndProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)wndProc);
    g_imgui = true;
    g_initTick = GetTickCount();
    log("overlay: ImGui ready on window %p (%ldx%ld)", (void*)hwnd, r.right, r.bottom);
}

static void newFrame() {
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    if (g_visible) ImGui::GetIO().AddMousePosEvent(g_vx, g_vy);
    drawToast();
    if (g_visible) drawMenu();
    ImGui::Render();
}

static void dumpScreenshot11(IDXGISwapChain* sc);

static void render11(IDXGISwapChain* sc) {
    if (!g_d11) {
        if (FAILED(sc->GetDevice(IID_PPV_ARGS(&g_d11)))) return;
        g_d11->GetImmediateContext(&g_ctx);
    }
    if (!g_dx11Backend) g_dx11Backend = ImGui_ImplDX11_Init(g_d11, g_ctx);
    if (!g_rtv11) {
        ID3D11Texture2D* bb = nullptr;
        if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&bb)))) return;
        g_d11->CreateRenderTargetView(bb, nullptr, &g_rtv11);
        bb->Release();
        if (!g_rtv11) return;
    }
    ImGui_ImplDX11_NewFrame();
    newFrame();
    g_ctx->OMSetRenderTargets(1, &g_rtv11, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    dumpScreenshot11(sc);
}

static void srvAlloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
    int i = g_srvFree.back();
    g_srvFree.pop_back();
    *cpu = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
    *gpu = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
    cpu->ptr += (SIZE_T)i * g_srvInc;
    gpu->ptr += (UINT64)i * g_srvInc;
}

static void srvFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE) {
    g_srvFree.push_back((int)((cpu.ptr - g_srvHeap->GetCPUDescriptorHandleForHeapStart().ptr) / g_srvInc));
}

static void waitFence(UINT64 v) {
    if (g_fence && g_fence->GetCompletedValue() < v) {
        g_fence->SetEventOnCompletion(v, g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, 1000);
    }
}

static void releaseTargets12() {
    waitFence(g_fenceValue);
    for (auto& f : g_frames)
        if (f.rt) { f.rt->Release(); f.rt = nullptr; }
}

static bool setup12(IDXGISwapChain* sc) {
    if (!g_d12 && FAILED(sc->GetDevice(IID_PPV_ARGS(&g_d12)))) return false;
    DXGI_SWAP_CHAIN_DESC d;
    sc->GetDesc(&d);
    UINT n = d.BufferCount;
    if (g_frames.size() != n) {
        releaseTargets12();
        for (auto& f : g_frames) if (f.alloc) f.alloc->Release();
        g_frames.assign(n, Frame{});
        if (g_rtvHeap) g_rtvHeap->Release();
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, n, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (FAILED(g_d12->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtvHeap)))) return false;
        UINT inc = g_d12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < n; i++) {
            g_frames[i].rtv = h;
            h.ptr += inc;
            if (FAILED(g_d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_frames[i].alloc)))) return false;
        }
    }
    if (!g_srvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 64, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        if (FAILED(g_d12->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_srvHeap)))) return false;
        g_srvInc = g_d12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (int i = 63; i >= 0; i--) g_srvFree.push_back(i);
    }
    if (!g_list) {
        if (FAILED(g_d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frames[0].alloc, nullptr, IID_PPV_ARGS(&g_list)))) return false;
        g_list->Close();
        g_d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
        g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    for (UINT i = 0; i < n; i++) {
        if (g_frames[i].rt) continue;
        if (FAILED(sc->GetBuffer(i, IID_PPV_ARGS(&g_frames[i].rt)))) return false;
        g_d12->CreateRenderTargetView(g_frames[i].rt, nullptr, g_frames[i].rtv);
    }
    if (!g_dx12Backend) {
        ImGui_ImplDX12_InitInfo info;
        info.Device = g_d12;
        info.CommandQueue = g_queue;
        info.NumFramesInFlight = (int)n;
        info.RTVFormat = d.BufferDesc.Format;
        info.DSVFormat = DXGI_FORMAT_UNKNOWN;
        info.SrvDescriptorHeap = g_srvHeap;
        info.SrvDescriptorAllocFn = srvAlloc;
        info.SrvDescriptorFreeFn = srvFree;
        g_dx12Backend = ImGui_ImplDX12_Init(&info);
        log("overlay: D3D12 backend %s (%u buffers, format %d)", g_dx12Backend ? "ready" : "FAILED", n, (int)d.BufferDesc.Format);
    }
    return g_dx12Backend;
}

static void render12(IDXGISwapChain* sc) {
    if (!g_queue) return;  // waiting for the game's command queue
    if (!setup12(sc)) return;
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) return;
    UINT idx = sc3->GetCurrentBackBufferIndex();
    sc3->Release();
    if (idx >= g_frames.size()) return;
    Frame& f = g_frames[idx];
    waitFence(f.fence);

    ImGui_ImplDX12_NewFrame();
    newFrame();

    f.alloc->Reset();
    g_list->Reset(f.alloc, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = f.rt;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_list->ResourceBarrier(1, &b);
    g_list->OMSetRenderTargets(1, &f.rtv, FALSE, nullptr);
    g_list->SetDescriptorHeaps(1, &g_srvHeap);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_list);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    g_list->ResourceBarrier(1, &b);
    g_list->Close();
    ID3D12CommandList* lists[] = {g_list};
    g_queue->ExecuteCommandLists(1, lists);
    g_queue->Signal(g_fence, ++g_fenceValue);
    f.fence = g_fenceValue;
}

static void frame(IDXGISwapChain* sc, UINT flags) {
    if (flags & DXGI_PRESENT_TEST) return;
    if (sc != g_sc) {
        // New swap chain (startup, fullscreen toggle): drop per-swapchain resources.
        if (g_rtv11) { g_rtv11->Release(); g_rtv11 = nullptr; }
        releaseTargets12();
        g_sc = sc;
        ID3D12Device* d12 = nullptr;
        if (SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&d12)))) { g_api = Api::D3D12; d12->Release(); }
        else g_api = Api::D3D11;
        log("overlay: swap chain %p uses %s", (void*)sc, g_api == Api::D3D12 ? "D3D12" : "D3D11");
    }
    if (!g_imgui) {
        DXGI_SWAP_CHAIN_DESC d;
        if (FAILED(sc->GetDesc(&d)) || !d.OutputWindow) return;
        initImGui(d.OutputWindow);
    }
    bool toast = GetTickCount() - g_initTick < 7500;
    if (!g_visible && !toast) return;
    if (g_api == Api::D3D11) render11(sc);
    else render12(sc);
}

static thread_local int t_depth = 0;

static HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (t_depth++ == 0) frame(sc, flags);
    HRESULT r = oPresent(sc, sync, flags);
    t_depth--;
    return r;
}

static HRESULT STDMETHODCALLTYPE hkPresent11(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (t_depth++ == 0) frame(sc, flags);
    HRESULT r = oPresent11(sc, sync, flags);
    t_depth--;
    return r;
}

static HRESULT STDMETHODCALLTYPE hkPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    if (t_depth++ == 0) frame(sc, flags);
    HRESULT r = oPresent1(sc, sync, flags, p);
    t_depth--;
    return r;
}

static void beforeResize() {
    if (g_rtv11) { g_rtv11->Release(); g_rtv11 = nullptr; }
    releaseTargets12();
}

static HRESULT STDMETHODCALLTYPE hkResize(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    beforeResize();
    return oResize(sc, n, w, h, f, fl);
}

static HRESULT STDMETHODCALLTYPE hkResize11(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    beforeResize();
    return oResize11(sc, n, w, h, f, fl);
}

static void STDMETHODCALLTYPE hkExec(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    if (!g_queue) {
        D3D12_COMMAND_QUEUE_DESC d = q->GetDesc();
        if (d.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            g_queue = q;
            q->AddRef();
            log("overlay: captured D3D12 direct queue %p", (void*)q);
        }
    }
    oExec(q, n, lists);
}

// Test hook: HSL_SCREENSHOT=<path.bmp> saves one D3D11 frame with the menu open.
static void dumpScreenshot11(IDXGISwapChain* sc) {
    static int frames = 0;
    static bool done = false;
    wchar_t path[MAX_PATH];
    if (done || !g_visible || !GetEnvironmentVariableW(L"HSL_SCREENSHOT", path, MAX_PATH)) return;
    if (++frames < 40) return;
    done = true;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&bb)))) return;
    D3D11_TEXTURE2D_DESC td;
    bb->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (SUCCEEDED(g_d11->CreateTexture2D(&td, nullptr, &st))) {
        g_ctx->CopyResource(st, bb);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            FILE* f = _wfopen(path, L"wb");
            if (f) {
                int w = td.Width, h = td.Height, row = w * 4;
                BITMAPFILEHEADER fh{0x4D42, (DWORD)(54 + row * h), 0, 0, 54};
                BITMAPINFOHEADER ih{sizeof ih, w, -h, 1, 32, BI_RGB, (DWORD)(row * h), 0, 0, 0, 0};
                fwrite(&fh, sizeof fh, 1, f);
                fwrite(&ih, sizeof ih, 1, f);
                bool rgba = td.Format == DXGI_FORMAT_R8G8B8A8_UNORM || td.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                std::vector<uint8_t> line(row);
                for (int y = 0; y < h; y++) {
                    memcpy(line.data(), (uint8_t*)m.pData + y * m.RowPitch, row);
                    if (rgba)
                        for (int x = 0; x < w; x++) std::swap(line[x * 4], line[x * 4 + 2]);
                    fwrite(line.data(), 1, row, f);
                }
                fclose(f);
                log("overlay: screenshot written");
            }
            g_ctx->Unmap(st, 0);
        }
        st->Release();
    }
    bb->Release();
}

// ------------------------------------------------------------------ hook installation

static HWND dummyWindow() {
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"HalfSwordLabDummy";
    RegisterClassExW(&wc);
    return CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
}

static std::vector<void*> g_hooked;

static bool hook(void* target, void* detour, void** original, const char* what) {
    if (!target) return false;
    for (void* t : g_hooked)
        if (t == target) {
            log("overlay: %s at %p is shared with an existing hook", what, target);
            return true;
        }
    MH_STATUS s = MH_CreateHook(target, detour, original);
    if (s == MH_OK) g_hooked.push_back(target);
    if (s == MH_OK) s = MH_EnableHook(target);
    log("overlay: hook %s at %p: %s", what, target, MH_StatusToString(s));
    return s == MH_OK;
}

static void installD3D12(HWND hwnd) {
    HMODULE d12 = LoadLibraryW(L"d3d12.dll"), dxgi = LoadLibraryW(L"dxgi.dll");
    if (!d12 || !dxgi) return;
    auto create = (PFN_D3D12_CREATE_DEVICE)GetProcAddress(d12, "D3D12CreateDevice");
    auto factoryFn = (HRESULT(WINAPI*)(REFIID, void**))GetProcAddress(dxgi, "CreateDXGIFactory1");
    if (!create || !factoryFn) return;
    ID3D12Device* dev = nullptr;
    ID3D12CommandQueue* q = nullptr;
    IDXGIFactory4* fac = nullptr;
    IDXGISwapChain1* sc = nullptr;
    if (SUCCEEDED(create(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) && SUCCEEDED(factoryFn(IID_PPV_ARGS(&fac)))) {
            DXGI_SWAP_CHAIN_DESC1 sd{};
            sd.Width = sd.Height = 64;
            sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.SampleDesc.Count = 1;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.BufferCount = 2;
            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            if (SUCCEEDED(fac->CreateSwapChainForHwnd(q, hwnd, &sd, nullptr, nullptr, &sc))) {
                void** vt = *(void***)sc;
                hook(vt[8], (void*)hkPresent, (void**)&oPresent, "D3D12 Present");
                hook(vt[22], (void*)hkPresent1, (void**)&oPresent1, "D3D12 Present1");
                hook(vt[13], (void*)hkResize, (void**)&oResize, "D3D12 ResizeBuffers");
                hook((*(void***)q)[10], (void*)hkExec, (void**)&oExec, "ExecuteCommandLists");
            }
        }
    } else {
        log("overlay: no D3D12 device available (fine if the game runs D3D11)");
    }
    if (sc) sc->Release();
    if (fac) fac->Release();
    if (q) q->Release();
    if (dev) dev->Release();
}

static void installD3D11(HWND hwnd) {
    HMODULE d11 = LoadLibraryW(L"d3d11.dll");
    if (!d11) return;
    auto create = (PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)GetProcAddress(d11, "D3D11CreateDeviceAndSwapChain");
    if (!create) return;
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    if (SUCCEEDED(create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx))) {
        void** vt = *(void***)sc;
        // Under DXVK/vkd3d both APIs can share one DXGI swap chain class; hook() skips duplicates.
        hook(vt[8], (void*)hkPresent11, (void**)&oPresent11, "D3D11 Present");
        hook(vt[13], (void*)hkResize11, (void**)&oResize11, "D3D11 ResizeBuffers");
    }
    if (sc) sc->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
}

static DWORD WINAPI thread(LPVOID) {
    MH_Initialize();
    HWND hwnd = dummyWindow();
    installD3D12(hwnd);
    installD3D11(hwnd);
    DestroyWindow(hwnd);
    return 0;
}

void start(unsigned toggleKey) {
    if (toggleKey) g_toggleKey = toggleKey;
    CloseHandle(CreateThread(nullptr, 0, thread, nullptr, 0, nullptr));
}

}  // namespace overlay
