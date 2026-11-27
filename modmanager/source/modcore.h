// The mods of a card's ports: everything that needs no console.
//
// This is the same thing the PC tool (tools/mods.py) does, on the same files, so that either can
// be used after the other:
//
//   <card>/mods/<mod>/mod.json          what a mod is: name, game, version, author, description, requires
//   <card>/mods/<mod>/files/<path>      its files, at the paths they have in the game
//   <card>/nx2-relink/<port>/port.json  which game a port is: {"title": ..., "title_id": ...} (may be missing)
//   <card>/nx2-relink/<port>/mods/<path>   what the port reads in place of the game's own file
//   <card>/nx2-relink/<port>/mods.json  which mods are on, in order, and the files each put there
//
// Putting a mod on copies its files into the port's mods folder; taking it off removes those files
// and no others. Two mods that have the same path overlap: the one later in the order is the one
// in use, and when it is taken off the other's file is put back. A file in the port's mods folder
// that no mod put there is left alone (a mod that has the same path replaces it, and that is said).
// A mod can be on for a port without being kept in /mods (the PC tool can put one on straight from
// the PC): it can stay on, in its place in the order, or be taken off; it can't be put on from here.
//
// Only the C/C++ standard library and POSIX-style file calls are used (fopen, opendir, stat, mkdir,
// rmdir, remove, rename), all given paths under one root: "sdmc:/" on the console, a folder on a PC.
//
// What is written, and where: files under <root>/nx2-relink/<port>/mods/ (each first under the name
// "<name>.nx2mods-tmp", renamed when whole), and <root>/nx2-relink/<port>/mods.json (first as
// "mods.json.nx2mods-tmp"). Nothing else is created, changed or removed; port.json is only read.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace modcore {

extern const char* const kToolFolder;   // "nx2-relink": the folder of the card the ports are in
extern const char* const kModsFolder;   // "mods": the folder of the card the mods are kept in
extern const char* const kTempSuffix;   // ".nx2mods-tmp": the end of the name a file has while it is being written

struct Mod {
    std::string id;            // its folder's name in /mods
    std::string name;          // from mod.json; its id when that has none
    std::string version, author, description;
    std::string game_title, game_title_id;   // the title id in lower case
    std::vector<std::string> needs;          // mod.json's "requires": other mods' ids
    std::map<std::string, int64_t> files;    // path in the game -> size in bytes
    std::string bad;           // why it can't be used; empty when it can
    bool absent = false;       // on for a port and not kept in /mods: `files` is what the port's record says it put there

    int64_t size() const;
    bool runs_code() const;    // it has a hook file (Code/<name>.nxh): code a port maps and calls as the game starts
};
typedef std::map<std::string, Mod> ModMap;

struct Port {
    std::string name;          // its folder's name
    std::string title, title_id;   // from port.json; both empty when the game isn't known
    int mods_on = 0;
};

struct StateMod {
    std::string id, name, version;
    std::map<std::string, int64_t> files;    // the paths of which this mod's file is the one in use -> size
};
struct State {
    std::vector<StateMod> mods;              // in order: a later one wins the paths it shares
};

enum Fit { FitYes, FitNo, FitUnknown };

/// <root>/<rel>, whatever slashes the two end and begin with.
std::string card_path(const std::string& root, const std::string& rel);

/// The mods kept in <root>/mods. False when that folder isn't there (the map is then empty).
bool read_mods(const std::string& root, ModMap& out);

/// The ports in <root>/nx2-relink, by name. False when that folder isn't there.
bool read_ports(const std::string& root, std::vector<Port>& out);

/// One port's identity and how many mods it has on.
Port read_port(const std::string& root, const std::string& name);

/// A port's record of the mods that are on; empty when there is none or it can't be read.
State read_state(const std::string& root, const std::string& port);

/// Whether a mod is for a port's game: by title id when both say one, else by title, else unknown.
Fit fits(const Mod& mod, const Port& port);

/// The paths more than one of these mods has: path -> the mods that have it, in the order given
/// (the last is the one in use).
std::map<std::string, std::vector<std::string>> overlaps(const std::vector<std::string>& order, const ModMap& mods);

/// What a port's screen shows.
struct PortView {
    Port port;
    State state;
    ModMap mods;                       // the mods offered: the card's that aren't for another game, and the absent ones that are on
    std::vector<std::string> on;       // the ones that are on and can stay on, in order
    std::vector<std::string> dropped;  // on by the record, but unusable now or for another game: taken off at the next apply
    int other_game = 0;                // how many of the card's mods are for another game (not offered)
    std::vector<std::string> loose;    // files in the port's mods folder that no mod put there
};
PortView load_port_view(const std::string& root, const std::string& port, const ModMap& card_mods);

/// What an apply would do. Nothing is written to work it out.
struct Plan {
    std::string port;
    std::vector<std::string> want;                 // the mods to be on, in order
    std::map<std::string, std::string> owner;      // path -> the mod whose file is to be the one in use
    std::vector<std::string> remove;               // recorded paths that no wanted mod has: removed
    std::vector<std::string> send;                 // paths to copy from their owner's folder in /mods
    std::vector<std::string> loose_hit;            // files put by hand that a mod's file replaces
    std::vector<std::string> unsafe;               // recorded paths this refuses to touch (not plain relative paths)
    std::vector<std::string> absent_missing;       // files of an absent mod that aren't whole on the card, and can't be put back from here
    std::vector<std::string> notes;                // overlaps and unmet needs, in words
    int64_t bytes = 0;                             // the sizes of `send` added up
    State old_state;
    std::map<std::string, std::pair<std::string, int64_t>> had;   // by the record: path -> (mod, size)
    std::map<std::string, int64_t> on_card;        // what the port's mods folder has now
};

/// Works out a plan. False, with why in `error`, when the mods wanted can't be put on: one isn't
/// in `mods`, can't be used, or is for another game; or the port isn't there.
/// `mods` is a PortView's (the card's mods and the absent ones).
bool make_plan(const std::string& root, const std::string& port, const std::vector<std::string>& want,
               const ModMap& mods, Plan& plan, std::string& error);

struct Progress {
    enum Phase { Removing, Copying, Checking, Recording };
    Phase phase = Removing;
    int index = 0, count = 0;      // which file of how many (of this phase)
    std::string path;              // the file, as a path in the game
    int64_t done = 0, total = 0;   // bytes copied so far, of all there are to copy
};
typedef std::function<void(const Progress&)> ProgressFn;

struct Result {
    bool ok = false;               // everything asked for was done
    bool refused = false;          // nothing was done: `summary` says why
    int removed = 0, copied = 0;
    int64_t bytes = 0;
    std::vector<std::string> failed;     // paths that couldn't be removed or copied
    std::vector<std::string> messages;   // notes and errors, one a line
    std::string summary;                 // the one line that says how it went
};

struct Options {
    size_t chunk = 1 << 20;        // bytes read and written at a time; progress is said after each
    bool read_back = true;         // a copied file is read again and compared with what was read from the mod
};

/// Makes the mods that are on for a port exactly `want`, in that order. Safe to run again after a
/// run that was cut short, by this or by the PC tool.
Result apply(const std::string& root, const std::string& port, const std::vector<std::string>& want,
             const ModMap& mods, const ProgressFn& progress = ProgressFn(), const Options& options = Options());

/// A size in words: "812 B", "34.5 KB", "12.3 MB".
std::string size_text(int64_t bytes);

} // namespace modcore
