// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Leo Kuroshita (@kurogedelic)
// nestopia-cz: a front end for the Nestopia core on the Cardputer Zero (LCD, keyboard, sound).
//   nestopia-cz [--headless N] [--ppm FILE] [--scale tv|fit|crop] [--no-sound] game.nes
// Video goes to /dev/fb0 (RGB565, 320x170), keys come from evdev, sound through SDL2 (PipeWire).
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "font8x8_basic.h"
#include "NstApiEmulator.hpp"
#include "NstApiMachine.hpp"
#include "NstApiVideo.hpp"
#include "NstApiSound.hpp"
#include "NstApiInput.hpp"
#include "NstApiCartridge.hpp"

#ifdef NESTOPIACZ_HAS_SDL
#include <SDL.h>
#endif

using namespace Nes::Api;

namespace {

constexpr int kSrcW = 256, kSrcH = 240;
constexpr int kLcdW = 320, kLcdH = 170;
constexpr int kRate = 48000;
constexpr double kFps = 60.0988;

double nowS() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// ---- the LCD -------------------------------------------------------------------------------------------------
struct Lcd {
    int fd = -1;
    uint8_t* map = nullptr;
    size_t len = 0;
    int w = kLcdW, h = kLcdH, stride = kLcdW * 2;
    bool open_fb(const char* path) {
        fd = ::open(path, O_RDWR);
        if (fd < 0) return false;
        fb_var_screeninfo vi;
        fb_fix_screeninfo fi;
        if (ioctl(fd, FBIOGET_VSCREENINFO, &vi) || ioctl(fd, FBIOGET_FSCREENINFO, &fi) || vi.bits_per_pixel != 16) return false;
        w = vi.xres; h = vi.yres; stride = fi.line_length;
        len = (size_t)stride * h;
        void* m = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (m == MAP_FAILED) return false;
        map = (uint8_t*)m;
        memset(map, 0, len);
        return true;
    }
};

// ---- the keyboard ---------------------------------------------------------------------------------------------
struct Keys {
    int fd = -1;
    uint32_t pad = 0;      // Nes::Api::Input::Pad bits
    bool quit = false, save = false, load = false;
    bool m_up = false, m_down = false, m_ok = false;   // menu edges (the ROM list)
    double esc_down = 0;
    bool open_dev(const char* path) {
        fd = ::open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) return false;
        ioctl(fd, EVIOCGRAB, 1);               // the launcher must not see these keys
        return true;
    }
    static int bit(int code) {
        switch (code) {
        case KEY_UP: case KEY_F: return Input::Controllers::Pad::UP;
        case KEY_DOWN: case KEY_X: return Input::Controllers::Pad::DOWN;
        case KEY_LEFT: case KEY_Z: return Input::Controllers::Pad::LEFT;
        case KEY_RIGHT: case KEY_C: return Input::Controllers::Pad::RIGHT;
        case KEY_A: case KEY_K: return Input::Controllers::Pad::A;
        case KEY_S: case KEY_J: return Input::Controllers::Pad::B;
        case KEY_ENTER: return Input::Controllers::Pad::START;
        case KEY_BACKSPACE: return Input::Controllers::Pad::SELECT;
        }
        return 0;
    }
    void poll() {
        if (fd < 0) return;
        input_event ev[32];
        for (;;) {
            ssize_t n = read(fd, ev, sizeof ev);
            if (n < (ssize_t)sizeof(input_event)) break;
            for (size_t i = 0; i < n / sizeof(input_event); i++) {
                if (ev[i].type != EV_KEY) continue;
                int c = ev[i].code, v = ev[i].value;
                if (v == 1) { if (c == KEY_UP || c == KEY_F) m_up = true; if (c == KEY_DOWN || c == KEY_X) m_down = true; if (c == KEY_ENTER) m_ok = true; }
                if (int b = bit(c)) { if (v) pad |= b; else pad &= ~b; }
                else if (c == KEY_ESC) { if (v == 1) esc_down = nowS(); if (v == 0) esc_down = 0; }
                else if (c == KEY_1 && v == 1) save = true;
                else if (c == KEY_2 && v == 1) load = true;
            }
        }
        if (esc_down && nowS() - esc_down > 1.0) quit = true;
    }
    ~Keys() { if (fd >= 0) { ioctl(fd, EVIOCGRAB, 0); close(fd); } }
};

