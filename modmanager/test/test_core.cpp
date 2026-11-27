// The logic core checked on a PC: a pretend card is built under a folder, mods are put on and
// taken off, and what is on the card afterwards is compared with what should be there. With
// --repo and --python it also runs the PC tool (tools/mods.py, through nxrelink.py) against the
// same pretend cards, in both directions, and compares what the two leave.
//
//   test_core <work folder> [--repo <nx2-recomp folder> --python <python>]
//
// The work folder is emptied first. All names in the pretend card are made up.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "json.h"
#include "modcore.h"

using modcore::ModMap;

namespace {

int g_checks = 0, g_failed = 0;
std::string g_section;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("  FAILED  [%s] %s\n", g_section.c_str(), what.c_str());
    }
}

void section(const char* name) {
    g_section = name;
    std::printf("-- %s\n", name);
}

// ---------------------------------------------------------------- files, for the test's own use
bool is_dir(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

void make_dirs(const std::string& path) {
    for (size_t at = 1; at <= path.size(); ++at) {
        if (at == path.size() || path[at] == '/') {
            mkdir(path.substr(0, at).c_str(), 0777);
        }
    }
}

void write_file(const std::string& path, const std::string& data) {
    make_dirs(path.substr(0, path.rfind('/')));
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::printf("the test can't write %s\n", path.c_str());
        std::exit(2);
    }
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
}

bool read_file(const std::string& path, std::string& out) {
    out.clear();
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
        out.append(buf, n);
    }
    std::fclose(f);
    return true;
}

std::string slurp(const std::string& path) {
    std::string s;
    return read_file(path, s) ? s : std::string("<no such file>");
}

void remove_tree(const std::string& path) {
    if (DIR* d = opendir(path.c_str())) {
        std::vector<std::string> names;
        while (const dirent* e = readdir(d)) {
            if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, "..")) {
                names.push_back(e->d_name);
            }
        }
        closedir(d);
        for (const std::string& n : names) {
            remove_tree(path + "/" + n);
        }
        rmdir(path.c_str());
    } else {
        std::remove(path.c_str());
    }
}

/// Every file under a folder with what it holds, and every folder (with the value "<dir>").
void tree(const std::string& dir, const std::string& prefix, std::map<std::string, std::string>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) {
        return;
    }
    std::vector<std::string> names;
    while (const dirent* e = readdir(d)) {
        if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, "..")) {
            names.push_back(e->d_name);
        }
    }
    closedir(d);
    for (const std::string& n : names) {
        const std::string full = dir + "/" + n;
        if (is_dir(full)) {
            out[prefix + n + "/"] = "<dir>";
            tree(full, prefix + n + "/", out);
        } else {
            out[prefix + n] = slurp(full);
        }
    }
}

std::map<std::string, std::string> tree_of(const std::string& dir) {
    std::map<std::string, std::string> out;
    tree(dir, "", out);
    return out;
}

/// Bytes that are told apart by a seed, and are not all alike from one megabyte to the next.
std::string blob(size_t size, unsigned seed) {
    std::string s(size, '\0');
    unsigned x = seed * 2654435761u + 12345u;
    for (size_t i = 0; i < size; ++i) {
        x = x * 1664525u + 1013904223u;
        s[i] = static_cast<char>(x >> 24);
    }
    return s;
}

// ---------------------------------------------------------------- the pretend card
const char* const kGameA = "0400aaaa00001000";
const char* const kGameB = "0400bbbb00002000";

struct Blobs {
    std::string colors_hero = blob(3000, 1), colors_palette = blob(700, 2);
    std::string shapes_hero = blob(3000, 3), shapes_tail = blob(41, 4), shapes_big = blob(3 * 1024 * 1024 + 517, 5);
    std::string needs_file = blob(1234, 6), other_file = blob(900, 7), nojson_file = blob(10, 8);
    std::string hand_notes = "put here by hand\n", hand_extra = blob(555, 9);
    std::string pc_only = blob(2222, 10), pc_hero = blob(3100, 11);
} const B;

void build_card(const std::string& card) {
    remove_tree(card);
    // ports
    write_file(card + "/nx2-relink/alpha/port.json", "{\n \"title\": \"Star Garden\",\n \"title_id\": \"0400AAAA00001000\"\n}");
    write_file(card + "/nx2-relink/alpha/mods/hand/notes.txt", B.hand_notes);
    write_file(card + "/nx2-relink/alpha/mods/model/extra.bin", B.hand_extra);
    write_file(card + "/nx2-relink/alpha/save/slot0.bin", "a save: not this program's business");
    make_dirs(card + "/nx2-relink/beta");
    write_file(card + "/nx2-relink/beta/log.txt", "a port with no port.json and no mods folder");
    write_file(card + "/nx2-relink/a-file-not-a-port.txt", "x");
    // mods
    write_file(card + "/mods/colors/mod.json",
               std::string("{\"name\": \"Brighter Colors\", \"game\": {\"title_id\": \"") + kGameA +
                   "\", \"title\": \"Star Garden\"}, \"version\": \"1.2\", \"author\": \"Somebody\", \"description\": \"Every colour a little brighter.\"}");
    write_file(card + "/mods/colors/files/model/hero.bin", B.colors_hero);
    write_file(card + "/mods/colors/files/ui/palette.dat", B.colors_palette);
    write_file(card + "/mods/shapes/mod.json",
               std::string("{\"name\": \"Rounder Shapes\", \"game\": {\"title_id\": \"0400AAAA00001000\"}, \"version\": 2.0, \"extra\": [1, {\"k\": null}, true]}"));
    write_file(card + "/mods/shapes/files/model/hero.bin", B.shapes_hero);
    write_file(card + "/mods/shapes/files/model/deep/a/b/tail.bin", B.shapes_tail);
    write_file(card + "/mods/shapes/files/stage/big.pack", B.shapes_big);
    // (a byte order mark, escapes, a game named by title alone, a need)
    write_file(card + "/mods/needs/mod.json",
               "\xEF\xBB\xBF{\"name\": \"Caf\\u00e9 \\\"Menu\\\" \\ud83d\\ude00\", \"game\": {\"title\": \"  star GARDEN \"}, \"version\": \"0.3\","
               " \"description\": \"line one\\nline two\", \"requires\": [\"colors\", \"not-there\"]}");
    write_file(card + "/mods/needs/files/ui/menu.dat", B.needs_file);
    write_file(card + "/mods/othergame/mod.json", std::string("{\"name\": \"For Another Game\", \"game\": {\"title_id\": \"") + kGameB + "\", \"title\": \"Moon Harbor\"}}");
    write_file(card + "/mods/othergame/files/data/thing.bin", B.other_file);
    write_file(card + "/mods/nojson/files/x.bin", B.nojson_file);
    write_file(card + "/mods/nofiles/mod.json", "{\"name\": \"Nothing In It\", \"game\": {\"title_id\": \"0400aaaa00001000\"}}");
    write_file(card + "/mods/badjson/mod.json", "{\"name\": \"Broken\", ");
    write_file(card + "/mods/badjson/files/y.bin", "y");
    write_file(card + "/mods/.hidden/mod.json", "{}");
    write_file(card + "/mods/a-file.txt", "not a mod");
    // elsewhere on the card: never to be touched
    write_file(card + "/switch/other.nro", "another program");
    write_file(card + "/evil.txt", "must stay");
}

