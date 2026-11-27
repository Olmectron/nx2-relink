// The program's screens, made on a PC: a pretend card is built (every name on it is made up),
// the same ui::App the console runs is played through a few states, and each screen is written
// as a picture (PPM; tools/ppm_to_png.py makes PNG files of them).
//
//   preview <work folder> <folder for the pictures>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gfx.h"
#include "ui.h"

namespace {

void make_dirs(const std::string& path) {
    for (size_t at = 1; at <= path.size(); ++at) {
        if (at == path.size() || path[at] == '/') {
            mkdir(path.substr(0, at).c_str(), 0777);
        }
    }
}

void write_file(const std::string& path, const std::string& data) {
    make_dirs(path.substr(0, path.rfind('/')));
    if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(data.data(), 1, data.size(), f);
        std::fclose(f);
    }
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

void save(const gfx::Canvas& c, const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::printf("can't write %s\n", path.c_str());
        std::exit(1);
    }
    std::fprintf(f, "P6\n%d %d\n255\n", c.width, c.height);
    std::vector<unsigned char> row(static_cast<size_t>(c.width) * 3);
    for (int y = 0; y < c.height; ++y) {
        for (int x = 0; x < c.width; ++x) {
            const uint32_t p = c.pixels[static_cast<size_t>(y) * c.width + x];
            row[static_cast<size_t>(x) * 3] = p & 0xff;
            row[static_cast<size_t>(x) * 3 + 1] = (p >> 8) & 0xff;
            row[static_cast<size_t>(x) * 3 + 2] = (p >> 16) & 0xff;
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    std::printf("written: %s\n", path.c_str());
}

const char* const kIsle = "0400123400001000";

void mod(const std::string& card, const std::string& id, const std::string& json, const std::vector<std::pair<std::string, size_t>>& files) {
    if (!json.empty()) {
        write_file(card + "/mods/" + id + "/mod.json", json);
    }
    for (const auto& f : files) {
        write_file(card + "/mods/" + id + "/files/" + f.first, std::string(f.second, static_cast<char>('a' + id.size() % 26)));
    }
}

std::string isle_mod(const std::string& name, const std::string& version, const std::string& more = "") {
    return "{\"name\": \"" + name + "\", \"game\": {\"title_id\": \"" + kIsle + "\", \"title\": \"Skyward Isle\"}, \"version\": \"" + version + "\"" + more + "}";
}

void build_card(const std::string& card) {
    remove_tree(card);
    write_file(card + "/nx2-relink/skyward-isle/port.json", std::string("{\"title\": \"Skyward Isle\", \"title_id\": \"") + kIsle + "\"}");
    write_file(card + "/nx2-relink/paper-racers/port.json", "{\"title\": \"Paper Racers Deluxe\", \"title_id\": \"0400567800002000\"}");
    make_dirs(card + "/nx2-relink/untitled-port");
    write_file(card + "/nx2-relink/skyward-isle/mods/by-hand/tweak.bin", "by hand");

    mod(card, "hd-textures", isle_mod("HD Textures", "2.1", ", \"author\": \"Pixel Gardener\", \"description\": \"Every texture of the islands and the sky redrawn at four times the size. "
                                                            "Overlaps with any mod that changes the same textures: put the one you want to see later in the order.\""),
        {{"textures/islands.pack", 6 * 1024 * 1024}, {"textures/sky.pack", 3 * 1024 * 1024 + 400 * 1024}, {"textures/hero.tex", 800 * 1024}});
    mod(card, "night-palette", isle_mod("Night Palette", "1.0", ", \"author\": \"Moth\", \"description\": \"The whole game at dusk.\""),
        {{"textures/sky.pack", 2 * 1024 * 1024}, {"shaders/tone.bin", 40 * 1024}});
    mod(card, "wide-camera", isle_mod("Wide Camera", "0.9", ", \"description\": \"The camera pulled back.\", \"requires\": [\"camera-core\"]"), {{"camera/params.bin", 2048}});
    mod(card, "music-restored", isle_mod("Music Restored", "3", ", \"author\": \"Tape Deck\", \"description\": \"The first version's soundtrack.\""), {{"audio/bank.bin", 9 * 1024 * 1024}});
    mod(card, "a-very-long-folder-name-for-a-mod", isle_mod("An Extremely Long Mod Name That Does Not Fit In Its Column At All", "1.0.0-beta12"), {{"misc/long.bin", 1234}});
    mod(card, "broken-one", "{\"name\": \"Broken One\", ", {{"x.bin", 10}});
    mod(card, "empty-one", isle_mod("Empty One", "1"), {});
    mod(card, "no-json", "", {{"y.bin", 10}});
    mod(card, "racer-skins", "{\"name\": \"Racer Skins\", \"game\": {\"title_id\": \"0400567800002000\"}, \"version\": \"1\"}", {{"skins/a.bin", 5000}});
    for (int i = 1; i <= 7; ++i) {
        char id[32], name[48];
        std::snprintf(id, sizeof id, "little-fix-%02d", i);
        std::snprintf(name, sizeof name, "Little Fix %d", i);
        mod(card, id, isle_mod(name, "1." + std::to_string(i), ", \"description\": \"A small correction.\""), {{std::string("fixes/") + id + ".bin", static_cast<size_t>(i) * 3000}});
    }
    // what the port has on: one of the card's mods, and one the PC tool put on straight from the PC
    write_file(card + "/nx2-relink/skyward-isle/mods/audio/bank.bin", std::string(9 * 1024 * 1024, 'm'));
    write_file(card + "/nx2-relink/skyward-isle/mods/outfits/cape.bin", std::string(300 * 1024, 'c'));
    write_file(card + "/nx2-relink/skyward-isle/mods.json",
               "{\"version\": 1, \"mods\": [{\"id\": \"music-restored\", \"name\": \"Music Restored\", \"version\": \"3\", \"files\": {\"audio/bank.bin\": 9437184}},"
               " {\"id\": \"pc-sent-outfit\", \"name\": \"Red Cape Outfit\", \"version\": \"1.4\", \"files\": {\"outfits/cape.bin\": 307200}}]}");
}

ui::Input press(char what) {
    ui::Input in;
    in.up = what == 'u';
    in.down = what == 'd';
    in.a = what == 'a';
    in.b = what == 'b';
    in.x = what == 'x';
    in.y = what == 'y';
    in.l = what == 'l';
    in.r = what == 'r';
    in.plus = what == '+';
    return in;
}

void play(ui::App& app, const char* keys) {
    for (const char* k = keys; *k; ++k) {
        app.update(press(*k));
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: preview <work folder> <folder for the pictures>\n");
        return 2;
    }
    const std::string card = std::string(argv[1]) + "/card", out = argv[2];
    make_dirs(out);
    gfx::Canvas canvas(ui::kScreenW, ui::kScreenH);
    auto shot = [&](ui::App& app, const char* name) {
        app.draw(canvas);
        save(canvas, out + "/" + name + ".ppm");
    };

    build_card(card);
    {
        ui::App app(card);
        shot(app, "01-ports");
        play(app, "dda"); // (the third port: its game isn't known)
        shot(app, "02-port-game-not-known");
        play(app, "bua"); // (back, and into the port that has mods on)
        shot(app, "03-mods");
        // hd-textures and night-palette on (they overlap), wide-camera on (what it needs isn't there)
        play(app, "ddda");
        play(app, "ddddddddda");
        play(app, "ddddddddda");
        shot(app, "04-mods-changed");
        play(app, "uul"); // (hd-textures moved earlier in the order)
        shot(app, "05-mods-order");
        play(app, "dddddddddddddd");
        shot(app, "06-mods-scrolled");
        play(app, "y");
        shot(app, "07-ask-all-off");
        play(app, "b");
        int frames = 0;
        app.set_present([&]() {
            if (++frames == 24) {
                shot(app, "08-applying");
            }
        });
        play(app, "x");
        shot(app, "09-applied");
        play(app, "a");
        shot(app, "10-mods-after");
    }
    {
        // a mod's file that is gone when it is to be copied
        build_card(card);
        ui::App app(card);
        play(app, "da");
        play(app, "ddda");
        std::remove((card + "/mods/hd-textures/files/textures/sky.pack").c_str());
        play(app, "x");
        shot(app, "11-apply-failed");
    }
    {
        remove_tree(card);
        make_dirs(card);
        ui::App app(card);
        shot(app, "12-no-ports");
        write_file(card + "/nx2-relink/lonely-port/port.json", "{\"title\": \"Lonely Game\", \"title_id\": \"0400999900003000\"}");
        play(app, "ya");
        shot(app, "13-no-mods-folder");
    }
    remove_tree(card);
    return 0;
}
