#include "ui.h"

#include <algorithm>
#include <cstdio>
#include <set>

namespace ui {

const char* const kVersion = "0.1.0";

namespace {

using gfx::rgb;

const uint32_t kBg = rgb(18, 22, 30);
const uint32_t kBar = rgb(30, 38, 52);
const uint32_t kText = rgb(232, 236, 242);
const uint32_t kDim = rgb(140, 150, 166);
const uint32_t kFaint = rgb(70, 80, 96);
const uint32_t kAccent = rgb(64, 200, 184);
const uint32_t kSelected = rgb(40, 64, 88);
const uint32_t kWarn = rgb(240, 184, 76);
const uint32_t kBad = rgb(244, 108, 96);
const uint32_t kGood = rgb(120, 214, 128);

const int kMargin = 32;
const int kTitleH = 56;
const int kFooterH = 48;
const int kRowH = 34;
const int kListTop = 98;
const int kModRows = 12;    // rows of mods that fit above the highlighted mod's details
const int kPortRows = 15;

std::string number(long long n) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%lld", n);
    return buf;
}

std::string joined(const std::vector<std::string>& names) {
    std::string out;
    for (size_t i = 0; i < names.size(); ++i) {
        out += (i ? ", " : "") + names[i];
    }
    return out;
}

void move_cursor(int& cursor, int count, const Input& in) {
    if (count <= 0) {
        cursor = 0;
        return;
    }
    if (in.up) {
        cursor = cursor > 0 ? cursor - 1 : count - 1;
    }
    if (in.down) {
        cursor = cursor + 1 < count ? cursor + 1 : 0;
    }
}

void keep_in_view(int cursor, int& scroll, int visible, int count) {
    if (cursor < scroll) {
        scroll = cursor;
    }
    if (cursor >= scroll + visible) {
        scroll = cursor - visible + 1;
    }
    scroll = std::max(0, std::min(scroll, std::max(0, count - visible)));
}

void scrollbar(gfx::Canvas& c, int top, int visible, int scroll, int count) {
    if (count <= visible) {
        return;
    }
    const int x = kScreenW - 20, h = visible * kRowH;
    c.fill(x, top, 6, h, kFaint);
    const int thumb = std::max(16, h * visible / count);
    c.fill(x, top + (h - thumb) * scroll / (count - visible), 6, thumb, kAccent);
}

} // namespace

App::App(const std::string& root) : root_(root) {
    read_card();
}

void App::read_card() {
    has_mods_folder_ = modcore::read_mods(root_, card_mods_);
    has_ports_folder_ = modcore::read_ports(root_, ports_);
    port_cursor_ = std::max(0, std::min(port_cursor_, static_cast<int>(ports_.size()) - 1));
    keep_in_view(port_cursor_, port_scroll_, kPortRows, static_cast<int>(ports_.size()));
}

bool App::is_on(const std::string& id) const {
    return std::find(on_.begin(), on_.end(), id) != on_.end();
}

void App::open_port(const std::string& name) {
    view_ = modcore::load_port_view(root_, name, card_mods_);
    on_ = view_.on;
    cursor_ = scroll_ = 0;
    message_.clear();
    rebuild_rows("");
    screen_ = Mods;
}