std::string find_keyboard() {
    const char* by = "/dev/input/by-path/platform-3f804000.i2c-event";
    if (access(by, R_OK) == 0) return by;
    return "";
}

// ---- Nestopia callbacks -----------------------------------------------------------------------------------------
std::vector<uint16_t> g_video(kSrcW * kSrcH);
std::vector<int16_t> g_audio(2048);



// ---- the ROM list ----------------------------------------------------------------------------------------------
void draw_text(const Lcd& lcd, int x, int y, const std::string& t, uint16_t fg, uint16_t bg) {
    for (size_t k = 0; k < t.size(); k++) {
        unsigned char c = (unsigned char)t[k] & 0x7F;
        for (int r = 0; r < 8; r++) for (int b = 0; b < 8; b++) {
            int px = x + (int)k * 8 + b, py = y + r;
            if (px < 0 || px >= lcd.w || py < 0 || py >= lcd.h) continue;
            ((uint16_t*)(lcd.map + (size_t)py * lcd.stride))[px] = ((font8x8_basic[c][r] >> b) & 1) ? fg : bg;
        }
    }
}
std::vector<std::string> scan_roms(const std::string& dir) {
    std::vector<std::string> out;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() > 4 && (n.substr(n.size() - 4) == ".nes" || n.substr(n.size() - 4) == ".NES")) out.push_back(n);
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}
// returns the chosen ROM path, or "" to quit
std::string pick_rom(const Lcd& lcd, Keys& keys, const std::string& dir) {
    auto roms = scan_roms(dir);
    int sel = 0, top = 0;
    const int rows = 16;
    keys.m_up = keys.m_down = keys.m_ok = false;
    while (!keys.quit) {
        keys.poll();
        if (keys.m_up && sel > 0) sel--;
        if (keys.m_down && sel + 1 < (int)roms.size()) sel++;
        if (!roms.empty() && keys.m_ok) { keys.m_ok = false; return dir + "/" + roms[sel]; }
        keys.m_up = keys.m_down = keys.m_ok = false;
        if (sel < top) top = sel;
        if (sel >= top + rows) top = sel - rows + 1;
        for (int y = 0; y < lcd.h; y++) memset(lcd.map + (size_t)y * lcd.stride, 0, lcd.w * 2);
        draw_text(lcd, 4, 2, "Nestopia", 0xFFE0, 0);
        draw_text(lcd, 4, 12, roms.empty() ? "put .nes files in ~/roms" : "up/down + Enter, hold Esc: quit", 0x8410, 0);
        for (int i = 0; i < rows && top + i < (int)roms.size(); i++) {
            std::string n = roms[top + i];
            if (n.size() > 38) n = n.substr(0, 38);
            bool on = top + i == sel;
            draw_text(lcd, 4, 24 + i * 9, n, on ? 0x0000 : 0xFFFF, on ? 0xFFE0 : 0);
        }
        usleep(30000);
    }
    return "";
}