ModMap mods_of(const std::string& card) {
    ModMap mods;
    modcore::read_mods(card, mods);
    return mods;
}

modcore::Result set_mods(const std::string& card, const std::string& port, const std::vector<std::string>& want, int* calls = nullptr) {
    const modcore::PortView view = modcore::load_port_view(card, port, mods_of(card));
    modcore::Options options;
    options.chunk = 64 * 1024;
    int n = 0;
    int64_t last = 0;
    bool forward = true;
    const modcore::Result r = modcore::apply(card, port, want, view.mods, [&](const modcore::Progress& p) {
        ++n;
        forward = forward && p.done >= last && p.done <= p.total;
        last = p.done;
    }, options);
    check(forward, "progress never goes back, nor past its total");
    if (calls) {
        *calls = n;
    }
    return r;
}

const modcore::StateMod* state_mod(const modcore::State& s, const std::string& id) {
    for (const modcore::StateMod& m : s.mods) {
        if (m.id == id) {
            return &m;
        }
    }
    return nullptr;
}

std::vector<std::string> ids_of(const modcore::State& s) {
    std::vector<std::string> out;
    for (const modcore::StateMod& m : s.mods) {
        out.push_back(m.id);
    }
    return out;
}

bool same_state(const modcore::State& a, const modcore::State& b) {
    if (a.mods.size() != b.mods.size()) {
        return false;
    }
    for (size_t i = 0; i < a.mods.size(); ++i) {
        if (a.mods[i].id != b.mods[i].id || a.mods[i].name != b.mods[i].name || a.mods[i].version != b.mods[i].version || a.mods[i].files != b.mods[i].files) {
            return false;
        }
    }
    return true;
}

bool no_temp_files(const std::string& card) {
    for (const auto& f : tree_of(card)) {
        if (f.first.find(modcore::kTempSuffix) != std::string::npos) {
            return false;
        }
    }
    return true;
}

/// The whole card but the ports' records (those are compared by what they say).
std::map<std::string, std::string> card_but_records(const std::string& card) {
    std::map<std::string, std::string> t = tree_of(card);
    for (auto it = t.begin(); it != t.end();) {
        const bool record = it->first.size() > 10 && it->first.compare(it->first.size() - 10, 10, "/mods.json") == 0;
        it = record ? t.erase(it) : std::next(it);
    }
    return t;
}

typedef std::vector<std::string> Ids;

// ---------------------------------------------------------------- the JSON reader and writer
void test_json() {
    section("json");
    json::Value v;
    std::string e;
    check(json::parse("{\"a\": [1, -2.5, 1e3, true, false, null], \"b\": {\"c\": \"d\"}, \"n\": 12345678901}", v, e), "an object with everything parses");
    check(v.is_object() && v.get("a") && v.get("a")->array.size() == 6, "the array has its six values");
    check(v.get("a")->array[0].is_integer && v.get("a")->array[0].integer == 1, "1 is an integer");
    check(!v.get("a")->array[1].is_integer && v.get("a")->array[1].number == -2.5, "-2.5 is a number");
    check(v.get("a")->array[2].number == 1000.0, "1e3 is 1000");
    check(v.get("a")->array[3].boolean && v.get("a")->array[4].type == json::Value::Bool && v.get("a")->array[5].is_null(), "true, false, null");
    check(v.get("b")->get("c")->string == "d", "a nested object's member");
    check(v.get("n")->integer == 12345678901LL, "an integer past 32 bits");
    check(v.get("zzz") == nullptr, "a member that isn't there is nullptr");
    check(json::parse("\"\\\"\\\\\\/\\b\\f\\n\\r\\t\\u0041\\u00e9\\u20ac\\ud83d\\ude00\"", v, e) && v.string == "\"\\/\b\f\n\r\tA\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80",
          "every escape, \\u in one, two and three bytes, and a surrogate pair");
    check(json::parse("\xEF\xBB\xBF  {\"k\": 1}\n", v, e) && v.get("k")->integer == 1, "a byte order mark is skipped");
    check(json::parse("{\"k\": 1, \"k\": 2}", v, e) && v.get("k")->integer == 2, "of two members of one name, the last");
    check(json::parse("[]", v, e) && v.is_array() && v.array.empty(), "an empty array");
    check(json::parse("{}", v, e) && v.is_object() && v.object.empty(), "an empty object");
    check(json::parse("  -0.5e-2 ", v, e) && v.number == -0.005, "a number with an exponent");
    check(json::parse("\"caf\xC3\xA9\"", v, e) && v.string == "caf\xC3\xA9", "UTF-8 in a string as it is");
    const char* bad[] = {"", "{", "[1,]", "{\"a\":}", "{\"a\" 1}", "{a: 1}", "\"abc", "\"\\x\"", "\"\\u12g4\"", "01", "1.", "tru", "[1] 2", "{\"a\": 1,}",
                         "\"tab\there\"", "nul", "-", "\"\xFF\xFE\"", "\"\xC3\"", "'single'"};
    for (const char* text : bad) {
        check(!json::parse(text, v, e) && !e.empty(), std::string("refused: ") + text);
    }
    std::string deep(200, '[');
    check(!json::parse(deep, v, e), "two hundred open brackets are refused, not followed");

    json::Value top = json::Value::make_object();
    top.add("version", json::Value::make_integer(1));
    json::Value& list = top.add("mods", json::Value::make_array());
    json::Value& one = list.add("", json::Value::make_object());
    one.add("id", json::Value::make_string("a\"b\\c"));
    one.add("name", json::Value::make_string("Caf\xC3\xA9 \xF0\x9F\x98\x80\n"));
    json::Value& files = one.add("files", json::Value::make_object());
    files.add("x/y.bin", json::Value::make_integer(12));
    files.add("z", json::Value::make_integer(-1));
    list.add("", json::Value::make_object());
    const std::string expect = "{\n \"version\": 1,\n \"mods\": [\n  {\n   \"id\": \"a\\\"b\\\\c\",\n   \"name\": \"Caf\\u00e9 \\ud83d\\ude00\\n\",\n"
                               "   \"files\": {\n    \"x/y.bin\": 12,\n    \"z\": -1\n   }\n  },\n  {}\n ]\n}";
    check(json::dump(top) == expect, "written as Python's json.dumps(indent=1) writes it");
    check(json::parse(json::dump(top), v, e) && json::dump(v) == expect, "what is written reads back the same");
    json::Value empty = json::Value::make_object();
    empty.add("mods", json::Value::make_array());
    check(json::dump(empty) == "{\n \"mods\": []\n}", "an empty array is written []");
}