/// The rows again: the ticked mods in their order, then the others by name of folder, the ones
/// that can't be used last. The highlight stays on the mod named.
void App::rebuild_rows(const std::string& keep_on) {
    rows_ = on_;
    std::vector<std::string> unusable;
    for (const auto& m : view_.mods) {
        if (is_on(m.first)) {
            continue;
        }
        (m.second.bad.empty() ? rows_ : unusable).push_back(m.first);
    }
    rows_.insert(rows_.end(), unusable.begin(), unusable.end());
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i] == keep_on) {
            cursor_ = static_cast<int>(i);
        }
    }
    cursor_ = std::max(0, std::min(cursor_, static_cast<int>(rows_.size()) - 1));
    scroll_to_cursor();

    // what is said of each mod
    notes_.clear();
    std::vector<std::string> usable = on_;
    for (const auto& m : view_.mods) {
        if (m.second.bad.empty() && !is_on(m.first)) {
            usable.push_back(m.first);
        }
    }
    std::map<std::string, std::set<std::string>> shares;
    for (const auto& o : modcore::overlaps(usable, view_.mods)) {
        for (const std::string& a : o.second) {
            for (const std::string& b : o.second) {
                if (a != b) {
                    shares[a].insert(b);
                }
            }
        }
    }
    std::map<std::string, int> lost;
    for (const auto& o : modcore::overlaps(on_, view_.mods)) {
        for (const std::string& a : o.second) {
            if (a != o.second.back()) {
                ++lost[a];
            }
        }
    }
    for (const auto& m : view_.mods) {
        const modcore::Mod& mod = m.second;
        Note note;
        auto add = [&note](const std::string& brief, const std::string& full) {
            note.brief += (note.brief.empty() ? "" : "; ") + brief;
            note.full += (note.full.empty() ? "" : "; ") + full;
        };
        if (mod.runs_code()) {
            add("runs code", "runs code in the game: it has a hook file (Code/*.nxh), made for one version of one game; the port's log says whether it used it");
        }
        if (!mod.bad.empty()) {
            add("can't be used", "can't be used: " + mod.bad);
        }
        if (mod.absent) {
            add("not kept on this card", "not kept on this card: it can only be taken off (the PC tool put it on, and only it can put it on again)");
        }
        std::vector<std::string> missing;
        for (const std::string& need : mod.needs) {
            if (!is_on(need)) {
                missing.push_back(need);
            }
        }
        if (!missing.empty()) {
            add("needs " + joined(missing), "needs " + joined(missing) + (missing.size() == 1 ? ", which isn't on" : ", which aren't on"));
        }
        if (shares.count(m.first)) {
            const std::string who = joined(std::vector<std::string>(shares[m.first].begin(), shares[m.first].end()));
            add("overlaps " + who, "overlaps with " + who + ": they have files in common, and the game gets those of the one later in the order");
        }
        if (lost.count(m.first)) {
            add(number(lost[m.first]) + " file(s) overridden", number(lost[m.first]) + " of its files are not the ones in use: a mod after it has the same");
        }
        notes_[m.first] = note;
    }
}

void App::scroll_to_cursor() {
    keep_in_view(cursor_, scroll_, kModRows, static_cast<int>(rows_.size()));
}

/// Whether the mods ticked are not the ones the port's record has, in its order.
bool App::changed() const {
    std::vector<std::string> recorded;
    for (const modcore::StateMod& m : view_.state.mods) {
        if (std::find(recorded.begin(), recorded.end(), m.id) == recorded.end()) {
            recorded.push_back(m.id);
        }
    }
    return recorded != on_;
}

void App::run_apply(const std::vector<std::string>& want) {
    screen_ = Applying;
    ask_ = AskNothing;
    message_.clear();
    progress_ = modcore::Progress();
    last_shown_ = 0;
    if (present_) {
        present_();
    }
    modcore::Progress::Phase phase = progress_.phase;
    int index = -1;
    result_ = modcore::apply(root_, view_.port.name, want, view_.mods, [&](const modcore::Progress& p) {
        progress_ = p;
        if (!present_) {
            return;
        }
        // a new file or step is always shown; within a file, a few times a second
        const uint64_t now = clock_ ? clock_() : 0;
        if (clock_ && p.phase == phase && p.index == index && now - last_shown_ < 50) {
            return;
        }
        phase = p.phase;
        index = p.index;
        last_shown_ = now;
        present_();
    });
    const std::string here = rows_.empty() ? std::string() : rows_[static_cast<size_t>(cursor_)];
    view_ = modcore::load_port_view(root_, view_.port.name, card_mods_);
    on_ = view_.on;
    rebuild_rows(here);
    screen_ = Outcome;
}

void App::update(const Input& in) {
    switch (screen_) {
    case Ports:
        update_ports(in);
        break;
    case Mods:
        update_mods(in);
        break;
    case Applying:
        break;
    case Outcome:
        if (in.plus) {
            done_ = true;
        } else if (in.a || in.b) {
            screen_ = Mods;
        }
        break;
    }
}

