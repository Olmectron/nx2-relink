# nx2-mods

A program for the console's homebrew menu that puts mods on and takes them off the ports on the card, the way
`tools/mods.py` (`nxrelink.py mods ...`) does from a PC. Both read and write the same files, so either can be used after
the other.

Version 0.1.0. Built and tested on a PC only so far: see "What is not known yet" at the end.

## What it does

1. **Games on this card**: the ports found in `/nx2-relink/`, each with its game (from `port.json`; "game not known" when
   there is none) and how many mods it has on.
2. **A port's mods**: the mods kept in `/mods` that are for that port's game (all of them when the port's game isn't
   known). The ones that are on come first, in their order, numbered. Each row has a tick box, the mod's name, version, size
   and a note; the highlighted mod's folder, author, description and notes in full are under the list.
3. **Apply**: a progress bar while files are removed and copied, then a line that says how it went, with every note and
   error under it.

| Button | Games | A port's mods |
|---|---|---|
| Up / Down (held: repeats) | move | move |
| `A` | open the port | put the highlighted mod on (last in the order) or take it off |
| `L` / `R` (or `ZL` / `ZR`) | | move the highlighted mod earlier / later in the order |
| `X` | | apply |
| `Y` | read the card again | take every mod off (asks first, then applies) |
| `B` | | back (asks first when changes are not applied) |
| `+` | exit | exit (asks first when changes are not applied) |

The controls are also written at the bottom of every screen. Nothing changes on the card until `X` (or `Y` and a yes).

Notes a mod can have:

- **overlaps X**: it and X have a file at the same path. The game gets the file of the one later in the order; when that one
  is taken off, the other's file is put back.
- **N file(s) overridden**: it is on, and a mod later in the order has N of its paths.
- **needs X**: its `mod.json` lists X under `requires`, and X isn't on. It is a note, not a refusal.
- **can't be used**: it has no `mod.json`, its `mod.json` can't be read, or it has no files. It can't be ticked.
- **not kept on this card**: it is on for this port and has no folder in `/mods` (the PC tool can put a mod on straight from
  the PC). It keeps its place in the order and its files as long as it stays ticked; it can be moved in the order and taken
  off; once taken off and applied, only the PC tool can put it on again. Its files are never copied by this program.

Mods for another game are not offered (the last line of the screen says how many there are). A mod that is on by the
port's record and can't be used any more (or is for another game now) is shown unticked, and is taken off at the next apply.

## The files

```
/mods/<mod>/mod.json                 {"name": "...", "game": {"title_id": "0400...", "title": "..."}, "version": "1.0",
                                      "author": "...", "description": "...", "requires": ["<another mod's folder>"]}
/mods/<mod>/files/<path in the game> the mod's files
/nx2-relink/<port>/port.json         {"title": "...", "title_id": "0400..."}      (read only; may be missing)
/nx2-relink/<port>/mods/<path>       what the port reads in place of the game's own file
/nx2-relink/<port>/mods.json         {"version": 1, "mods": [{"id": "<mod's folder>", "name": "...", "version": "...",
                                                              "files": {"<path>": <size>, ...}}, ...]}
```

- A mod is for a port when their title ids are the same (compared in lower case); when one of the two has no title id, when
  their titles are the same (capitals and outer spaces aside); when that can't be told either, the mod is offered.
- `mods.json` lists the mods that are on, in order: a later one wins the paths it shares. A mod's `files` are only the paths
  where its file is the one in use, each with its size.
- A file in the port's `mods` folder that no mod's record names was put there by hand: it is never removed. If a mod that is
  put on has a file at the same path, the mod's file replaces it (as the PC tool does), and the result screen says so.
- `mods.json` is written as Python's `json.dumps(indent=1)` writes it (ASCII, `\uXXXX` for the rest), with a mod's files in
  the order of their paths. Read: a byte order mark and keys this doesn't know are accepted.

Two things this program writes that `tools/mods.py` (as of this version) doesn't, both accepted by it:

- **A size of `-1`** in `files`: "this path is this mod's, and the file isn't whole on the card". The record is written
  once before any file is copied, with `-1` for every file about to be copied, and again at the end with the real sizes; a
  copy that fails keeps its `-1`. So if the console is turned off halfway, every file already copied is known to be a mod's
  (it is removed when the mod is taken off, not left behind as if put by hand), and the next apply, here or on the PC,
  copies again what has no size. (`mods.py` leaves a failed path out of the record instead.)
- **Temporary names**: a file is copied to `<its name>.nx2mods-tmp`, read back and compared, and renamed only then, so a file
  under its own name is always whole. `mods.json` is written the same way (`mods.json.nx2mods-tmp`). Leftovers with that
  ending are removed at the next apply; a `mods.json.nx2mods-tmp` with no `mods.json` beside it is taken as the record (the
  run stopped between removing the old one and renaming the new one).