// ---------------------------------------------------------------- reading the card
void test_reading(const std::string& card) {
    section("reading the card");
    build_card(card);
    ModMap mods;
    check(modcore::read_mods(card, mods), "/mods is found");
    check(mods.size() == 7, "seven mods (a hidden folder and a plain file are not mods)");
    check(mods.count("colors") && mods["colors"].name == "Brighter Colors" && mods["colors"].version == "1.2" && mods["colors"].author == "Somebody" &&
              mods["colors"].description == "Every colour a little brighter." && mods["colors"].bad.empty(),
          "a mod's name, version, author and description");
    check(mods["colors"].files.size() == 2 && mods["colors"].files["model/hero.bin"] == 3000 && mods["colors"].files["ui/palette.dat"] == 700 && mods["colors"].size() == 3700,
          "a mod's files and their sizes");
    check(mods["colors"].game_title_id == kGameA && mods["colors"].game_title == "Star Garden", "a mod's game");
    check(mods["shapes"].game_title_id == kGameA, "a title id in capitals is read in lower case");
    check(mods["shapes"].version == "2.0", "a version written as a number is read as Python's str() gives it");
    check(mods["shapes"].files.count("model/deep/a/b/tail.bin") == 1 && mods["shapes"].files["stage/big.pack"] == static_cast<int64_t>(B.shapes_big.size()),
          "files in folders inside folders");
    check(mods["needs"].name == "Caf\xC3\xA9 \"Menu\" \xF0\x9F\x98\x80" && mods["needs"].bad.empty(), "a mod.json with a byte order mark and escapes");
    check(mods["needs"].needs == Ids({"colors", "not-there"}), "what a mod needs");
    check(mods["needs"].description == "line one\nline two", "a description of two lines");
    check(mods["nojson"].bad == "no mod.json", "a mod with no mod.json can't be used");
    check(mods["nofiles"].bad.find("no files") == 0, "a mod with no files can't be used");
    check(mods["badjson"].bad.find("mod.json can't be read") == 0, "a mod whose mod.json is broken can't be used");
    check(mods["nojson"].name == "nojson", "a mod with no name is called by its folder");
    ModMap none;
    check(!modcore::read_mods(card + "/nowhere", none) && none.empty(), "a card with no /mods: said, and no mods");

    std::vector<modcore::Port> ports;
    check(modcore::read_ports(card, ports) && ports.size() == 2, "two ports (a plain file is not a port)");
    check(ports.size() == 2 && ports[0].name == "alpha" && ports[0].title == "Star Garden" && ports[0].title_id == kGameA && ports[0].mods_on == 0, "a port's game, from port.json");
    check(ports.size() == 2 && ports[1].name == "beta" && ports[1].title.empty() && ports[1].title_id.empty(), "a port with no port.json: game not known");
    std::vector<modcore::Port> no_ports;
    check(!modcore::read_ports(card + "/nowhere", no_ports) && no_ports.empty(), "a card with no ports folder: said, and no ports");

    modcore::Port alpha = modcore::read_port(card, "alpha"), beta = modcore::read_port(card, "beta");
    check(modcore::fits(mods["colors"], alpha) == modcore::FitYes, "a mod of the port's game fits (by title id)");
    check(modcore::fits(mods["othergame"], alpha) == modcore::FitNo, "a mod of another game doesn't");
    check(modcore::fits(mods["needs"], alpha) == modcore::FitYes, "a mod that names the game by title alone fits, whatever its capitals and spaces");
    check(modcore::fits(mods["nojson"], alpha) == modcore::FitUnknown, "a mod that names no game: not known");
    check(modcore::fits(mods["othergame"], beta) == modcore::FitUnknown, "a port whose game isn't known: not known");

    const modcore::PortView va = modcore::load_port_view(card, "alpha", mods);
    check(va.mods.size() == 6 && !va.mods.count("othergame") && va.other_game == 1, "the port is offered the mods that aren't for another game");
    check(va.on.empty() && va.dropped.empty(), "none is on");
    check(va.loose == Ids({"hand/notes.txt", "model/extra.bin"}), "the files put by hand are told");
    const modcore::PortView vb = modcore::load_port_view(card, "beta", mods);
    check(vb.mods.size() == 7 && vb.other_game == 0 && vb.loose.empty(), "a port whose game isn't known is offered every mod");

    const auto over = modcore::overlaps({"colors", "needs", "shapes"}, mods);
    check(over.size() == 1 && over.count("model/hero.bin") && over.at("model/hero.bin") == Ids({"colors", "shapes"}), "the one path two mods share, and who has it");
    check(modcore::size_text(812) == "812 B" && modcore::size_text(35328) == "34.5 KB" && modcore::size_text(12897485) == "12.3 MB", "sizes in words");
}

