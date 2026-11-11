// Front end. Two modes:
//   snatcher <disc.cue>            window (SDL2): video, audio, keyboard / game controller
//   snatcher <disc.cue> <frames>   headless test run: N frames, then misses.txt and stats
// (a folder made by tools/prepare_disc.py works in place of the .cue, for development)
// The emulated CPUs run on worker threads; this thread owns SDL and paces frames at 59.92 Hz.
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <filesystem>
#include <vector>
#include "audio.h"
#include "system.h"
#ifdef USE_SDL
#include <SDL.h>
#endif

void run_machine();
void print_profile();

static uint64_t s_frames = 0;           // headless: stop after this many
static bool s_headless = true;

// ---------------------------------------------------------------- frame hand-off (emu -> host)
static std::mutex s_fm;
static std::condition_variable s_fcv;
static uint32_t s_fb[Vdp::W * Vdp::H];
static int s_fw = 256, s_fh = 224;
static bool s_frame_ready = false, s_go = true;
static std::vector<int16_t> s_audio;

static void dump_frame() {
    static int every = getenv("SNATCHER_DUMP") ? atoi(getenv("SNATCHER_DUMP")) : 0;
    if (!every || g.frame % every) return;
    char name[64];
    snprintf(name, sizeof name, "frame_%05llu.ppm", (unsigned long long)g.frame);
    FILE* f = fopen(name, "wb");
    if (!f) return;
    int w = g.vdp.width(), h = g.vdp.height();
    fprintf(f, "P6 %d %d 255\n", w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t c = g.vdp.fb[y * Vdp::W + x];
            uint8_t px[3] = {(uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
            fwrite(px, 1, 3, f);
        }
    fclose(f);
}

extern uint32_t g_pcm_writes, g_pcm_on;
extern uint32_t g_fm_writes, g_psg_writes;
static void headless_report() {
    if (g.frame % 60 == 0)
        fprintf(stderr, "frame %llu  vint %u int2 %u sectors %u  cd track %d pos %u  pcm w%u on%u fm w%u psg w%u  main loop %06x sub loop %06x\n",
                (unsigned long long)g.frame, g.n_vint, g.n_int2, g.n_sectors, g.cd.audio_track, g.cd.pos, g_pcm_writes, g_pcm_on, g_fm_writes, g_psg_writes,
                g.main.dbg_pc, g.sub.dbg_pc);
    if (getenv("SNATCHER_COMM") && g.frame % 30 == 0)
        fprintf(stderr, "  f%llu cmd %04x %04x %04x %04x stat %04x %04x %04x %04x flags %02x/%02x ret=%d dmna=%d\n",
                (unsigned long long)g.frame, g.cmd[0], g.cmd[1], g.cmd[2], g.cmd[3], g.stat[0], g.stat[1], g.stat[2], g.stat[3],
                g.flag_main, g.flag_sub, g.ret, g.dmna);
    if (getenv("SNATCHER_COMM") && g.frame % 30 == 0)
        fprintf(stderr, "    sub $1511e=%02x $9b44=%02x $7f80c=%02x $7f80d=%02x $7f80e=%02x $7f80f=%02x $9808=%04x\n",
                g.prg[0x1511e], g.prg[0x9b44], g.prg[0x7f80c], g.prg[0x7f80d], g.prg[0x7f80e], g.prg[0x7f80f], be16(&g.prg[0x9808]));
    if (getenv("SNATCHER_COMM") && g.frame % 300 == 0) {
        fprintf(stderr, "    seq idx $9b40=%04x list:", be16(&g.prg[0x9b40]));
        for (int k = 0; k < 16; k++) fprintf(stderr, " %04x", be16(&g.prg[0x9940 + 2 * k]));
        fprintf(stderr, "  text $9720=%02x $9722=%02x $9727=%02x $9740=%02x $7991=%02x\n", g.prg[0x9720], g.prg[0x9722], g.prg[0x9727], g.prg[0x9740], g.prg[0x7991]);
    }
    if (g.frame < s_frames) return;
    FILE* f = fopen("misses.txt", "w");
    for (int c = 0; c < 2; c++)
        for (uint32_t a : g.misses[c]) if (f) fprintf(f, "%s %06x\n", c ? "sub" : "main", a);
    if (f) fclose(f);
    fprintf(stderr, "done: %llu frames, %zu main / %zu sub addresses interpreted (misses.txt)\n",
            (unsigned long long)g.frame, g.misses[0].size(), g.misses[1].size());
    print_profile();
    if (FILE* v = fopen("vdp.bin", "wb")) {         // VDP state for offline inspection
        fwrite(g.vdp.vram, 1, 0x10000, v); fwrite(g.vdp.cram, 2, 64, v); fwrite(g.vdp.vsram, 2, 40, v);
        fwrite(g.vdp.reg, 1, 32, v); fclose(v);
    }
    fflush(stderr);
    _Exit(0);
}

// SNATCHER_INPUT="1300:80,1306:00,..." - at frame N set the pad to hex buttons (U D L R B C A Start)
static void scripted_input() {
    static const char* s = getenv("SNATCHER_INPUT");
    if (!s) return;
    for (const char* p = s; *p;) {
        unsigned long f = strtoul(p, (char**)&p, 10);
        if (*p != ':') break;
        unsigned long b = strtoul(p + 1, (char**)&p, 16);
        if (f == g.frame) g.pad = (uint8_t)b;
        if (*p == ',') p++;
    }
}

void frame_done() {                     // emu side, once per frame (scheduler lock held)
    dump_frame();
    scripted_input();
    std::vector<int16_t> a;
    audio_frame(a);
    if (s_headless) {
        static FILE* wav = getenv("SNATCHER_WAV") ? fopen(getenv("SNATCHER_WAV"), "wb") : nullptr;   // raw s16le stereo 44.1k
        if (wav) { fwrite(a.data(), 2, a.size(), wav); fflush(wav); }
        headless_report();
        return;
    }
    std::unique_lock<std::mutex> lk(s_fm);
    s_fcv.wait(lk, [] { return s_go; });        // host paces us
    s_go = false;
    memcpy(s_fb, g.vdp.fb, sizeof s_fb);
    s_fw = g.vdp.width();
    s_fh = g.vdp.height();
    s_audio.insert(s_audio.end(), a.begin(), a.end());
    s_frame_ready = true;
    s_fcv.notify_all();
}

#ifdef USE_SDL
static uint8_t keys_to_pad(const uint8_t* k, SDL_GameController* gc) {
    uint8_t p = 0;
    auto btn = [&](SDL_GameControllerButton b) { return gc && SDL_GameControllerGetButton(gc, b); };
    int ax = gc ? SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX) : 0;
    int ay = gc ? SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY) : 0;
    if (k[SDL_SCANCODE_UP] || btn(SDL_CONTROLLER_BUTTON_DPAD_UP) || ay < -16000) p |= 0x01;
    if (k[SDL_SCANCODE_DOWN] || btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN) || ay > 16000) p |= 0x02;
    if (k[SDL_SCANCODE_LEFT] || btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT) || ax < -16000) p |= 0x04;
    if (k[SDL_SCANCODE_RIGHT] || btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || ax > 16000) p |= 0x08;
    // Snatcher: A draws the Blaster, B cancels, C selects / shoots
    if (k[SDL_SCANCODE_X] || btn(SDL_CONTROLLER_BUTTON_B)) p |= 0x10;          // B: cancel
    if (k[SDL_SCANCODE_C] || k[SDL_SCANCODE_SPACE] || btn(SDL_CONTROLLER_BUTTON_A)) p |= 0x20;   // C: select
    if (k[SDL_SCANCODE_Z] || btn(SDL_CONTROLLER_BUTTON_X)) p |= 0x40;          // A: Blaster
    if (k[SDL_SCANCODE_RETURN] || btn(SDL_CONTROLLER_BUTTON_START)) p |= 0x80;
    return p;
}