What the program touches on the card: it creates, replaces, renames and removes files and empty folders under
`/nx2-relink/<port>/mods/`, and `/nx2-relink/<port>/mods.json` (with its temporary name). It reads `/mods` and
`/nx2-relink/<port>/port.json`. Nothing else is opened. A path in `mods.json` that is not a plain relative path (`..`, a
leading `/`, `\`, `:`) is never followed.

## Building

In devkitPro's own shell (Git Bash's `make` doesn't do), with ounce-devkit (libounce) installed; `DEVKITPRO` is
`/opt/devkitpro` there:

```
/c/devkitPro/msys2/usr/bin/bash -lc "cd /c/<where this repository is>/modmanager && make"
```

That writes `../out/nx2-mods.nro` (name "nx2-mods", author "Olmectron", version 0.1.0, `icon.jpg`). Copy it to
`/switch/nx2-mods.nro` on the card (or a folder of `/switch/`) and start it from the homebrew menu.

Other targets, same shell:

```
make test       builds test/test_core.cpp with the PC's g++ and runs it: a pretend card under build/host/work,
                and tools/mods.py run against the same card in both directions (needs Python; without it, say
                build/host/test_core build/host/work   to run the part that needs none)
make preview    builds preview/preview_main.cpp with the PC's g++, plays the program through its screens on a
                pretend card and writes them to preview/*.png (needs Python + Pillow for the PNG step)
make font       source/font_data.h again, from the pictures in tools/make_font.py
make icon       icon.jpg again, from tools/make_icon.py (needs Python + Pillow)
make clean
```

devkitPro's shell starts without Windows' `APPDATA`, so a Pillow installed "for the user" isn't found from it; the
two steps that need Pillow can be run from any other shell instead:

```
python tools/make_icon.py icon.jpg
python tools/ppm_to_png.py build/host/pictures preview      (after: build/host/preview build/host/preview-work build/host/pictures)
```

## The source

| File | What |
|---|---|
| `source/json.h`, `json.cpp` | a small JSON reader and writer (objects, arrays, strings with every escape, numbers, true/false/null) |
| `source/modcore.h`, `modcore.cpp` | the logic: reading mods, ports and records under a card root; matching by game; overlaps; the plan; the apply |
| `source/gfx.h`, `gfx.cpp` | a picture in memory, boxes and text |
| `source/font_data.h` | the font, 5 x 9 dots a character, made by `tools/make_font.py` from pictures drawn by hand in that file |
| `source/ui.h`, `ui.cpp` | the screens: given what was pressed, draws on a `gfx::Canvas`; knows nothing of the console |
| `source/main_switch.cpp` | the console: pad, framebuffer, main loop |
| `test/test_core.cpp` | the checks |
| `preview/preview_main.cpp` | the screens made on a PC; `preview/*.png` are its pictures |
| `tools/make_font.py`, `make_icon.py`, `ppm_to_png.py` | the font's header, the icon, PPM to PNG |

`json` and `modcore` use the C++ standard library and `fopen`, `opendir`/`readdir`, `stat`, `mkdir`, `rmdir`, `remove`,
`rename`, with every path under one root: `sdmc:/` on the console, a folder on a PC. No code of anyone else's is in this
folder: the drawing and the font were written for it.

Text is drawn in the program's own font, which has ASCII. Accented Latin letters are drawn without their accent, and any
other character as a small box; names are still read, compared and written as they are.

## What is not known yet

The program has not been run on a console. What was checked on a PC is the logic (the same `json.cpp` and `modcore.cpp`
the console build compiles), the screens (the same `ui.cpp` and `gfx.cpp`), and that the `.nro` builds and links. What only
a console can tell:

- **The screen**: `framebufferCreate(nwindowGetDefault(), 1280, 720, PIXEL_FORMAT_RGBA_8888, 2)`, `framebufferMakeLinear`,
  and one `framebufferBegin` / copy / `framebufferEnd` a frame, as libounce's header describes for Switch 2. If the
  framebuffer can't be made the program leaves at once, with nothing shown.
- **The pad**: `padConfigureInput(1, HidNpadStyleSet_NpadStandard)`, `padInitializeDefault`, `padUpdate`.
- **The card's file system**: that `sdmc:/` is mounted when `main` starts; that `rename` either replaces a file that is
  there or fails (the program then removes the old file and renames again); that `rmdir` removes an empty folder and
  refuses one that isn't; that `stat` gives a file's size right after it is written and closed; long names and names
  that are not ASCII.
- **Time**: a copy is written, read back whole and compared, so it takes about twice a plain copy. The screen is redrawn
  between pieces of 1 MB; the system's messages are answered then too (`appletMainLoop`), and a request to close is
  obeyed once the apply is done. The console's auto-sleep is not held off.
- **Leaving**: returning from `main` goes back to the homebrew menu.
