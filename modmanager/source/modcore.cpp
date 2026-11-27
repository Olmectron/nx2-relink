#include "modcore.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <set>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "json.h"

namespace modcore {

const char* const kToolFolder = "nx2-relink";
const char* const kModsFolder = "mods";
const char* const kTempSuffix = ".nx2mods-tmp";

namespace {

const char* const kState = "mods.json";
const size_t kSmallFileMost = 1 << 20;   // a mod.json, port.json or mods.json larger than this is not read past it
const int kDeepest = 48;                 // folders inside folders, at most

bool ends_with(const std::string& s, const char* tail) {
    const size_t n = std::strlen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

std::string lower_ascii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

std::string strip(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || (s[a] >= '\t' && s[a] <= '\r'))) {
        ++a;
    }
    while (b > a && (s[b - 1] == ' ' || (s[b - 1] >= '\t' && s[b - 1] <= '\r'))) {
        --b;
    }
    return s.substr(a, b - a);
}

std::string join_names(const std::vector<std::string>& names, size_t most = 0) {
    std::string out;
    for (size_t i = 0; i < names.size(); ++i) {
        if (most && i == most) {
            out += " ...";
            break;
        }
        out += (i ? ", " : "") + names[i];
    }
    return out;
}

std::string count_text(size_t n, const char* thing) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%lu %s", static_cast<unsigned long>(n), thing);
    return buf;
}

bool is_dir(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/// A file's size, or -1 when it isn't there (or is a folder).
int64_t file_size(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) {
        return -1;
    }
    return static_cast<int64_t>(st.st_size);
}

/// The names in a folder, sorted, each said to be a folder or not. False when it can't be opened.
bool list_dir(const std::string& dir, std::vector<std::pair<std::string, bool>>& out) {
    out.clear();
    DIR* d = opendir(dir.c_str());
    if (!d) {
        return false;
    }
    std::vector<std::string> names;
    while (const dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (name != "." && name != "..") {
            names.push_back(name);
        }
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    for (const std::string& name : names) {
        out.emplace_back(name, is_dir(dir + "/" + name));
    }
    return true;
}

/// Every file under a folder: path under it -> size.
void walk(const std::string& dir, const std::string& prefix, std::map<std::string, int64_t>& out, int depth = 0) {
    std::vector<std::pair<std::string, bool>> entries;
    if (depth > kDeepest || !list_dir(dir, entries)) {
        return;
    }
    for (const auto& e : entries) {
        const std::string full = dir + "/" + e.first;
        if (e.second) {
            walk(full, prefix + e.first + "/", out, depth + 1);
        } else {
            const int64_t n = file_size(full);
            if (n >= 0) {
                out[prefix + e.first] = n;
            }
        }
    }
}

/// A small file whole; false when it isn't there.
bool read_small(const std::string& path, std::string& out) {
    out.clear();
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    char buf[4096];
    size_t n;
    while (out.size() < kSmallFileMost && (n = std::fread(buf, 1, sizeof buf, f)) > 0) {
        out.append(buf, n);
    }
    std::fclose(f);
    return true;
}

/// rename, for a card whose file system refuses to rename onto a file that is there.
bool move_over(const std::string& from, const std::string& to) {
    if (std::rename(from.c_str(), to.c_str()) == 0) {
        return true;
    }
    std::remove(to.c_str());
    return std::rename(from.c_str(), to.c_str()) == 0;
}

/// A path the record names is used only when it is a plain path under the port's mods folder.
bool safe_rel(const std::string& path) {
    if (path.empty() || path[0] == '/' || path.find('\\') != std::string::npos || path.find(':') != std::string::npos) {
        return false;
    }
    size_t at = 0;
    while (at <= path.size()) {
        const size_t slash = std::min(path.find('/', at), path.size());
        const std::string part = path.substr(at, slash - at);
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
        at = slash + 1;
    }
    return true;
}

std::string parent_of(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

/// Every folder a file's path goes through under `base`, made (and `base` itself).
void make_dirs(const std::string& base, const std::string& rel) {
    mkdir(base.c_str(), 0777);
    size_t at = 0;
    for (;;) {
        const size_t slash = rel.find('/', at);
        if (slash == std::string::npos) {
            break;
        }
        mkdir((base + "/" + rel.substr(0, slash)).c_str(), 0777);
        at = slash + 1;
    }
}

/// The folders a removed file was in, removed where they are left empty (never `base` itself).
void prune(const std::string& base, const std::string& rel) {
    std::string d = parent_of(rel);
    std::vector<std::pair<std::string, bool>> entries;
    while (!d.empty()) {
        const std::string full = base + "/" + d;
        if (!list_dir(full, entries) || !entries.empty() || rmdir(full.c_str()) != 0) {
            break;
        }
        d = parent_of(d);
    }
}

/// What Python's str(value or fallback) gives for the values a mod.json has.
std::string text_or(const json::Value* v, const std::string& fallback) {
    if (!v) {
        return fallback;
    }
    char buf[64];
    switch (v->type) {
    case json::Value::String:
        return v->string.empty() ? fallback : v->string;
    case json::Value::Bool:
        return v->boolean ? "True" : fallback;
    case json::Value::Number:
        if (v->number == 0.0) {
            return fallback;
        }
        if (v->is_integer) {
            std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v->integer));
            return buf;
        }
        for (int digits = 1; digits <= 17; ++digits) {
            std::snprintf(buf, sizeof buf, "%.*g", digits, v->number);
            if (std::strtod(buf, nullptr) == v->number) {
                break;
            }
        }
        if (!std::strpbrk(buf, ".eni")) {
            std::strcat(buf, ".0");
        }
        return buf;
    default:
        return fallback; // (null; a list or an object is not a name)
    }
}