// ---------------------------------------------------------------- putting on and taking off
void test_apply(const std::string& card) {
    section("one mod on");
    build_card(card);
    const std::string base = card + "/nx2-relink/alpha/mods";
    const std::map<std::string, std::string> outside_before = tree_of(card + "/mods");
    int calls = 0;
    modcore::Result r = set_mods(card, "alpha", {"colors"}, &calls);
    check(r.ok && !r.refused && r.copied == 2 && r.removed == 0 && r.bytes == 3700 && r.failed.empty(), "it says it went well: two files copied");
    check(slurp(base + "/model/hero.bin") == B.colors_hero && slurp(base + "/ui/palette.dat") == B.colors_palette, "the mod's files are in the port's mods folder, byte for byte");
    check(slurp(base + "/hand/notes.txt") == B.hand_notes && slurp(base + "/model/extra.bin") == B.hand_extra, "the files put by hand are as they were");
    modcore::State s = modcore::read_state(card, "alpha");
    check(ids_of(s) == Ids({"colors"}) && s.mods[0].name == "Brighter Colors" && s.mods[0].version == "1.2", "the record names the mod");
    check(s.mods[0].files.size() == 2 && s.mods[0].files["model/hero.bin"] == 3000 && s.mods[0].files["ui/palette.dat"] == 700, "the record has its files and sizes");
    json::Value raw;
    std::string e;
    check(json::parse(slurp(card + "/nx2-relink/alpha/mods.json"), raw, e) && raw.get("version") && raw.get("version")->integer == 1, "the record says version 1");
    check(calls >= 4, "progress was said");
    check(no_temp_files(card), "no file is left under a temporary name");
    check(modcore::read_port(card, "alpha").mods_on == 1, "the port says one mod is on");
    check(tree_of(card + "/mods") == outside_before, "/mods is as it was");
    check(slurp(card + "/nx2-relink/alpha/port.json").find("Star Garden") != std::string::npos && slurp(card + "/nx2-relink/alpha/save/slot0.bin").find("a save") == 0,
          "port.json and the port's other files are as they were");

    section("the same again");
    r = set_mods(card, "alpha", {"colors"});
    check(r.ok && r.copied == 0 && r.removed == 0, "asked for what is already there, nothing is copied or removed");

    section("two that overlap");
    r = set_mods(card, "alpha", {"colors", "shapes"}, &calls);
    check(r.ok && r.copied == 3 && r.removed == 0, "the second mod's three files are copied");
    check(slurp(base + "/model/hero.bin") == B.shapes_hero, "the shared file is the later mod's");
    check(slurp(base + "/stage/big.pack") == B.shapes_big && slurp(base + "/model/deep/a/b/tail.bin") == B.shapes_tail && slurp(base + "/ui/palette.dat") == B.colors_palette,
          "every other file is its own mod's (a file of 3 MB among them, copied in pieces)");
    check(calls > 48, "progress was said between the pieces of the large file");
    s = modcore::read_state(card, "alpha");
    check(ids_of(s) == Ids({"colors", "shapes"}), "the record has both, in order");
    check(s.mods.size() == 2 && s.mods[0].files.size() == 1 && s.mods[0].files.count("ui/palette.dat") == 1, "the record gives the earlier mod only the file it still has in use");
    check(s.mods.size() == 2 && s.mods[1].files.size() == 3 && s.mods[1].files.count("model/hero.bin") == 1, "the record gives the shared file to the winner");
    bool said = false;
    for (const std::string& m : r.messages) {
        said = said || (m.find("Overlap") == 0 && m.find("colors, shapes") != std::string::npos && m.find("shapes's") != std::string::npos);
    }
    check(said, "the overlap is said, and who wins it");

    section("the order turned round");
    r = set_mods(card, "alpha", {"shapes", "colors"});
    check(r.ok && r.copied == 1 && r.removed == 0, "one file is copied: the shared one");
    check(slurp(base + "/model/hero.bin") == B.colors_hero, "the shared file is now the other mod's");
    s = modcore::read_state(card, "alpha");
    check(ids_of(s) == Ids({"shapes", "colors"}) && s.mods[0].files.size() == 2 && s.mods[1].files.size() == 2, "the record follows");

    section("the winner taken off");
    set_mods(card, "alpha", {"colors", "shapes"});
    r = set_mods(card, "alpha", {"colors"});
    check(r.ok && r.copied == 1 && r.removed == 2, "the other's file is copied back, the winner's own two removed");
    check(slurp(base + "/model/hero.bin") == B.colors_hero, "the other mod's file is back, byte for byte");
    check(!exists(base + "/stage") && !exists(base + "/model/deep"), "the folders left empty are gone");
    check(exists(base + "/model/extra.bin"), "a folder that still has a file put by hand stays");
    s = modcore::read_state(card, "alpha");
    check(ids_of(s) == Ids({"colors"}) && s.mods[0].files.size() == 2, "the record gives the file back to the mod that is left");
    said = false;
    for (const std::string& m : r.messages) {
        said = said || m == "Taken off: shapes";
    }
    check(said, "which mod was taken off is said");

    section("all taken off");
    set_mods(card, "alpha", {"colors", "shapes", "needs"});
    r = set_mods(card, "alpha", {});
    check(r.ok && r.copied == 0 && r.removed == 5, "five files removed");
    std::map<std::string, std::string> left = tree_of(base);
    check(left.size() == 4 && left["hand/notes.txt"] == B.hand_notes && left["model/extra.bin"] == B.hand_extra && left.count("hand/") && left.count("model/"),
          "the port's mods folder has the files put by hand and nothing else");
    s = modcore::read_state(card, "alpha");
    check(s.mods.empty() && slurp(card + "/nx2-relink/alpha/mods.json") == "{\n \"version\": 1,\n \"mods\": []\n}", "the record says no mod is on");
    check(tree_of(card + "/mods") == outside_before && slurp(card + "/evil.txt") == "must stay" && slurp(card + "/switch/other.nro") == "another program",
          "the rest of the card is as it was");

    section("a need that isn't met");
    r = set_mods(card, "alpha", {"needs"});
    said = false;
    for (const std::string& m : r.messages) {
        said = said || m == "Note: needs needs colors, not-there, which aren't on";
    }
    check(r.ok && said, "a mod put on without what it needs: done, and said");
    r = set_mods(card, "alpha", {"colors", "needs"});
    said = false;
    for (const std::string& m : r.messages) {
        said = said || m == "Note: needs needs not-there, which isn't on";
    }
    check(r.ok && said, "with one of the two it needs on, the other is still said");
    set_mods(card, "alpha", {});

    section("a file put by hand where a mod has one");
    write_file(base + "/ui/palette.dat", "by hand, and in the mod's way");
    modcore::Plan plan;
    std::string error;
    check(modcore::make_plan(card, "alpha", {"colors"}, mods_of(card), plan, error) && plan.loose_hit == Ids({"ui/palette.dat"}), "the plan says which file put by hand a mod replaces");
    check(slurp(base + "/ui/palette.dat") == "by hand, and in the mod's way", "working out a plan changes nothing");
    r = set_mods(card, "alpha", {"colors"});
    said = false;
    for (const std::string& m : r.messages) {
        said = said || (m.find("by hand are replaced") != std::string::npos && m.find("ui/palette.dat") != std::string::npos);
    }
    check(r.ok && said && slurp(base + "/ui/palette.dat") == B.colors_palette, "it is replaced by the mod's, as the PC tool does, and that is said");
    check(slurp(base + "/hand/notes.txt") == B.hand_notes && slurp(base + "/model/extra.bin") == B.hand_extra, "the other files put by hand are as they were");
    set_mods(card, "alpha", {});
}