// Scale the 256x240 picture into the LCD with bilinear smoothing (dropping rows would crush small text).
//   tv   = 4:3, the shape a TV showed it in (227x170)      fit  = square pixels (181x170, the whole picture)
//   crop = the middle 256x170 at 1:1, no scaling (top and bottom 35 lines are cut)
enum class Mode { Tv, Fit, Crop };
struct Axis { std::vector<int> i0, i1, w; };
Axis make_axis(int dst, int src, int offset = 0, bool one_to_one = false) {
    Axis a; a.i0.resize(dst); a.i1.resize(dst); a.w.resize(dst);
    for (int d = 0; d < dst; d++) {
        if (one_to_one) { a.i0[d] = a.i1[d] = d + offset; a.w[d] = 0; continue; }
        double f = (d + 0.5) * src / dst - 0.5;
        if (f < 0) f = 0;
        int i = (int)f; if (i > src - 1) i = src - 1;
        a.i0[d] = i; a.i1[d] = i + 1 < src ? i + 1 : i; a.w[d] = (int)((f - i) * 256);
    }
    return a;
}
void blit(const Lcd& lcd, Mode mode) {
    static Mode built = (Mode)-1;
    static Axis ax, ay; static int dw, dh;
    if (built != mode) {
        built = mode;
        dh = kLcdH;
        if (mode == Mode::Crop) { dw = kSrcW; ax = make_axis(dw, kSrcW, 0, true); ay = make_axis(dh, kSrcH, (kSrcH - kLcdH) / 2, true); }
        else { dw = mode == Mode::Tv ? kLcdH * 4 / 3 : kSrcW * kLcdH / kSrcH; ax = make_axis(dw, kSrcW); ay = make_axis(dh, kSrcH); }
    }
    const int ox = (lcd.w - dw) / 2, oy = (lcd.h - dh) / 2;
    for (int y = 0; y < dh; y++) {
        const uint16_t* r0 = &g_video[ay.i0[y] * kSrcW];
        const uint16_t* r1 = &g_video[ay.i1[y] * kSrcW];
        const int wy = ay.w[y];
        uint16_t* d = (uint16_t*)(lcd.map + (size_t)(y + oy) * lcd.stride) + ox;
        for (int x = 0; x < dw; x++) {
            const int x0 = ax.i0[x], x1 = ax.i1[x], wx = ax.w[x];
            uint32_t p00 = r0[x0], p01 = r0[x1], p10 = r1[x0], p11 = r1[x1];
            auto ch = [&](int shift, uint32_t mask) {
                int a = (p00 >> shift) & mask, b = (p01 >> shift) & mask;
                int c = (p10 >> shift) & mask, e = (p11 >> shift) & mask;
                int top = a * (256 - wx) + b * wx, bot = c * (256 - wx) + e * wx;
                return (top * (256 - wy) + bot * wy) >> 16;
            };
            d[x] = (uint16_t)((ch(11, 31) << 11) | (ch(5, 63) << 5) | ch(0, 31));
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string rom, ppm;
    long headless = 0, press_start = -1;
    Mode mode = Mode::Tv; bool sound_on = true;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--headless" && i + 1 < argc) headless = atol(argv[++i]);
        else if (a == "--ppm" && i + 1 < argc) ppm = argv[++i];
        else if (a == "--press-start" && i + 1 < argc) press_start = atol(argv[++i]);   // test aid for --headless
        else if (a == "--scale" && i + 1 < argc) { std::string m = argv[++i]; mode = m == "crop" ? Mode::Crop : m == "fit" ? Mode::Fit : Mode::Tv; }
        else if (a == "--no-sound") sound_on = false;
        else rom = a;
    }
    Emulator emu;
    Machine machine(emu);
    Video video(emu);
    Sound sound(emu);
    Input input(emu);

    Lcd lcd;
    Keys keys;
    bool have_lcd = false;
    if (!headless) {
        have_lcd = lcd.open_fb("/dev/fb0");
        if (!have_lcd) fprintf(stderr, "warning: /dev/fb0 not usable\n");
        std::string kb = find_keyboard();
        if (kb.empty() || !keys.open_dev(kb.c_str())) fprintf(stderr, "warning: no keyboard device\n");
    }

#ifdef NESTOPIACZ_HAS_SDL
    SDL_AudioDeviceID dev = 0;
    if (sound_on && !headless) {
        SDL_Init(SDL_INIT_AUDIO);
        SDL_AudioSpec want; SDL_zero(want);
        want.freq = kRate; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
        dev = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
        if (dev) SDL_PauseAudioDevice(dev, 0); else fprintf(stderr, "warning: no audio: %s\n", SDL_GetError());
    }
#endif

    Input::Controllers pads;
    auto play = [&](const std::string& rom) -> int {
    std::ifstream f(rom, std::ios::binary);
    if (!f) { fprintf(stderr, "cannot open %s\n", rom.c_str()); return 1; }
    Nes::Result r = machine.Load(f, Machine::FAVORED_NES_NTSC);
    if (NES_FAILED(r)) { fprintf(stderr, "cannot load %s (Nestopia result %d)\n", rom.c_str(), (int)r); return 1; }

    Video::RenderState rs;
    rs.filter = Video::RenderState::FILTER_NONE;
    rs.width = kSrcW; rs.height = kSrcH;
    rs.bits.count = 16;
    rs.bits.mask.r = 0xF800; rs.bits.mask.g = 0x07E0; rs.bits.mask.b = 0x001F;
    if (NES_FAILED(video.SetRenderState(rs))) { fprintf(stderr, "video setup failed\n"); return 1; }
    sound.SetSampleRate(kRate);
    sound.SetVolume(Sound::ALL_CHANNELS, 85);
    input.ConnectController(0, Input::PAD1);
    machine.Power(true);

    const int spf = (int)(kRate / kFps + 1);                // samples per frame (mono)
    std::vector<int16_t> mono(spf * 2), stereo(spf * 2 * 2);
    double t0 = nowS(), next = t0, busy = 0;
    long frames = 0;
    std::string state_path = rom + ".state";
    while (headless ? frames < headless : !keys.quit) {
        double a = nowS();
        keys.poll();
        pads.pad[0].buttons = keys.pad;
        if (headless && press_start >= 0 && frames >= press_start && frames < press_start + 5) pads.pad[0].buttons |= Input::Controllers::Pad::START;
        Sound::Output so(mono.data(), spf, nullptr, 0);
        Video::Output vo(g_video.data(), kSrcW * 2);
        emu.Execute(&vo, &so, &pads);
        if (have_lcd) blit(lcd, mode);
        if (keys.save) { keys.save = false; std::ofstream o(state_path, std::ios::binary); machine.SaveState(o); }
        if (keys.load) { keys.load = false; std::ifstream i(state_path, std::ios::binary); if (i) machine.LoadState(i); }
#ifdef NESTOPIACZ_HAS_SDL
        if (dev) {
            for (int i = 0; i < spf; i++) { stereo[2 * i] = mono[i]; stereo[2 * i + 1] = mono[i]; }
            SDL_QueueAudio(dev, stereo.data(), spf * 4);
            // audio paces the loop: keep about 3 frames queued
            while (SDL_GetQueuedAudioSize(dev) > (Uint32)(spf * 4 * 3)) usleep(1000);
        } else
#endif
        if (!headless) {
            next += 1.0 / kFps;
            double d = next - nowS();
            if (d > 0) usleep((useconds_t)(d * 1e6)); else next = nowS();
        }
        busy += nowS() - a;
        frames++;
    }
    double el = nowS() - t0;
    if (headless) {
        printf("frames %ld in %.2f s: %.1f fps (core+blit busy %.1f%%), samples nonzero: ", frames, el, frames / el, 100 * busy / el);
        long nz = 0; for (int i = 0; i < spf; i++) nz += mono[i] != 0; printf("%ld/%d\n", nz, spf);
        if (!ppm.empty()) {
            FILE* o = fopen(ppm.c_str(), "wb"); fprintf(o, "P6\n%d %d\n255\n", kSrcW, kSrcH);
            for (int i = 0; i < kSrcW * kSrcH; i++) { uint16_t p = g_video[i]; uint8_t c[3] = {(uint8_t)((p >> 11) << 3), (uint8_t)(((p >> 5) & 63) << 2), (uint8_t)((p & 31) << 3)}; fwrite(c, 1, 3, o); }
            fclose(o);
        }
    }
    machine.Power(false);
    machine.Unload();
    return 0;
};


    if (!rom.empty()) return play(rom);
    if (headless) { fprintf(stderr, "usage: nestopia-cz [--headless N] [--ppm FILE] [--scale tv|fit|crop] [--no-sound] game.nes | (no argument: ROM list of ~/roms)\n"); return 2; }
    std::string dir = std::string(getenv("HOME") ? getenv("HOME") : ".") + "/roms";
    for (;;) {
        std::string pick = pick_rom(lcd, keys, dir);
        if (pick.empty()) break;
        keys.quit = false; keys.esc_down = 0;
        play(pick);
        keys.quit = false; keys.esc_down = 0;       // Esc held in a game returns to the list
    }
    return 0;
}