static int run_window() {
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("Snatcher", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 320 * 3, 224 * 3,
                                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, Vdp::W, Vdp::H);
    SDL_AudioSpec want{}, have{};
    want.freq = 44100; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (dev) SDL_PauseAudioDevice(dev, 0);
    SDL_GameController* gc = nullptr;

    std::thread emu(run_machine);
    emu.detach();

    const double frame_s = 1.0 / 59.922743;
    uint64_t freq = SDL_GetPerformanceFrequency(), next = SDL_GetPerformanceCounter();
    bool fullscreen = false;
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { SDL_Quit(); _Exit(0); }
            if (e.type == SDL_CONTROLLERDEVICEADDED && !gc) gc = SDL_GameControllerOpen(e.cdevice.which);
            if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F11) {
                fullscreen = !fullscreen;
                SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
            }
            if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE && fullscreen) {
                fullscreen = false;
                SDL_SetWindowFullscreen(win, 0);
            }
        }
        g.pad = keys_to_pad(SDL_GetKeyboardState(nullptr), gc);
        // let the emulator produce one frame
        std::vector<int16_t> a;
        int fw, fh;
        {
            std::unique_lock<std::mutex> lk(s_fm);
            s_go = true;
            s_fcv.notify_all();
            s_fcv.wait(lk, [] { return s_frame_ready; });
            s_frame_ready = false;
            SDL_UpdateTexture(tex, nullptr, s_fb, Vdp::W * 4);
            a.swap(s_audio);
            fw = s_fw; fh = s_fh;
        }
        if (dev) {
            if (SDL_GetQueuedAudioSize(dev) < 44100 * 4 / 4)       // keep about 250 ms at most
                SDL_QueueAudio(dev, a.data(), (Uint32)(a.size() * 2));
        }
        // 4:3 picture, centred
        int ww, wh;
        SDL_GetRendererOutputSize(ren, &ww, &wh);
        int h = wh, w = h * 4 / 3;
        if (w > ww) { w = ww; h = w * 3 / 4; }
        SDL_Rect src{0, 0, fw, fh}, dst{(ww - w) / 2, (wh - h) / 2, w, h};
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, &src, &dst);
        SDL_RenderPresent(ren);
        // pace to 59.92 Hz (vsync may already have waited)
        next += (uint64_t)(frame_s * freq);
        uint64_t now = SDL_GetPerformanceCounter();
        if (now < next) SDL_Delay((Uint32)((next - now) * 1000 / freq));
        else if (now - next > freq / 4) next = now;           // fell far behind: resync
    }
}
#endif