void test_refusals(const std::string& card) {
    section("what is refused");
    build_card(card);
    const std::map<std::string, std::string> before = tree_of(card);
    const ModMap all = mods_of(card);
    modcore::Plan plan;
    std::string error;
    check(!modcore::make_plan(card, "alpha", {"othergame"}, all, plan, error) && error.find("is for Moon Harbor") != std::string::npos && error.find("Star Garden") != std::string::npos,
          "a mod for another game: refused, and both games named");
    modcore::Result r = modcore::apply(card, "alpha", {"colors", "othergame"}, all);
    check(r.refused && !r.ok && !r.summary.empty(), "apply refuses it too, and says why");
    check(tree_of(card) == before, "and nothing on the card changed, not even for the mod named beside it");
    check(!modcore::make_plan(card, "alpha", {"nojson"}, all, plan, error) && error.find("can't be used: no mod.json") != std::string::npos, "a mod with no mod.json: refused");
    check(!modcore::make_plan(card, "alpha", {"nofiles"}, all, plan, error) && error.find("can't be used: no files") != std::string::npos, "a mod with no files: refused");
    check(!modcore::make_plan(card, "alpha", {"badjson"}, all, plan, error) && error.find("can't be read") != std::string::npos, "a mod with a broken mod.json: refused");
    check(!modcore::make_plan(card, "alpha", {"never-heard-of"}, all, plan, error) && error.find("No mod") == 0, "a mod that isn't there: refused");
    check(!modcore::make_plan(card, "gamma", {"colors"}, all, plan, error) && error.find("No port") == 0, "a port that isn't there: refused");
    check(!modcore::make_plan(card, "../mods", {}, all, plan, error) && !modcore::make_plan(card, "", {}, all, plan, error), "a port's name that is a path: refused");
    check(tree_of(card) == before, "nothing changed");
    check(modcore::make_plan(card, "alpha", {"colors", "colors"}, all, plan, error) && plan.want == Ids({"colors"}), "a mod named twice is on once");

    section("a port whose game isn't known");
    r = set_mods(card, "beta", {"othergame", "colors"});
    check(r.ok && r.copied == 3, "any mod can be put on");
    check(slurp(card + "/nx2-relink/beta/mods/data/thing.bin") == B.other_file && slurp(card + "/nx2-relink/beta/mods/model/hero.bin") == B.colors_hero, "their files are there (the mods folder was made)");
    check(!exists(card + "/nx2-relink/beta/port.json"), "no port.json is written");
    check(!exists(card + "/nx2-relink/alpha/mods.json"), "the other port has no record");
    r = set_mods(card, "beta", {});
    check(r.ok && tree_of(card + "/nx2-relink/beta/mods").empty() && is_dir(card + "/nx2-relink/beta/mods"), "taken off, the mods folder is empty");
}

void test_half_done(const std::string& card) {
    section("after a run that was cut short");
    build_card(card);
    const std::string port = card + "/nx2-relink/alpha", base = port + "/mods";
    set_mods(card, "alpha", {"colors", "shapes"});
    const std::map<std::string, std::string> good = tree_of(base);
    const modcore::State good_state = modcore::read_state(card, "alpha");

    // a file half written under its temporary name, in a folder made for it alone
    write_file(base + "/stage/big.pack" + modcore::kTempSuffix, "half");
    write_file(base + "/never/made/it.bin" + modcore::kTempSuffix, "half");
    write_file(port + "/mods.json" + modcore::kTempSuffix, "{\"version\": 1, \"mo");
    check(modcore::load_port_view(card, "alpha", mods_of(card)).loose == Ids({"hand/notes.txt", "model/extra.bin"}), "a file under a temporary name is not counted as put by hand");
    modcore::Result r = set_mods(card, "alpha", {"colors", "shapes"});
    check(r.ok && tree_of(base) == good && no_temp_files(card), "what was half written is cleared away, and the rest is as it should be");

    // a file the record has and the card hasn't; one that is short; one that is someone else's
    std::remove((base + "/ui/palette.dat").c_str());
    write_file(base + "/stage/big.pack", B.shapes_big.substr(0, 1000));
    r = set_mods(card, "alpha", {"colors", "shapes"});
    check(r.ok && r.copied == 2 && tree_of(base) == good, "a missing file and a short one are copied again");

    // the record written before the copies, and the copies never made
    write_file(port + "/mods.json", "{\"version\": 1, \"mods\": [{\"id\": \"colors\", \"name\": \"Brighter Colors\", \"version\": \"1.2\", \"files\": {\"ui/palette.dat\": -1}},"
                                    " {\"id\": \"shapes\", \"name\": \"Rounder Shapes\", \"version\": \"2.0\", \"files\": {\"model/hero.bin\": -1, \"model/deep/a/b/tail.bin\": 41, \"stage/big.pack\": -1}}]}");
    write_file(base + "/model/hero.bin", B.colors_hero); // (the size of the right one, and not it)
    std::remove((base + "/stage/big.pack").c_str());
    r = set_mods(card, "alpha", {"colors", "shapes"});
    check(r.ok && r.copied == 3 && tree_of(base) == good && same_state(modcore::read_state(card, "alpha"), good_state),
          "files the record has with no size are copied again, whatever is there");
    write_file(port + "/mods.json", "{\"version\": 1, \"mods\": [{\"id\": \"shapes\", \"name\": \"Rounder Shapes\", \"version\": \"2.0\", \"files\": {\"model/hero.bin\": -1, \"stage/big.pack\": -1}}]}");
    r = set_mods(card, "alpha", {});
    check(r.ok && !exists(base + "/model/hero.bin") && !exists(base + "/stage"), "and taken off, they are removed: they were recorded as the mod's before they were copied");
    std::remove((base + "/model/deep/a/b/tail.bin").c_str());
    std::remove((base + "/ui/palette.dat").c_str());

    // the record removed and the new one not yet given its name
    set_mods(card, "alpha", {"colors", "shapes"});
    std::rename((port + "/mods.json").c_str(), (port + "/mods.json" + modcore::kTempSuffix).c_str());
    check(same_state(modcore::read_state(card, "alpha"), good_state), "a record that was whole and waiting for its name is the record");
    r = set_mods(card, "alpha", {"colors"});
    check(r.ok && exists(port + "/mods.json") && no_temp_files(card) && !exists(base + "/stage") && slurp(base + "/model/hero.bin") == B.colors_hero,
          "and the next apply goes by it");

    // files copied by a run that never wrote its record (the PC tool writes it last)
    set_mods(card, "alpha", {});
    write_file(base + "/model/hero.bin", B.shapes_hero);
    write_file(base + "/stage/big.pack", B.shapes_big.substr(0, 70000));
    r = set_mods(card, "alpha", {"shapes"});
    check(r.ok && slurp(base + "/stage/big.pack") == B.shapes_big && slurp(base + "/model/hero.bin") == B.shapes_hero, "files with no record are replaced by the mod's");
    r = set_mods(card, "alpha", {});
    check(r.ok && tree_of(base).size() == 4, "and then they are the mod's to remove");

    section("a copy that fails");
    build_card(card);
    set_mods(card, "alpha", {"colors"});
    ModMap stale = mods_of(card);
    const modcore::PortView view = modcore::load_port_view(card, "alpha", stale);
    write_file(card + "/mods/shapes/files/model/hero.bin", B.shapes_hero.substr(0, 100)); // (changed after it was listed)
    std::remove((card + "/mods/shapes/files/stage/big.pack").c_str());
    r = modcore::apply(card, "alpha", {"colors", "shapes"}, view.mods);
    check(!r.ok && !r.refused && r.failed == Ids({"model/hero.bin", "stage/big.pack"}) && r.copied == 1, "two files fail, the third is copied, and it says so");
    check(r.summary.find("2 file(s) failed") == 0, "the line that sums it up says how many failed");
    check(slurp(base + "/model/hero.bin") == B.colors_hero, "the file that was there is whole: a failed copy is never under the file's name");
    check(!exists(base + "/stage/big.pack") && no_temp_files(card), "nothing half written is left");
    modcore::State s = modcore::read_state(card, "alpha");
    check(ids_of(s) == Ids({"colors", "shapes"}) && state_mod(s, "shapes")->files.at("model/hero.bin") == -1 && state_mod(s, "shapes")->files.at("stage/big.pack") == -1 &&
              state_mod(s, "shapes")->files.at("model/deep/a/b/tail.bin") == 41,
          "the record has the failed files with no size");
    write_file(card + "/mods/shapes/files/model/hero.bin", B.shapes_hero);
    write_file(card + "/mods/shapes/files/stage/big.pack", B.shapes_big);
    r = set_mods(card, "alpha", {"colors", "shapes"});
    check(r.ok && r.copied == 2 && slurp(base + "/model/hero.bin") == B.shapes_hero && slurp(base + "/stage/big.pack") == B.shapes_big, "applied again once the mod is whole, they arrive");

    section("a record that names paths outside the port's mods folder");
    build_card(card);
    write_file(base + "/ui/palette.dat", B.colors_palette);
    write_file(port + "/mods.json", "{\"version\": 1, \"mods\": [{\"id\": \"colors\", \"name\": \"x\", \"version\": \"\", \"files\": "
                                    "{\"../../../evil.txt\": 9, \"../port.json\": 5, \"/evil.txt\": 9, \"ui/../../save/slot0.bin\": 3, \"ui/palette.dat\": 700}}]}");
    r = set_mods(card, "alpha", {});
    check(r.ok && exists(card + "/evil.txt") && exists(port + "/port.json") && exists(port + "/save/slot0.bin"), "they are not removed");
    check(!exists(base + "/ui"), "the path that is inside it is");

    section("records that are not as they should be");
    const char* odd[] = {"", "not json", "[]", "{\"mods\": 3}", "{\"mods\": [1, \"a\", {\"name\": \"no id\"}, {\"id\": 5}]}", "\xEF\xBB\xBF{\"version\": 1, \"mods\": []}"};
    for (const char* text : odd) {
        write_file(port + "/mods.json", text);
        check(modcore::read_state(card, "alpha").mods.empty(), std::string("read as no mod on: ") + text);
    }
    write_file(port + "/mods.json", "{\"mods\": [{\"id\": \"colors\"}, {\"id\": \"shapes\", \"files\": {\"a\": \"x\"}}], \"more\": {\"k\": 1}}");
    check(ids_of(modcore::read_state(card, "alpha")) == Ids({"colors", "shapes"}), "a record with less than usual, and with keys this doesn't know, is still read");
    write_file(port + "/port.json", "\xEF\xBB\xBF{\"title_id\": \"0400BBBB00002000\", \"more\": [1, 2]}");
    check(modcore::read_port(card, "alpha").title_id == kGameB && modcore::read_port(card, "alpha").title.empty(), "a port.json with a byte order mark, no title and keys this doesn't know");
    write_file(port + "/port.json", "[1, 2]");
    check(modcore::read_port(card, "alpha").title_id.empty(), "a port.json that is not an object: game not known");
}

