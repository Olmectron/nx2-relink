// The program's screens: the ports on the card, a port's mods, an apply under way and how it
// went. It is given what was pressed and draws on a gfx::Canvas; it knows nothing of the console,
// so a PC can play it through its screens and save them as pictures.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "gfx.h"
#include "modcore.h"

namespace ui {

const int kScreenW = 1280;
const int kScreenH = 720;
extern const char* const kVersion;

/// What was pressed since the last frame (for the directions: or is held and repeating).
struct Input {
    bool up = false, down = false;
    bool a = false, b = false, x = false, y = false;
    bool l = false, r = false;
    bool plus = false;
};

class App {
public:
    /// `root` is the card: "sdmc:/" on the console, a folder on a PC.
    explicit App(const std::string& root);

    /// Called to show a frame while an apply is under way (the apply doesn't return until it is done).
    void set_present(std::function<void()> present) { present_ = std::move(present); }
    /// A clock in milliseconds, to show progress no more often than is of use. Without one, every step is shown.
    void set_clock(std::function<uint64_t()> clock) { clock_ = std::move(clock); }

    void update(const Input& in);
    void draw(gfx::Canvas& canvas) const;
    /// True once the user asked to leave.
    bool done() const { return done_; }

private:
    enum Screen { Ports, Mods, Applying, Outcome };
    enum Ask { AskNothing, AskAllOff, AskLeavePort, AskLeaveProgram };

    void read_card();
    void open_port(const std::string& name);
    void rebuild_rows(const std::string& keep_on);
    bool changed() const;
    void run_apply(const std::vector<std::string>& want);
    void update_ports(const Input& in);
    void update_mods(const Input& in);
    void scroll_to_cursor();
    bool is_on(const std::string& id) const;

    void draw_frame(gfx::Canvas& c, const std::string& title, const std::string& right, const std::string& controls) const;
    void draw_ports(gfx::Canvas& c) const;
    void draw_mods(gfx::Canvas& c) const;
    void draw_applying(gfx::Canvas& c) const;
    void draw_outcome(gfx::Canvas& c) const;
    void draw_ask(gfx::Canvas& c) const;

    std::string root_;
    std::function<void()> present_;
    std::function<uint64_t()> clock_;
    bool done_ = false;
    Screen screen_ = Ports;
    Ask ask_ = AskNothing;

    // the card
    modcore::ModMap card_mods_;
    bool has_mods_folder_ = false, has_ports_folder_ = false;
    std::vector<modcore::Port> ports_;
    int port_cursor_ = 0, port_scroll_ = 0;

    // the port that is open
    modcore::PortView view_;
    std::vector<std::string> on_;     // the mods ticked, in order
    std::vector<std::string> rows_;   // every mod shown: the ticked ones first, in order
    int cursor_ = 0, scroll_ = 0;
    struct Note {
        std::string brief, full;   // beside the mod's name; under the list when it is the one highlighted
    };
    std::map<std::string, Note> notes_;
    std::string message_;             // said once, about the last button pressed

    // an apply
    modcore::Progress progress_;
    uint64_t last_shown_ = 0;
    modcore::Result result_;
};

} // namespace ui