/// A mod as this keeps it, from its mod.json's bytes (nullptr: there is none) and its files.
Mod describe(const std::string& id, const std::string* raw, std::map<std::string, int64_t> files) {
    Mod m;
    m.id = id;
    m.name = id;
    m.files = std::move(files);
    if (!raw) {
        m.bad = "no mod.json";
    } else {
        json::Value info;
        std::string error;
        if (!json::parse(*raw, info, error)) {
            m.bad = "mod.json can't be read (" + error + ")";
        } else if (!info.is_object()) {
            m.bad = "mod.json can't be read (it is not an object)";
        } else {
            m.name = text_or(info.get("name"), m.name);
            m.version = text_or(info.get("version"), "");
            m.author = text_or(info.get("author"), "");
            m.description = text_or(info.get("description"), "");
            if (const json::Value* game = info.get("game")) {
                if (game->is_object()) {
                    m.game_title_id = lower_ascii(text_or(game->get("title_id"), ""));
                    m.game_title = text_or(game->get("title"), "");
                } else {
                    m.game_title = text_or(game, "");
                }
            }
            if (const json::Value* needs = info.get("requires")) {
                if (needs->is_array()) {
                    for (const json::Value& x : needs->array) {
                        const std::string s = text_or(&x, "");
                        if (!s.empty()) {
                            m.needs.push_back(s);
                        }
                    }
                }
            }
        }
    }
    if (m.files.empty() && m.bad.empty()) {
        m.bad = "no files (they go in its folder \"files\", at the paths they have in the game)";
    }
    return m;
}

std::string port_dir(const std::string& root, const std::string& port) {
    return card_path(root, std::string(kToolFolder) + "/" + port);
}

bool parse_state(const std::string& raw, State& out) {
    json::Value got;
    std::string error;
    if (!json::parse(raw, got, error)) {
        return false;
    }
    const json::Value* mods = got.get("mods");
    if (!mods || !mods->is_array()) {
        return false;
    }
    out.mods.clear();
    for (const json::Value& m : mods->array) {
        const json::Value* id = m.get("id");
        if (!id || !id->is_string() || id->string.empty()) {
            continue;
        }
        StateMod s;
        s.id = id->string;
        s.name = text_or(m.get("name"), s.id);
        s.version = text_or(m.get("version"), "");
        if (const json::Value* files = m.get("files")) {
            for (const auto& f : files->object) {
                const json::Value& n = f.second;
                s.files[f.first] = n.is_number() ? (n.is_integer ? n.integer : static_cast<int64_t>(n.number)) : -1;
            }
        }
        out.mods.push_back(std::move(s));
    }
    return true;
}

