// nx2-mods on the console: a software framebuffer for the screens, the pad for what was pressed,
// and the card as "sdmc:/". Everything else is ui.cpp (the screens) and modcore.cpp (the mods).
//
// What this file asks of the system: the pad, the default window's framebuffer, the applet's main
// loop, and the system's tick counter (to repeat a held direction and to pace the progress bar).
// Files are reached through the C library alone. Leaving main returns to the homebrew menu.

#include <cstring>
#include <memory>

#include <switch.h>

#include "gfx.h"
#include "ui.h"

namespace {

/// A direction held repeats: after a pause, then steadily.
struct Held {
    u64 next = 0;

    bool step(bool pressed, bool held, u64 now_ms) {
        if (pressed) {
            next = now_ms + 380;
            return true;
        }
        if (!held) {
            return false;
        }
        if (now_ms >= next) {
            next = now_ms + 80;
            return true;
        }
        return false;
    }
};

u64 now_ms() {
    return armTicksToNs(armGetSystemTick()) / 1000000ULL;
}

void show(Framebuffer& fb, const gfx::Canvas& canvas) {
    u32 stride = 0;
    u8* base = static_cast<u8*>(framebufferBegin(&fb, &stride));
    if (!base) {
        return; // (no image to draw on this time: the next frame tries again)
    }
    for (int y = 0; y < canvas.height; ++y) {
        std::memcpy(base + static_cast<size_t>(y) * stride, canvas.pixels.data() + static_cast<size_t>(y) * canvas.width,
                    static_cast<size_t>(canvas.width) * 4);
    }
    framebufferEnd(&fb);
}

} // namespace

int main(int, char**) {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    Framebuffer fb;
    if (R_FAILED(framebufferCreate(&fb, nwindowGetDefault(), ui::kScreenW, ui::kScreenH, PIXEL_FORMAT_RGBA_8888, 2))) {
        return 1; // (no screen to say anything on)
    }
    if (R_FAILED(framebufferMakeLinear(&fb))) {
        framebufferClose(&fb);
        return 1;
    }

    auto canvas = std::make_unique<gfx::Canvas>(ui::kScreenW, ui::kScreenH);
    // (a first picture before the card is read: reading every mod's folder can take a moment)
    canvas->clear(gfx::rgb(18, 22, 30));
    canvas->text_centered(ui::kScreenW / 2, 340, "nx2-mods: reading the card...", 3, gfx::rgb(232, 236, 242));
    show(fb, *canvas);

    auto app = std::make_unique<ui::App>("sdmc:/");
    bool leave = false;
    app->set_clock(now_ms);
    app->set_present([&]() {
        // (while files are copied: the screen redrawn, and the system's messages still answered)
        if (!appletMainLoop()) {
            leave = true;
        }
        app->draw(*canvas);
        show(fb, *canvas);
    });

    Held up, down;
    while (!leave && appletMainLoop()) {
        padUpdate(&pad);
        const u64 pressed = padGetButtonsDown(&pad), held = padGetButtons(&pad);
        const u64 now = now_ms();
        ui::Input in;
        in.a = pressed & HidNpadButton_A;
        in.b = pressed & HidNpadButton_B;
        in.x = pressed & HidNpadButton_X;
        in.y = pressed & HidNpadButton_Y;
        in.l = pressed & (HidNpadButton_L | HidNpadButton_ZL);
        in.r = pressed & (HidNpadButton_R | HidNpadButton_ZR);
        in.plus = pressed & HidNpadButton_Plus;
        in.up = up.step(pressed & HidNpadButton_AnyUp, held & HidNpadButton_AnyUp, now);
        in.down = down.step(pressed & HidNpadButton_AnyDown, held & HidNpadButton_AnyDown, now);

        app->update(in);
        if (app->done()) {
            break;
        }
        app->draw(*canvas);
        show(fb, *canvas);
    }
    framebufferClose(&fb);
    return 0;
}