void App::update_ports(const Input& in) {
    if (in.plus) {
        done_ = true;
        return;
    }
    if (in.y) {
        read_card();
        return;
    }
    move_cursor(port_cursor_, static_cast<int>(ports_.size()), in);
    keep_in_view(port_cursor_, port_scroll_, kPortRows, static_cast<int>(ports_.size()));
    if (in.a && !ports_.empty()) {
        open_port(ports_[static_cast<size_t>(port_cursor_)].name);
    }
}

void App::update_mods(const Input& in) {
    if (ask_ != AskNothing) {
        const Ask asked = ask_;
        if (in.b) {
            ask_ = AskNothing;
        } else if (in.a) {
            ask_ = AskNothing;
            if (asked == AskAllOff) {
                run_apply(std::vector<std::string>());
            } else if (asked == AskLeavePort) {
                screen_ = Ports;
                read_card();
            } else {
                done_ = true;
            }
        }
        return;
    }
    if (in.up || in.down || in.a || in.b || in.x || in.y || in.l || in.r || in.plus) {
        message_.clear();
    }
    if (in.plus) {
        if (changed()) {
            ask_ = AskLeaveProgram;
        } else {
            done_ = true;
        }
        return;
    }
    if (in.b) {
        if (changed()) {
            ask_ = AskLeavePort;
        } else {
            screen_ = Ports;
            read_card();
        }
        return;
    }
    if (in.x) {
        run_apply(on_);
        return;
    }
    if (in.y) {
        if (on_.empty() && view_.state.mods.empty()) {
            message_ = "No mod is on for this port.";
        } else {
            ask_ = AskAllOff;
        }
        return;
    }
    move_cursor(cursor_, static_cast<int>(rows_.size()), in);
    scroll_to_cursor();
    if (rows_.empty()) {
        return;
    }
    const std::string id = rows_[static_cast<size_t>(cursor_)];
    const modcore::Mod& mod = view_.mods.at(id);
    const auto at = std::find(on_.begin(), on_.end(), id);
    if (in.a) {
        if (at != on_.end()) {
            on_.erase(at);
        } else if (!mod.bad.empty()) {
            message_ = "This mod can't be used: " + mod.bad;
            return;
        } else {
            on_.push_back(id); // (last: it wins the files it shares)
        }
        rebuild_rows(id);
    } else if (in.l || in.r) {
        if (at == on_.end()) {
            message_ = "Only a mod that is on has a place in the order.";
        } else if (in.l && at != on_.begin()) {
            std::iter_swap(at, at - 1);
            rebuild_rows(id);
        } else if (in.r && at + 1 != on_.end()) {
            std::iter_swap(at, at + 1);
            rebuild_rows(id);
        }
    }
}

// ---------------------------------------------------------------------------------------------

void App::draw(gfx::Canvas& c) const {
    switch (screen_) {
    case Ports:
        draw_ports(c);
        break;
    case Mods:
        draw_mods(c);
        if (ask_ != AskNothing) {
            draw_ask(c);
        }
        break;
    case Applying:
        draw_applying(c);
        break;
    case Outcome:
        draw_outcome(c);
        break;
    }
}

/// What every screen has: the bar at the top with the program's name and where one is, and the
/// bar at the bottom that says what the buttons do.
void App::draw_frame(gfx::Canvas& c, const std::string& title, const std::string& right, const std::string& controls) const {
    c.clear(kBg);
    c.fill(0, 0, kScreenW, kTitleH, kBar);
    int x = c.text(kMargin, 15, "nx2-mods", 3, kAccent);
    const size_t right_chars = gfx::plain(right).size();
    const int room = kScreenW - kMargin - gfx::advance(2, right_chars) - 24 - (x + gfx::advance(3, 1));
    c.text(x + gfx::advance(3, 1), 15, title, 3, kText, static_cast<size_t>(std::max(room, 0) / gfx::advance(3)));
    c.text_right(kScreenW - kMargin, 21, right, 2, kDim);
    c.fill(0, kScreenH - kFooterH, kScreenW, kFooterH, kBar);
    c.text(kMargin, kScreenH - kFooterH + 15, controls, 2, kText, 101);
}

