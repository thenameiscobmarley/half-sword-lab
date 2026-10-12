// Half Sword Lab game logic: characters, gear, coins, mods, commands, bridge files.
#pragma once
#include <map>
#include <string>
#include <vector>

namespace lab {

struct PropView { std::string n, t; double v; };
struct GearView { std::string id, name, cls, kind, src; double scale; };
struct CharView {
    std::string id, name, cls, role;
    bool dead = false;
    double scale = 1, mass = 1, speed = 1;
    std::vector<PropView> props;
    std::vector<GearView> gear;
};
struct CoinView { std::string owner, ownerId, n, t; double v; };
struct ClassView { std::string name, path; };

struct Snapshot {
    bool ready = false;
    std::string status;
    std::string map;
    double gravity = 0;
    bool paused = false;
    std::vector<CharView> chars;
    std::vector<CoinView> economy;
    std::vector<ClassView> weapons, armor;
    std::vector<std::string> log;
};

struct Command {
    std::string verb;
    std::vector<std::string> args;
};

void setStatus(const std::string& s);   // shown in the overlay before the engine is ready
void start();                            // after ue::init succeeded
void onGameThread();                     // called from the ProcessEvent hook
void enqueue(const Command& c);          // from the overlay (any thread)
Snapshot snapshot();
std::map<std::string, std::string> mods();
void setMod(const std::string& key, const std::string& value);  // also writes mods.cfg
std::string backupSaves(const std::string& reason);              // returns a message
void note(const std::string& msg);

}  // namespace lab