bool write_state(const std::string& root, const std::string& port, const State& state) {
    json::Value top = json::Value::make_object();
    top.add("version", json::Value::make_integer(1));
    json::Value& list = top.add("mods", json::Value::make_array());
    for (const StateMod& m : state.mods) {
        json::Value& one = list.add("", json::Value::make_object());
        one.add("id", json::Value::make_string(m.id));
        one.add("name", json::Value::make_string(m.name));
        one.add("version", json::Value::make_string(m.version));
        json::Value& files = one.add("files", json::Value::make_object());
        for (const auto& f : m.files) {
            files.add(f.first, json::Value::make_integer(f.second));
        }
    }
    const std::string text = json::dump(top);
    const std::string path = port_dir(root, port) + "/" + kState;
    const std::string temp = path + kTempSuffix;
    std::FILE* f = std::fopen(temp.c_str(), "wb");
    if (!f) {
        return false;
    }
    const bool written = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    const bool closed = std::fclose(f) == 0;
    if (!written || !closed || file_size(temp) != static_cast<int64_t>(text.size())) {
        std::remove(temp.c_str());
        return false;
    }
    return move_over(temp, path);
}

/// FNV-1a, 64 bits: to tell that what was read back is what was written.
uint64_t hash_more(uint64_t h, const unsigned char* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        h = (h ^ p[i]) * 1099511628211ULL;
    }
    return h;
}
const uint64_t kHashStart = 14695981039346656037ULL;

struct Copier {
    std::vector<unsigned char> buffer;
    const Options* options = nullptr;
    const ProgressFn* progress = nullptr;
    Progress said;

    void say() {
        if (progress && *progress) {
            (*progress)(said);
        }
    }

    /// One file copied whole to `to`, or not at all: it is written beside it under a temporary
    /// name and renamed when it has every byte. `expect` is the size the mod's listing gave.
    bool copy(const std::string& from, const std::string& to, int64_t expect, std::string& why) {
        const std::string temp = to + kTempSuffix;
        std::FILE* in = std::fopen(from.c_str(), "rb");
        if (!in) {
            why = std::string("it can't be read from the mod's folder (") + std::strerror(errno) + ")";
            return false;
        }
        std::FILE* out = std::fopen(temp.c_str(), "wb");
        if (!out) {
            why = std::string("the card can't be written to (") + std::strerror(errno) + ")";
            std::fclose(in);
            return false;
        }
        const int64_t before = said.done;
        int64_t total = 0;
        uint64_t wrote_hash = kHashStart;
        bool good = true;
        said.phase = Progress::Copying;
        for (;;) {
            const size_t n = std::fread(buffer.data(), 1, buffer.size(), in);
            if (n == 0) {
                if (std::ferror(in)) {
                    why = "reading it from the mod's folder failed";
                    good = false;
                }
                break;
            }
            if (std::fwrite(buffer.data(), 1, n, out) != n) {
                why = std::string("writing it failed (") + std::strerror(errno) + "): is the card full?";
                good = false;
                break;
            }
            wrote_hash = hash_more(wrote_hash, buffer.data(), n);
            total += static_cast<int64_t>(n);
            said.done = before + std::min(total, expect);
            say();
        }
        std::fclose(in);
        if (std::fclose(out) != 0 && good) {
            why = std::string("writing it failed (") + std::strerror(errno) + "): is the card full?";
            good = false;
        }
        if (good && total != expect) {
            why = "the mod's file changed size while it was copied";
            good = false;
        }
        if (good && file_size(temp) != expect) {
            why = "the copy doesn't have the size of the mod's file";
            good = false;
        }
        if (good && options->read_back) {
            said.phase = Progress::Checking;
            say();
            uint64_t read_hash = kHashStart;
            int64_t read = 0;
            std::FILE* back = std::fopen(temp.c_str(), "rb");
            if (back) {
                size_t n;
                while ((n = std::fread(buffer.data(), 1, buffer.size(), back)) > 0) {
                    read_hash = hash_more(read_hash, buffer.data(), n);
                    read += static_cast<int64_t>(n);
                    say();
                }
                std::fclose(back);
            }
            if (!back || read != expect || read_hash != wrote_hash) {
                why = "read back from the card, the copy is not what was written";
                good = false;
            }
        }
        if (good && !move_over(temp, to)) {
            why = std::string("it couldn't be given its name (") + std::strerror(errno) + ")";
            good = false;
        }
        if (good && file_size(to) != expect) {
            why = "the copy doesn't have the size of the mod's file";
            good = false;
        }
        if (!good) {
            std::remove(temp.c_str());
            said.done = before + expect; // (counted as gone by, so the bar still ends at its end)
            return false;
        }
        said.done = before + expect;
        return true;
    }
};