void App::draw_ports(gfx::Canvas& c) const {
    draw_frame(c, "Games on this card", std::string("version ") + kVersion, "Up/Down move   A open   Y read the card again   + exit");
    const int count = static_cast<int>(ports_.size());
    if (count == 0) {
        c.text_centered(kScreenW / 2, 280, "No port is on this card.", 3, kText);
        c.text_centered(kScreenW / 2, 330, has_ports_folder_ ? "The folder /nx2-relink/ is empty." : "There is no folder /nx2-relink/ on it.", 2, kDim);
        c.text_centered(kScreenW / 2, 356, "A port is a game made into a program by nx2-relink, installed with its files.", 2, kDim);
    } else {
        c.text(kMargin, 68, "Port", 2, kDim);
        c.text(kMargin + gfx::advance(3, 24), 68, "Game", 2, kDim);
        c.text_right(kScreenW - 48, 68, "Mods on", 2, kDim);
    }
    for (int k = 0; k < kPortRows && port_scroll_ + k < count; ++k) {
        const int i = port_scroll_ + k, y = kListTop + k * kRowH;
        const modcore::Port& port = ports_[static_cast<size_t>(i)];
        if (i == port_cursor_) {
            c.fill(16, y - 4, kScreenW - 48, kRowH, kSelected);
        }
        c.text(kMargin, y, port.name, 3, kText, 22);
        const bool known = !port.title.empty() || !port.title_id.empty();
        c.text(kMargin + gfx::advance(3, 24), y, known ? (port.title.empty() ? port.title_id : port.title) : std::string("game not known"), 3, known ? kText : kDim, 34);
        c.text_right(kScreenW - 48, y, number(port.mods_on), 3, port.mods_on ? kAccent : kDim);
    }
    scrollbar(c, kListTop - 4, kPortRows, port_scroll_, count);
    std::string kept;
    if (!has_mods_folder_) {
        kept = "This card has no folder /mods: there are no mods on it to put on.";
    } else if (card_mods_.empty()) {
        kept = "The folder /mods on this card is empty: there are no mods to put on.";
    } else {
        kept = number(static_cast<long long>(card_mods_.size())) + " mod(s) are kept in /mods on this card.";
    }
    c.text(kMargin, 628, kept, 2, has_mods_folder_ && !card_mods_.empty() ? kDim : kWarn, 101);
    c.text(kMargin, 650, "A mod is a folder /mods/<name>/ with mod.json and its files under files/.", 2, kFaint, 101);
}