// ---------------------------------------------------------------- mods that are on and not kept in /mods
void test_absent(const std::string& card) {
    section("a mod that is on and not kept on the card");
    build_card(card);
    const std::string port = card + "/nx2-relink/alpha", base = port + "/mods";
    // as the PC tool leaves it when it puts a mod on straight from the PC: after "colors", before nothing
    auto start = [&]() {
        build_card(card);
        set_mods(card, "alpha", {"colors"});
        write_file(base + "/pc/only.bin", B.pc_only);
        write_file(base + "/model/hero.bin", B.pc_hero);
        write_file(port + "/mods.json", "{\"version\": 1, \"mods\": [{\"id\": \"colors\", \"name\": \"Brighter Colors\", \"version\": \"1.2\", \"files\": {\"ui/palette.dat\": 700}},"
                                        " {\"id\": \"from-pc\", \"name\": \"Sent From The PC\", \"version\": \"7\", \"files\": {\"model/hero.bin\": 3100, \"pc/only.bin\": 2222}}]}");
    };
    start();
    modcore::PortView view = modcore::load_port_view(card, "alpha", mods_of(card));
    check(view.on == Ids({"colors", "from-pc"}) && view.dropped.empty(), "it is shown as on, in its place");
    check(view.mods.count("from-pc") && view.mods["from-pc"].absent && view.mods["from-pc"].name == "Sent From The PC" && view.mods["from-pc"].version == "7" &&
              view.mods["from-pc"].files.size() == 2 && view.mods["from-pc"].bad.empty(),
          "with the name, version and files the record has, and marked as not kept");
    check(view.loose == Ids({"hand/notes.txt", "model/extra.bin"}), "its files are not counted as put by hand");
    check(modcore::read_port(card, "alpha").mods_on == 2, "the port counts it");

    modcore::Plan plan;
    std::string error;
    check(modcore::make_plan(card, "alpha", {"colors", "from-pc"}, view.mods, plan, error) && plan.send.empty() && plan.remove.empty() && plan.absent_missing.empty(),
          "left as it is, nothing is to be copied or removed");
    const std::map<std::string, std::string> before = tree_of(base);
    modcore::Result r = set_mods(card, "alpha", {"colors", "from-pc"});
    modcore::State s = modcore::read_state(card, "alpha");
    check(r.ok && r.copied == 0 && r.removed == 0 && tree_of(base) == before, "applied so, its files are untouched");
    check(ids_of(s) == Ids({"colors", "from-pc"}) && state_mod(s, "from-pc")->files.size() == 2 && state_mod(s, "from-pc")->files.at("model/hero.bin") == 3100 &&
              state_mod(s, "from-pc")->name == "Sent From The PC" && state_mod(s, "from-pc")->version == "7" && state_mod(s, "colors")->files.size() == 1,
          "and the record keeps it as it was: it still wins the path it shares");

    // another mod put on after it: that one wins the shared path
    r = set_mods(card, "alpha", {"colors", "from-pc", "shapes"});
    s = modcore::read_state(card, "alpha");
    check(r.ok && slurp(base + "/model/hero.bin") == B.shapes_hero && slurp(base + "/pc/only.bin") == B.pc_only, "a mod put on after it wins the path they share; its other file stays");
    check(state_mod(s, "from-pc")->files.size() == 1 && state_mod(s, "from-pc")->files.count("pc/only.bin") == 1 && state_mod(s, "shapes")->files.count("model/hero.bin") == 1,
          "the record gives that path to the later mod");

    // a mod of the card's put before it: the absent mod's file stays
    start();
    r = set_mods(card, "alpha", {"shapes", "colors", "from-pc"});
    s = modcore::read_state(card, "alpha");
    check(r.ok && slurp(base + "/model/hero.bin") == B.pc_hero, "a mod put on before it doesn't take the path: the file stays as it is");
    check(state_mod(s, "shapes")->files.size() == 2 && !state_mod(s, "shapes")->files.count("model/hero.bin") && state_mod(s, "from-pc")->files.size() == 2, "and the record says whose it is");
    check(slurp(base + "/stage/big.pack") == B.shapes_big, "the earlier mod's own files are copied");

    // moved in the order, before a mod that has the same path
    r = set_mods(card, "alpha", {"from-pc", "shapes", "colors"});
    s = modcore::read_state(card, "alpha");
    check(r.ok && slurp(base + "/model/hero.bin") == B.colors_hero && slurp(base + "/pc/only.bin") == B.pc_only && ids_of(s) == Ids({"from-pc", "shapes", "colors"}) &&
              state_mod(s, "from-pc")->files.size() == 1,
          "moved before the mods that share its path, it loses it to the last of them");

    // taken off: its files are removed, and a path it had won goes back to the mod that lost it
    start();
    r = set_mods(card, "alpha", {"colors"});
    s = modcore::read_state(card, "alpha");
    check(r.ok && r.removed == 1 && r.copied == 1 && !exists(base + "/pc") && slurp(base + "/model/hero.bin") == B.colors_hero,
          "taken off: its own file is removed, and the file of the mod it had beaten is put back");
    check(ids_of(s) == Ids({"colors"}) && state_mod(s, "colors")->files.size() == 2, "the record follows");
    view = modcore::load_port_view(card, "alpha", mods_of(card));
    check(!view.mods.count("from-pc"), "once off it is no longer offered: this card doesn't have it");
    check(!modcore::make_plan(card, "alpha", {"colors", "from-pc"}, view.mods, plan, error), "and can't be put on");

    // it can't be put on for a port it isn't on for, even handed in as if it could
    start();
    view = modcore::load_port_view(card, "alpha", mods_of(card));
    r = modcore::apply(card, "beta", {"from-pc"}, view.mods);
    check(r.refused && r.summary.find("not kept on this card") != std::string::npos && !exists(card + "/nx2-relink/beta/mods"), "it can't be put on for another port");

    // all taken off
    r = set_mods(card, "alpha", {});
    check(r.ok && tree_of(base).size() == 4 && slurp(base + "/hand/notes.txt") == B.hand_notes, "all taken off: only the files put by hand are left");

    // one of its files is gone from the card: said, not an error, and never copied
    start();
    std::remove((base + "/pc/only.bin").c_str());
    view = modcore::load_port_view(card, "alpha", mods_of(card));
    check(modcore::make_plan(card, "alpha", view.on, view.mods, plan, error) && plan.absent_missing == Ids({"pc/only.bin"}) && plan.send.empty(), "a file of its that is missing is said, and not copied");
    r = set_mods(card, "alpha", view.on);
    check(r.ok && !exists(base + "/pc/only.bin") && state_mod(modcore::read_state(card, "alpha"), "from-pc")->files.size() == 2, "the record still has it, for the PC tool to put back");

    // a mod that is on, kept on the card, and unusable now
    start();
    std::remove((card + "/mods/colors/mod.json").c_str());
    view = modcore::load_port_view(card, "alpha", mods_of(card));
    check(view.on == Ids({"from-pc"}) && view.dropped == Ids({"colors"}), "a mod that is on and can't be used any more is set to be taken off");
    r = set_mods(card, "alpha", view.on);
    check(r.ok && !exists(base + "/ui/palette.dat") && slurp(base + "/model/hero.bin") == B.pc_hero, "its files are removed at the next apply; the absent mod's stay");
}