State new_state(const Plan& plan, const ModMap& mods, const std::set<std::string>& not_whole) {
    State state;
    for (const std::string& mid : plan.want) {
        const Mod& m = mods.at(mid);
        StateMod s;
        s.id = mid;
        s.name = m.name;
        s.version = m.version;
        for (const auto& f : m.files) {
            const auto who = plan.owner.find(f.first);
            if (who != plan.owner.end() && who->second == mid) {
                // A file that isn't whole on the card yet (or failed) is recorded with no size: it
                // is this mod's to remove, and the next apply, here or on the PC, copies it again.
                s.files[f.first] = not_whole.count(f.first) ? -1 : f.second;
            }
        }
        state.mods.push_back(std::move(s));
    }
    return state;
}

} // namespace

int64_t Mod::size() const {
    int64_t n = 0;
    for (const auto& f : files) {
        n += std::max<int64_t>(f.second, 0);
    }
    return n;
}

bool Mod::runs_code() const {
    for (const auto& f : files) {
        std::string p = f.first;
        for (char& c : p) {
            c = c == '\\' ? '/' : (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
        }
        const size_t at = p.find_first_not_of('/');
        if (at != std::string::npos && p.compare(at, 5, "code/") == 0 && p.size() >= 4 && p.compare(p.size() - 4, 4, ".nxh") == 0) {
            return true;
        }
    }
    return false;
}

std::string card_path(const std::string& root, const std::string& rel) {
    std::string out = root;
    while (!out.empty() && (out.back() == '/' || out.back() == '\\')) {
        out.pop_back();
    }
    size_t at = 0;
    while (at < rel.size() && rel[at] == '/') {
        ++at;
    }
    return out + "/" + rel.substr(at);
}

bool read_mods(const std::string& root, ModMap& out) {
    out.clear();
    const std::string folder = card_path(root, kModsFolder);
    std::vector<std::pair<std::string, bool>> entries;
    if (!is_dir(folder) || !list_dir(folder, entries)) {
        return false;
    }
    for (const auto& e : entries) {
        if (!e.second || e.first[0] == '.') {
            continue;
        }
        const std::string base = folder + "/" + e.first;
        std::map<std::string, int64_t> files;
        walk(base + "/files", "", files);
        std::string raw;
        const bool has = read_small(base + "/mod.json", raw);
        out[e.first] = describe(e.first, has ? &raw : nullptr, std::move(files));
    }
    return true;
}

Port read_port(const std::string& root, const std::string& name) {
    Port port;
    port.name = name;
    std::string raw, error;
    json::Value got;
    if (read_small(port_dir(root, name) + "/port.json", raw) && json::parse(raw, got, error) && got.is_object()) {
        port.title = text_or(got.get("title"), "");
        port.title_id = lower_ascii(text_or(got.get("title_id"), ""));
    }
    port.mods_on = static_cast<int>(read_state(root, name).mods.size());
    return port;
}

bool read_ports(const std::string& root, std::vector<Port>& out) {
    out.clear();
    const std::string folder = card_path(root, kToolFolder);
    std::vector<std::pair<std::string, bool>> entries;
    if (!is_dir(folder) || !list_dir(folder, entries)) {
        return false;
    }
    for (const auto& e : entries) {
        if (e.second) {
            out.push_back(read_port(root, e.first));
        }
    }
    return true;
}

State read_state(const std::string& root, const std::string& port) {
    State state;
    const std::string path = port_dir(root, port) + "/" + kState;
    std::string raw;
    if (read_small(path, raw)) {
        if (!raw.empty()) {
            parse_state(raw, state);
        }
        return state;
    }
    // No record, and one that was whole and waiting to be given its name (a run cut short between
    // removing the old and renaming the new): that one is the record.
    if (read_small(path + kTempSuffix, raw)) {
        parse_state(raw, state);
    }
    return state;
}

Fit fits(const Mod& mod, const Port& port) {
    if (!mod.game_title_id.empty() && !port.title_id.empty()) {
        return mod.game_title_id == port.title_id ? FitYes : FitNo;
    }
    const std::string a = lower_ascii(strip(mod.game_title)), b = lower_ascii(strip(port.title));
    if (!a.empty() && !b.empty()) {
        return a == b ? FitYes : FitNo;
    }
    return FitUnknown;
}

std::map<std::string, std::vector<std::string>> overlaps(const std::vector<std::string>& order, const ModMap& mods) {
    std::map<std::string, std::vector<std::string>> seen;
    for (const std::string& mid : order) {
        const auto m = mods.find(mid);
        if (m == mods.end()) {
            continue;
        }
        for (const auto& f : m->second.files) {
            seen[f.first].push_back(mid);
        }
    }
    for (auto it = seen.begin(); it != seen.end();) {
        it = it->second.size() > 1 ? std::next(it) : seen.erase(it);
    }
    return seen;
}

PortView load_port_view(const std::string& root, const std::string& port, const ModMap& card_mods) {
    PortView view;
    view.port = read_port(root, port);
    view.state = read_state(root, port);
    for (const auto& m : card_mods) {
        if (fits(m.second, view.port) == FitNo) {
            ++view.other_game;
        } else {
            view.mods[m.first] = m.second;
        }
    }
    std::set<std::string> seen, owned;
    for (const StateMod& s : view.state.mods) {
        for (const auto& f : s.files) {
            owned.insert(f.first);
        }
        if (!seen.insert(s.id).second) {
            continue;
        }
        const auto kept = card_mods.find(s.id);
        if (kept != card_mods.end()) {
            if (kept->second.bad.empty() && view.mods.count(s.id)) {
                view.on.push_back(s.id);
            } else {
                view.dropped.push_back(s.id);
            }
            continue;
        }
        Mod absent;
        absent.id = s.id;
        absent.name = s.name.empty() ? s.id : s.name;
        absent.version = s.version;
        absent.absent = true;
        for (const auto& f : s.files) {
            if (safe_rel(f.first)) {
                absent.files[f.first] = f.second;
            }
        }
        view.mods[s.id] = absent;
        view.on.push_back(s.id);
    }
    std::map<std::string, int64_t> on_card;
    walk(port_dir(root, port) + "/mods", "", on_card);
    for (const auto& f : on_card) {
        if (!owned.count(f.first) && !ends_with(f.first, kTempSuffix)) {
            view.loose.push_back(f.first);
        }
    }
    return view;
}

bool make_plan(const std::string& root, const std::string& port, const std::vector<std::string>& want_in,
               const ModMap& mods, Plan& plan, std::string& error) {
    plan = Plan();
    plan.port = port;
    if (port.empty() || port.find('/') != std::string::npos || port.find('\\') != std::string::npos || port == "." || port == ".." ||
        !is_dir(port_dir(root, port))) {
        error = "No port \"" + port + "\" on this card (/" + kToolFolder + "/" + port + "/ isn't there).";
        return false;
    }
    const Port identity = read_port(root, port);
    plan.old_state = read_state(root, port);
    std::set<std::string> on_now, taken;
    for (const StateMod& s : plan.old_state.mods) {
        on_now.insert(s.id);
    }
    for (const std::string& mid : want_in) {
        if (!taken.insert(mid).second) {
            continue; // (named twice: once is enough)
        }
        const auto found = mods.find(mid);
        if (found == mods.end()) {
            error = "No mod \"" + mid + "\" on the card (in /" + kModsFolder + "/).";
            return false;
        }
        const Mod& m = found->second;
        if (m.absent) {
            if (!on_now.count(mid)) {
                error = "The mod \"" + mid + "\" is not kept on this card: it can't be put on from here.";
                return false;
            }
        } else if (!m.bad.empty()) {
            error = "The mod \"" + mid + "\" can't be used: " + m.bad + ".";
            return false;
        } else if (fits(m, identity) == FitNo) {
            error = "The mod \"" + mid + "\" is for " + (m.game_title.empty() ? m.game_title_id : m.game_title) + ", and the port \"" + port +
                    "\" is " + (identity.title.empty() ? identity.title_id : identity.title) + ": not put on.";
            return false;
        }
        plan.want.push_back(mid);
    }

    walk(port_dir(root, port) + "/mods", "", plan.on_card);
    for (auto it = plan.on_card.begin(); it != plan.on_card.end();) {
        it = ends_with(it->first, kTempSuffix) ? plan.on_card.erase(it) : std::next(it);
    }
    // what each path should be, and from which mod: the last mod of the order that has it
    for (const std::string& mid : plan.want) {
        for (const auto& f : mods.at(mid).files) {
            plan.owner[f.first] = mid;
        }
    }
    // what the card has now, by the record
    for (const StateMod& s : plan.old_state.mods) {
        for (const auto& f : s.files) {
            if (safe_rel(f.first)) {
                plan.had[f.first] = std::make_pair(s.id, f.second);
            } else {
                plan.unsafe.push_back(f.first);
            }
        }
    }

    std::map<std::vector<std::string>, size_t> shared;
    for (const auto& o : overlaps(plan.want, mods)) {
        ++shared[o.second];
    }
    for (const auto& s : shared) {
        plan.notes.push_back("Overlap: " + count_text(s.second, "file(s)") + " in " + join_names(s.first) + ": the game gets " + s.first.back() + "'s");
    }
    for (const std::string& mid : plan.want) {
        std::vector<std::string> missing;
        for (const std::string& need : mods.at(mid).needs) {
            if (!taken.count(need) || std::find(plan.want.begin(), plan.want.end(), need) == plan.want.end()) {
                missing.push_back(need);
            }
        }
        if (!missing.empty()) {
            plan.notes.push_back("Note: " + mid + " needs " + join_names(missing) + (missing.size() == 1 ? ", which isn't on" : ", which aren't on"));
        }
    }
    for (const auto& o : plan.owner) {
        const std::string& path = o.first;
        const Mod& m = mods.at(o.second);
        const int64_t size = m.files.at(path);
        const auto had = plan.had.find(path);
        const auto there = plan.on_card.find(path);
        if (m.absent) {
            // its file is whatever the record says is there: left as it is, never copied
            if (there == plan.on_card.end() || (size >= 0 && there->second != size)) {
                plan.absent_missing.push_back(path);
            }
            continue;
        }
        if (there != plan.on_card.end() && had == plan.had.end()) {
            plan.loose_hit.push_back(path);
        }
        if (had == plan.had.end() || had->second != std::make_pair(o.second, size) || there == plan.on_card.end() || there->second != size) {
            plan.send.push_back(path);
            plan.bytes += size;
        }
    }
    for (const auto& h : plan.had) {
        if (!plan.owner.count(h.first)) {
            plan.remove.push_back(h.first);
        }
    }
    if (!plan.loose_hit.empty()) {
        plan.notes.push_back("Note: " + count_text(plan.loose_hit.size(), "file(s)") +
                             " put in the port's mods folder by hand are replaced by a mod's: " + join_names(plan.loose_hit, 3));
    }
    if (!plan.absent_missing.empty()) {
        plan.notes.push_back("Note: " + count_text(plan.absent_missing.size(), "file(s)") +
                             " of a mod not kept on this card are not whole, and can't be put back from here: " + join_names(plan.absent_missing, 3));
    }
    if (!plan.unsafe.empty()) {
        plan.notes.push_back("Note: " + count_text(plan.unsafe.size(), "path(s)") + " in the record are not paths inside the port's mods folder: not touched");
    }
    return true;
}

Result apply(const std::string& root, const std::string& port, const std::vector<std::string>& want,
             const ModMap& mods, const ProgressFn& progress, const Options& options) {
    Result result;
    Plan plan;
    if (!make_plan(root, port, want, mods, plan, result.summary)) {
        result.refused = true;
        return result;
    }
    result.messages = plan.notes;
    const std::string base = port_dir(root, port) + "/mods";

    Copier copier;
    copier.options = &options;
    copier.progress = &progress;
    copier.said.total = plan.bytes;

    // What an earlier run that was cut short left half written: never under a file's own name.
    {
        std::map<std::string, int64_t> all;
        walk(base, "", all);
        for (const auto& f : all) {
            if (ends_with(f.first, kTempSuffix) && std::remove((base + "/" + f.first).c_str()) == 0) {
                prune(base, f.first);
            }
        }
    }

    // 1. The files that are nobody's any more.
    std::set<std::string> failed;
    copier.said.phase = Progress::Removing;
    copier.said.count = static_cast<int>(plan.remove.size());
    for (size_t k = 0; k < plan.remove.size(); ++k) {
        const std::string& path = plan.remove[k];
        copier.said.index = static_cast<int>(k) + 1;
        copier.said.path = path;
        copier.say();
        if (plan.on_card.count(path)) {
            if (std::remove((base + "/" + path).c_str()) != 0) {
                result.messages.push_back(path + " couldn't be removed (" + std::strerror(errno) + ")");
                failed.insert(path);
                continue;
            }
            ++result.removed;
        }
        prune(base, path);
    }

    // 2. The record, before a byte is copied: which mods are on, and every file that is to be
    //    theirs. If the run stops after this, each file copied so far is known to be a mod's.
    std::set<std::string> not_whole(plan.send.begin(), plan.send.end());
    copier.said.phase = Progress::Recording;
    copier.said.index = copier.said.count = 0;
    copier.said.path.clear();
    copier.say();
    if (!write_state(root, port, new_state(plan, mods, not_whole))) {
        result.failed.assign(failed.begin(), failed.end());
        result.messages.push_back(std::string("The record (") + kState + ") couldn't be written (" + std::strerror(errno) + ")");
        result.summary = "The port's record couldn't be written: no file was copied. Is the card full or locked?";
        return result;
    }

    // 3. The files to copy, each from the mod that is to have that path.
    copier.buffer.resize(std::max<size_t>(options.chunk, 4096));
    copier.said.count = static_cast<int>(plan.send.size());
    not_whole.clear();
    for (size_t k = 0; k < plan.send.size(); ++k) {
        const std::string& path = plan.send[k];
        const std::string& mid = plan.owner.at(path);
        const int64_t size = mods.at(mid).files.at(path);
        copier.said.phase = Progress::Copying;
        copier.said.index = static_cast<int>(k) + 1;
        copier.said.path = path;
        copier.say();
        make_dirs(base, path);
        std::string why;
        if (copier.copy(card_path(root, std::string(kModsFolder) + "/" + mid + "/files/" + path), base + "/" + path, size, why)) {
            ++result.copied;
            result.bytes += size;
        } else {
            result.messages.push_back(path + " was NOT copied: " + why);
            failed.insert(path);
            not_whole.insert(path);
        }
        copier.say();
    }

    // 4. The record as it ended: every mod that is on, with the files of it that are the ones in use.
    copier.said.phase = Progress::Recording;
    copier.said.path.clear();
    copier.say();
    const bool recorded = write_state(root, port, new_state(plan, mods, not_whole));
    if (!recorded) {
        result.messages.push_back(std::string("The record (") + kState + ") couldn't be written (" + std::strerror(errno) + ")");
    }
    std::vector<std::string> gone;
    for (const StateMod& s : plan.old_state.mods) {
        if (std::find(plan.want.begin(), plan.want.end(), s.id) == plan.want.end() && std::find(gone.begin(), gone.end(), s.id) == gone.end()) {
            gone.push_back(s.id);
        }
    }
    if (!gone.empty()) {
        result.messages.push_back("Taken off: " + join_names(gone));
    }
    result.failed.assign(failed.begin(), failed.end());
    if (!failed.empty()) {
        result.summary = count_text(failed.size(), "file(s) failed") + ": apply again. Until then the port may have a mod half on.";
    } else if (!recorded) {
        result.summary = "The files are in place, but the port's record couldn't be written: apply again.";
    } else {
        result.ok = true;
        char buf[160];
        std::snprintf(buf, sizeof buf, " (%d copied, %s; %d removed)", result.copied, size_text(result.bytes).c_str(), result.removed);
        result.summary = (plan.want.empty() ? std::string("Done: no mod is on; the game's own files are used.")
                                            : "Done: " + count_text(plan.want.size(), "mod(s) on") + "; the port uses them from its next start.") + buf;
    }
    return result;
}

std::string size_text(int64_t bytes) {
    char buf[48];
    if (bytes < 1024) {
        std::snprintf(buf, sizeof buf, "%d B", static_cast<int>(std::max<int64_t>(bytes, 0)));
    } else if (bytes < 1024 * 1024) {
        std::snprintf(buf, sizeof buf, "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else if (bytes < 1024LL * 1024 * 1024) {
        std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else {
        std::snprintf(buf, sizeof buf, "%.2f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    }
    return buf;
}

} // namespace modcore