void App::draw_mods(gfx::Canvas& c) const {
    const modcore::Port& port = view_.port;
    const bool known = !port.title.empty() || !port.title_id.empty();
    const int count = static_cast<int>(rows_.size());
    draw_frame(c, port.name + ": " + (known ? (port.title.empty() ? port.title_id : port.title) : std::string("game not known")),
               number(static_cast<long long>(on_.size())) + " on, of " + number(count),
               "Up/Down move   A on/off   L/R earlier/later   X apply   Y all off   B back   + exit");

    const int x_box = kMargin, x_name = 112, x_version = 632, right_size = 900, x_note = 920;
    if (count == 0) {
        std::string why = !has_mods_folder_ ? "This card has no folder /mods." : card_mods_.empty() ? "There are no mods in /mods on this card."
                                                                                                      : "None of the mods in /mods on this card is for this game.";
        c.text_centered(kScreenW / 2, 250, why, 3, kText);
        c.text_centered(kScreenW / 2, 300, "A mod is a folder /mods/<name>/ with mod.json and its files under files/.", 2, kDim);
        c.text_centered(kScreenW / 2, 326, "Its mod.json names the game it is for: \"game\": {\"title_id\": \"...\"}.", 2, kDim);
    } else {
        c.text(x_box, 68, "On", 2, kDim);
        c.text(x_name, 68, "Mod", 2, kDim);
        c.text(x_version, 68, "Version", 2, kDim);
        c.text_right(right_size, 68, "Size", 2, kDim);
        c.text(x_note, 68, "Note", 2, kDim);
    }
    for (int k = 0; k < kModRows && scroll_ + k < count; ++k) {
        const int i = scroll_ + k, y = kListTop + k * kRowH;
        const std::string& id = rows_[static_cast<size_t>(i)];
        const modcore::Mod& mod = view_.mods.at(id);
        const auto at = std::find(on_.begin(), on_.end(), id);
        const bool on = at != on_.end(), usable = mod.bad.empty();
        if (i == cursor_) {
            c.fill(16, y - 5, kScreenW - 48, kRowH, kSelected);
        }
        c.frame(x_box, y - 1, 24, 24, 3, usable ? kText : kFaint);
        if (on) {
            c.fill(x_box + 6, y + 5, 12, 12, kAccent);
            c.text_right(x_name - 12, y, number(at - on_.begin() + 1), 3, kAccent);
        }
        c.text(x_name, y, mod.name, 3, usable ? kText : kDim, 28);
        c.text(x_version, y, mod.version, 3, kDim, 7);
        c.text_right(right_size, y, modcore::size_text(mod.size()), 3, kDim);
        const auto note = notes_.find(id);
        if (note != notes_.end()) {
            c.text(x_note, y + 5, note->second.brief, 2, usable ? kWarn : kBad, 27);
        }
    }
    scrollbar(c, kListTop - 5, kModRows, scroll_, count);

    // the highlighted mod, in full
    const int panel = kListTop + kModRows * kRowH + 4;
    c.fill(kMargin, panel, kScreenW - 2 * kMargin, 2, kFaint);
    int y = panel + 12;
    const int pitch = 22;
    if (count > 0) {
        const std::string& id = rows_[static_cast<size_t>(cursor_)];
        const modcore::Mod& mod = view_.mods.at(id);
        std::string facts = "/mods/" + id;
        if (mod.absent) {
            facts = id + " (not in /mods)";
        }
        if (!mod.author.empty()) {
            facts += "   by " + mod.author;
        }
        facts += "   " + number(static_cast<long long>(mod.files.size())) + " file(s), " + modcore::size_text(mod.size());
        if (!mod.game_title.empty() || !mod.game_title_id.empty()) {
            facts += "   for " + (mod.game_title.empty() ? mod.game_title_id : mod.game_title);
        }
        c.text(kMargin, y, facts, 2, kDim, 101);
        y += pitch;
        const std::vector<std::string> about = gfx::wrapped(gfx::plain(mod.description), 101, 2);
        if (about.empty()) {
            c.text(kMargin, y, mod.absent ? "(its description is in its folder, which this card doesn't have)" : "(no description)", 2, kFaint);
        }
        for (size_t n = 0; n < about.size(); ++n) {
            c.text(kMargin, y + static_cast<int>(n) * pitch, about[n], 2, kText);
        }
        y += 2 * pitch;
        const auto note = notes_.find(id);
        if (note != notes_.end()) {
            const std::vector<std::string> lines = gfx::wrapped(gfx::plain(note->second.full), 101, 2);
            for (size_t n = 0; n < lines.size(); ++n) {
                c.text(kMargin, y + static_cast<int>(n) * pitch, lines[n], 2, mod.bad.empty() ? kWarn : kBad);
            }
        }
        y += 2 * pitch;
    } else {
        y += 5 * pitch;
    }
    // the port, in one line
    std::string status;
    uint32_t color = kDim;
    if (!message_.empty()) {
        status = message_;
        color = kWarn;
    } else if (!view_.dropped.empty()) {
        status = joined(view_.dropped) + (view_.dropped.size() == 1 ? " was on and can't be used any more: X takes it off." : " were on and can't be used any more: X takes them off.");
        color = kWarn;
    } else if (changed()) {
        status = "The changes are not applied yet: X applies them.";
        color = kAccent;
    } else {
        if (!view_.loose.empty()) {
            status = number(static_cast<long long>(view_.loose.size())) + " file(s) put by hand in this port's mods folder are left alone.   ";
        }
        if (view_.other_game > 0) {
            status += number(view_.other_game) + " mod(s) on the card are for other games (not shown).";
        }
        if (status.empty() && !known) {
            status = "This port's game isn't known (it has no port.json): every mod on the card is offered.";
        }
    }
    c.text(kMargin, y, status, 2, color, 101);
}