// ---------------------------------------------------------------- with the PC tool
std::string g_repo, g_python, g_work;

bool run_pc(const std::string& card, const std::string& args, std::string* output = nullptr, const std::string& pc_mods = "") {
    const std::string log = g_work + "/pc-tool.log";
    const std::string mods_folder = pc_mods.empty() ? g_work + "/pc-mods-empty" : pc_mods;
    make_dirs(mods_folder);
    const std::string command = "cd \"" + g_repo + "\" && PYTHONDONTWRITEBYTECODE=1 PYTHONIOENCODING=utf-8 NX2_GUI= NX2_PROGRESS= \"" + g_python + "\" nxrelink.py mods " + args +
                                " --target switch2 --mods \"" + mods_folder + "\" --sd \"" + card + "\" > \"" + log + "\" 2>&1";
    const int status = std::system(command.c_str());
    const std::string text = slurp(log);
    if (output) {
        *output = text;
    }
    if (status != 0) {
        std::printf("  the PC tool said (status %d), for: mods %s\n%s\n", status, args.c_str(), text.c_str());
    }
    return status == 0;
}

void compare_cards(const std::string& a, const std::string& b, const std::string& when) {
    check(card_but_records(a) == card_but_records(b), when + ": the two cards have the same files, byte for byte");
    check(same_state(modcore::read_state(a, "alpha"), modcore::read_state(b, "alpha")), when + ": the two records say the same");
    check(no_temp_files(a) && no_temp_files(b), when + ": no temporary file");
}