// No path given: look for a .cue next to the program, in ./game, or in the current folder.
static std::string find_disc(const char* argv0) {
    namespace fs = std::filesystem;
    std::vector<fs::path> dirs;
    std::error_code ec;
    fs::path exe = fs::absolute(argv0, ec).parent_path();
    dirs.push_back(exe); dirs.push_back(exe / "game"); dirs.push_back(fs::current_path(ec)); dirs.push_back("game");
    for (auto& d : dirs) {
        for (auto it = fs::directory_iterator(d, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::string ext = it->path().extension().string();
            for (auto& ch : ext) ch = (char)tolower(ch);
            if (ext == ".cue") return it->path().string();
        }
        ec.clear();
    }
    return "";
}

int main(int argc, char** argv) {
    std::string disc = argc > 1 ? argv[1] : find_disc(argv[0]);
    if (disc.empty()) {
        const char* msg = "Put your Snatcher (Sega CD, US) disc image - the .cue file and its .bin - next to this "
                          "program or in a \"game\" folder, or drag the .cue onto the program.";
        fprintf(stderr, "%s\n", msg);
#ifdef USE_SDL
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Snatcher", msg, nullptr);
#endif
        return 1;
    }
    if (!g.boot(disc)) {
#ifdef USE_SDL
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Snatcher", ("Could not read the disc image:\n" + disc).c_str(), nullptr);
#endif
        return 1;
    }
    g.force_interp = getenv("SNATCHER_INTERP") != nullptr;
    if (argc > 2) {
        s_frames = strtoull(argv[2], nullptr, 10);
        run_machine();
        return 0;
    }
#ifdef USE_SDL
    s_headless = false;
    return run_window();
#else
    fprintf(stderr, "built without SDL: give a frame count for a headless run\n");
    return 1;
#endif
}