void App::draw_applying(gfx::Canvas& c) const {
    draw_frame(c, "Applying to " + view_.port.name, "", "Please wait: don't turn the console off, and don't take the card out.");
    const modcore::Progress& p = progress_;
    std::string doing;
    switch (p.phase) {
    case modcore::Progress::Removing:
        doing = p.count ? "Removing " + number(p.index) + " of " + number(p.count) : std::string("Getting ready");
        break;
    case modcore::Progress::Copying:
        doing = "Copying " + number(p.index) + " of " + number(p.count);
        break;
    case modcore::Progress::Checking:
        doing = "Reading back " + number(p.index) + " of " + number(p.count);
        break;
    case modcore::Progress::Recording:
        doing = "Writing the port's record";
        break;
    }
    c.text_centered(kScreenW / 2, 230, doing, 3, kText);
    c.text_centered(kScreenW / 2, 276, p.path, 2, kDim, 96);
    const int x = 140, y = 330, w = 1000, h = 40;
    c.frame(x, y, w, h, 3, kText);
    const double share = p.total > 0 ? std::min(1.0, static_cast<double>(p.done) / static_cast<double>(p.total)) : (p.phase == modcore::Progress::Recording ? 1.0 : 0.0);
    c.fill(x + 6, y + 6, static_cast<int>((w - 12) * share), h - 12, kAccent);
    std::string amount = modcore::size_text(p.done) + " of " + modcore::size_text(p.total);
    if (p.total > 0) {
        amount += "   (" + number(static_cast<long long>(share * 100.0)) + "%)";
    }
    c.text_centered(kScreenW / 2, y + h + 18, p.total > 0 ? amount : std::string("Nothing to copy"), 2, kDim);
}

void App::draw_outcome(gfx::Canvas& c) const {
    draw_frame(c, (result_.ok ? "Applied to " : "Not applied to ") + view_.port.name, "", "A / B back to the mods   + exit");
    const uint32_t color = result_.ok ? kGood : kBad;
    int y = 84;
    for (const std::string& line : gfx::wrapped(gfx::plain(result_.summary), 67, 4)) {
        c.text(kMargin, y, line, 3, color);
        y += 32;
    }
    y += 16;
    const int most = (kScreenH - kFooterH - 12 - y) / 22;
    int shown = 0;
    for (size_t i = 0; i < result_.messages.size(); ++i) {
        const std::string& m = result_.messages[i];
        const bool error = m.find("NOT") != std::string::npos || m.find("couldn't") != std::string::npos;
        const std::vector<std::string> lines = gfx::wrapped(gfx::plain(m), 101, 2);
        if (shown + static_cast<int>(lines.size()) > most - 1 && i + 1 < result_.messages.size()) {
            c.text(kMargin, y, "... and " + number(static_cast<long long>(result_.messages.size() - i)) + " more line(s)", 2, kDim);
            break;
        }
        for (const std::string& line : lines) {
            c.text(kMargin, y, line, 2, error ? kBad : kText);
            y += 22;
            ++shown;
        }
    }
}

void App::draw_ask(gfx::Canvas& c) const {
    std::string question, more;
    switch (ask_) {
    case AskAllOff: {
        question = "Take every mod off this port?";
        int absent = 0;
        for (const std::string& id : on_) {
            absent += view_.mods.at(id).absent ? 1 : 0;
        }
        more = absent ? number(absent) + " of them can't be put on again from here: not kept on this card."
                      : std::string("Their files are removed from the port now. The mods stay in /mods.");
        break;
    }
    case AskLeavePort:
        question = "Leave without applying?";
        more = "The changes made here are not applied to the port.";
        break;
    case AskLeaveProgram:
        question = "Exit without applying?";
        more = "The changes made here are not applied to the port.";
        break;
    case AskNothing:
        return;
    }
    const int w = 860, h = 210, x = (kScreenW - w) / 2, y = (kScreenH - h) / 2;
    c.fill(x, y, w, h, kBar);
    c.frame(x, y, w, h, 3, kAccent);
    c.text_centered(kScreenW / 2, y + 32, question, 3, kText, 44);
    c.text_centered(kScreenW / 2, y + 86, more, 2, kDim, 68);
    c.text_centered(kScreenW / 2, y + 146, "A yes        B no", 3, kAccent);
}

} // namespace ui