void test_with_pc_tool() {
    section("with the PC tool (tools/mods.py)");
    const std::string a = g_work + "/card-a", b = g_work + "/card-b";
    build_card(a);
    build_card(b);
    std::string out;

    // on a card this program never touched, the PC tool lists what this program lists
    check(run_pc(a, "list alpha", &out) && out.find("[ ] colors: Brighter Colors 1.2") != std::string::npos && out.find("NOT for this port's game") != std::string::npos,
          "the PC tool lists the pretend card");

    // 1. this program on card A, the PC tool on card B
    check(set_mods(a, "alpha", {"colors", "shapes"}).ok, "step 1 here");
    check(run_pc(b, "set alpha colors shapes"), "step 1 by the PC tool");
    compare_cards(a, b, "step 1");
    check(run_pc(a, "list alpha", &out) && out.find("[1] colors:") != std::string::npos && out.find("[2] shapes:") != std::string::npos &&
              out.find("2 file(s) in the port's mods folder that no mod put there") != std::string::npos,
          "the PC tool reads this program's record: both on, in order, and the two files put by hand");
    check(run_pc(a, "set alpha colors shapes", &out) && out.find("0 file(s) to copy") != std::string::npos && out.find("0 to remove") != std::string::npos,
          "the PC tool finds nothing to do after this program");
    modcore::Plan plan;
    std::string error;
    check(modcore::make_plan(b, "alpha", {"colors", "shapes"}, mods_of(b), plan, error) && plan.send.empty() && plan.remove.empty() && plan.loose_hit.empty(),
          "this program finds nothing to do after the PC tool");
    compare_cards(a, b, "step 1, each checked by the other");

    // 2. the other way round, on what the other left: the order changed, a third mod
    check(run_pc(a, "set alpha shapes colors needs"), "step 2 by the PC tool, on this program's card");
    check(set_mods(b, "alpha", {"shapes", "colors", "needs"}).ok, "step 2 here, on the PC tool's card");
    compare_cards(a, b, "step 2");
    check(slurp(a + "/nx2-relink/alpha/mods/model/hero.bin") == B.colors_hero, "step 2: the shared file is the later mod's");

    // 3. the winner of the shared file taken off
    check(set_mods(a, "alpha", {"shapes", "needs"}).ok, "step 3 here");
    check(run_pc(b, "remove alpha colors"), "step 3 by the PC tool");
    compare_cards(a, b, "step 3");
    check(slurp(b + "/nx2-relink/alpha/mods/model/hero.bin") == B.shapes_hero && slurp(a + "/nx2-relink/alpha/mods/model/hero.bin") == B.shapes_hero, "step 3: the other's file is back on both");

    // 4. "apply" on the PC adds at the end; here the same list
    check(run_pc(a, "apply alpha colors"), "step 4 by the PC tool");
    check(set_mods(b, "alpha", {"shapes", "needs", "colors"}).ok, "step 4 here");
    compare_cards(a, b, "step 4");

    // 5. a record this program wrote with a file that failed (no size): the PC tool copies it
    {
        ModMap stale = mods_of(a);
        const modcore::PortView view = modcore::load_port_view(a, "alpha", stale);
        std::remove((a + "/mods/shapes/files/stage/big.pack").c_str());
        std::remove((a + "/nx2-relink/alpha/mods/stage/big.pack").c_str());
        const modcore::Result r = modcore::apply(a, "alpha", {"shapes", "needs", "colors"}, view.mods);
        check(!r.ok && r.failed == Ids({"stage/big.pack"}), "step 5: a copy made to fail here");
        write_file(a + "/mods/shapes/files/stage/big.pack", B.shapes_big);
        check(run_pc(a, "set alpha shapes needs colors", &out) && out.find("1 file(s) to copy") != std::string::npos, "step 5: the PC tool copies the one file this program recorded as not whole");
        compare_cards(a, b, "step 5");
    }

    // 6. all off
    check(run_pc(a, "remove alpha --all"), "step 6 by the PC tool");
    check(set_mods(b, "alpha", {}).ok, "step 6 here");
    compare_cards(a, b, "step 6");
    check(tree_of(a + "/nx2-relink/alpha/mods").size() == 4, "step 6: only the files put by hand are left");

    // 7. a mod the PC tool puts on straight from the PC: on, and not in the card's /mods
    const std::string pc_mods = g_work + "/pc-mods";
    remove_tree(pc_mods);
    write_file(pc_mods + "/from-pc/mod.json", "{\"name\": \"Sent From The PC\", \"game\": {\"title_id\": \"0400aaaa00001000\"}, \"version\": \"7\"}");
    write_file(pc_mods + "/from-pc/files/model/hero.bin", B.pc_hero);
    write_file(pc_mods + "/from-pc/files/pc/only.bin", B.pc_only);
    const std::string mods_dir = a + "/nx2-relink/alpha/mods";
    check(run_pc(a, "set alpha colors from-pc", &out, pc_mods) && !exists(a + "/mods/from-pc"), "step 7: the PC tool puts a mod on straight from the PC");
    modcore::PortView view = modcore::load_port_view(a, "alpha", mods_of(a));
    check(view.on == Ids({"colors", "from-pc"}) && view.mods.count("from-pc") && view.mods["from-pc"].absent && view.mods["from-pc"].files.size() == 2,
          "step 7: here it is on, and marked as not kept on the card");
    check(set_mods(a, "alpha", {"shapes", "colors", "from-pc"}).ok && slurp(mods_dir + "/model/hero.bin") == B.pc_hero && slurp(mods_dir + "/pc/only.bin") == B.pc_only,
          "step 7: another mod put on before it here; its files stay");
    check(run_pc(a, "list alpha", &out, pc_mods) && out.find("[1] shapes:") != std::string::npos && out.find("[2] colors:") != std::string::npos && out.find("[3] from-pc:") != std::string::npos,
          "step 7: the PC tool reads the record this program wrote with it");
    check(run_pc(a, "set alpha shapes colors from-pc", &out, pc_mods) && out.find("0 file(s) to copy") != std::string::npos && out.find("0 to remove") != std::string::npos,
          "step 7: and finds nothing to do");
    check(set_mods(a, "alpha", {"shapes", "colors"}).ok && !exists(mods_dir + "/pc") && slurp(mods_dir + "/model/hero.bin") == B.colors_hero,
          "step 7: taken off here: its file removed, the file it had beaten put back");
    check(run_pc(b, "set alpha shapes colors"), "step 7: the same two by the PC tool on the other card");
    compare_cards(a, b, "step 7");
    check(run_pc(a, "remove alpha --all") && set_mods(b, "alpha", {}).ok, "step 8: all off, each by the other");
    compare_cards(a, b, "step 8");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: test_core <work folder> [--repo <nx2-recomp folder> --python <python>]\n");
        return 2;
    }
    g_work = argv[1];
    if (g_work[0] != '/' && g_work.find(':') == std::string::npos) {
        char here[2048];
        if (getcwd(here, sizeof here)) {
            g_work = std::string(here) + "/" + g_work; // (the PC tool is run from its own folder)
        }
    }
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--repo")) {
            g_repo = argv[i + 1];
        } else if (!std::strcmp(argv[i], "--python")) {
            g_python = argv[i + 1];
        }
    }
    remove_tree(g_work);
    make_dirs(g_work);
    const std::string card = g_work + "/card";

    test_json();
    test_reading(card);
    test_apply(card);
    test_refusals(card);
    test_half_done(card);
    test_absent(card);
    const int core_checks = g_checks, core_failed = g_failed;
    bool with_pc = false;
    if (!g_repo.empty() && !g_python.empty()) {
        with_pc = true;
        test_with_pc_tool();
    }
    std::printf("\ncore: %d checks, %d failed\n", core_checks, core_failed);
    if (with_pc) {
        std::printf("with the PC tool: %d checks, %d failed\n", g_checks - core_checks, g_failed - core_failed);
    } else {
        std::printf("with the PC tool: NOT RUN (give --repo and --python)\n");
    }
    std::printf("%s: %d of %d checks passed\n", g_failed ? "FAILED" : "PASSED", g_checks - g_failed, g_checks);
    return g_failed ? 1 : 0;
}
