// Control flow for recompiled code: dispatch, the two-CPU scheduler, interrupts and exceptions.
//
// Both CPUs run on their own host thread, but only one at a time: they take turns a scanline at
// a time (main 488 cycles at 7.67 MHz, sub 795 cycles at 12.5 MHz). At the end of each turn the
// sub thread advances the hardware by one line (V-blank, frame end) and hands back to main.
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include "system.h"
#include "table.h"

void interpret(Cpu& c, uint32_t pc);           // interp.cpp (Musashi fallback)
void frame_done();                              // main.cpp

static const int64_t MAIN_LINE = 488, SUB_LINE = 795;
static const uint32_t BIOS_RET = 0x005f00;      // sub BIOS: the module's main routine returned (call it again)
static const uint32_t INT2_RET = 0x005f02;      // sub BIOS: the module's int2 routine returned (rte)

// ---------------------------------------------------------------- dispatch

static bool image_live(Cpu& c, const ImageEntry& im) {
    for (int k = 0; k < 8; k++)
        if (c.rd32(im.base + 4 * k) != im.sig[k]) return false;
    return true;
}

static const char* s_skip = getenv("SNATCHER_INTERP_IMG");   // debug: interpret these images
static uint32_t s_rlo = getenv("SNATCHER_INTERP_LO") ? strtoul(getenv("SNATCHER_INTERP_LO"), nullptr, 16) : 0;
static uint32_t s_rhi = getenv("SNATCHER_INTERP_HI") ? strtoul(getenv("SNATCHER_INTERP_HI"), nullptr, 16) : 0;

GuestFn find_fn(Cpu& c, uint32_t a) {
    for (int i = 0; i < g_image_count; i++) {
        const ImageEntry& im = g_images[i];
        if (im.cpu != c.id || a < im.base || a >= im.end) continue;
        if (s_skip && strstr(s_skip, im.name)) continue;
        if (s_rlo && a >= s_rlo && a < s_rhi) continue;
        int lo = 0, hi = im.count - 1;
        GuestFn f = nullptr;
        while (lo <= hi) {
            int m = (lo + hi) / 2;
            if (im.funcs[m].addr == a) { f = im.funcs[m].fn; break; }
            if (im.funcs[m].addr < a) lo = m + 1; else hi = m - 1;
        }
        if (f && image_live(c, im)) return f;
    }
    return nullptr;
}

void Cpu::jump(uint32_t t) {
    t &= 0xffffff;
    if (id == CPU_SUB && t == INT2_RET) { rte(); return; }
    if (id == CPU_SUB && t == BIOS_RET) {
        push32(BIOS_RET);
        enter(BIOS_RET);
        jump(g.sp_entry[1]);
        leave();
        return;
    }
    if (bios_call(*this, t)) return;
    if (!g.force_interp)
        if (GuestFn f = find_fn(*this, t)) { f(*this, t); return; }
    interpret(*this, t);
}

void Cpu::miss(uint32_t pc) { interpret(*this, pc); }

// ---------------------------------------------------------------- scheduler

static std::mutex s_mtx;
static std::condition_variable s_cv;
static int s_turn = CPU_MAIN;


static bool sub_runs() { return !g.sub_reset && !g.sub_busreq; }

static void hw_line() {         // called with s_mtx held, once both CPUs finished the line
    g.main.limit += MAIN_LINE;
    g.sub.limit += SUB_LINE;
    if (++g.line == 224 && g.vdp.vint_enabled()) g.vint_pending = true;
    if (g.line == 262) {
        g.line = 0;
        g.frame++;
        frame_done();
    }
}

static bool take_interrupt(Cpu& c, uint32_t resume);

// Main CPU fired IFL2: the real sub CPU responds within microseconds, while main is still running.
// Give the sub its turn right away (main keeps its remaining cycles for this line).
void yield_to_sub() {
    if (!sub_runs()) return;
    std::unique_lock<std::mutex> lk(s_mtx);
    s_turn = CPU_SUB;
    s_cv.notify_all();
    s_cv.wait(lk, [] { return s_turn == CPU_MAIN; });
}

bool Cpu::sync(uint32_t resume) {
    if (cycles < limit) cycles = limit;         // idle for the rest of this line
    {
        std::unique_lock<std::mutex> lk(s_mtx);
        if (id == CPU_MAIN) {
            if (sub_runs()) {
                s_turn = CPU_SUB;
                s_cv.notify_all();
                s_cv.wait(lk, [] { return s_turn == CPU_MAIN; });
            } else {
                g.sub.cycles = g.sub.limit;     // a held sub CPU just loses its time
                hw_line();
            }
        } else {
            hw_line();
            s_turn = CPU_MAIN;
            s_cv.notify_all();
            s_cv.wait(lk, [] { return s_turn == CPU_SUB; });
        }
    }
    return take_interrupt(*this, resume);
}

// ---------------------------------------------------------------- interrupts and exceptions

static bool deliver(Cpu& c, int level, uint32_t handler, uint32_t resume, bool bios_wrap) {
    uint32_t sr = c.get_sr();
    c.set_sr((sr & 0x00ff) | 0x2000 | (level << 8));
    c.push32(resume);
    c.push16(sr);
    c.enter(resume);
    if (bios_wrap) {        // the sub BIOS calls the module's int2 routine as a subroutine, then rte
        c.push32(INT2_RET);
        c.enter(INT2_RET);
        c.jump(handler);
        c.leave();
        if (c.ret == INT2_RET) c.rte();
    } else {
        c.jump(handler);
    }
    c.leave();
    return c.ret == resume;
}

static bool take_interrupt(Cpu& c, uint32_t resume) {
    if (c.id == CPU_MAIN) {
        if (g.vint_pending && c.imask < 6 && g.main_vint) {
            g.vint_pending = false;
            g.n_vint++;
            return deliver(c, 6, g.main_vint, resume, false);
        }
    } else {
        if (g.sub_int2 && (g.sub_imask & 4) && c.imask < 2) {
            g.sub_int2 = false;
            g.n_int2++;
            return deliver(c, 2, g.sp_entry[2], resume, true);
        }
    }
    return true;
}

bool Cpu::check_irq(uint32_t resume) { return take_interrupt(*this, resume); }

void Cpu::exception(int vec, uint32_t resume) {
    uint32_t handler = rd32(vec * 4);
    fprintf(stderr, "[%s] exception %d at %06x -> %06x\n", id ? "sub" : "main", vec, resume, handler);
    uint32_t sr = get_sr();
    set_sr((sr & 0xffff) | 0x2000);
    push32(resume);
    push16(sr);
    if (!handler) { fprintf(stderr, "no handler, stopping\n"); exit(1); }
    enter(resume);
    jump(handler);
    leave();
}

void Cpu::rte() {
    uint32_t sr = pop16();
    uint32_t pc = pop32();
    set_sr(sr);
    ret = pc;
}

void Cpu::stop(uint32_t resume) {
    for (;;) {
        bool any = id == CPU_MAIN ? g.vint_pending : (g.sub_int2 && (g.sub_imask & 4));
        if (any) { take_interrupt(*this, resume); return; }
        cycles = limit;
        if (!sync(resume)) return;
    }
}

void Cpu::reset_devices() {}

// ---------------------------------------------------------------- CPU threads

static void wait_turn(int id) {
    std::unique_lock<std::mutex> lk(s_mtx);
    s_cv.wait(lk, [id] { return s_turn == id; });
}

static void top_level(Cpu& c, uint32_t pc) {
    static int logged = 0;
    for (;;) {
        c.nexpect = 0;
        c.jump(pc);
        pc = c.ret;
        if (getenv("SNATCHER_TRACE_TOP") && logged++ < 40)
            fprintf(stderr, "[%s] top-level dispatch -> %06x (frame %llu line %d)\n", c.id ? "sub" : "main", pc,
                    (unsigned long long)g.frame, g.line);
    }
}

static void main_thread() {
    wait_turn(CPU_MAIN);
    top_level(g.main, 0xff0000);
}

static void sub_thread() {
    wait_turn(CPU_SUB);
    Cpu& c = g.sub;
    c.push32(BIOS_RET);         // BIOS: call the module's init routine, then its main routine forever
    c.enter(BIOS_RET);
    c.jump(g.sp_entry[0]);
    c.leave();
    c.set_sr(0x2000);
    for (;;) {
        c.nexpect = 0;
        c.push32(BIOS_RET);
        c.enter(BIOS_RET);
        c.jump(g.sp_entry[1]);
        c.leave();
        if (c.ret != BIOS_RET) top_level(c, c.ret);
    }
}

void run_machine() {
    g.main.limit = MAIN_LINE;
    g.sub.limit = SUB_LINE;
    std::thread s(sub_thread);
    main_thread();
    s.join();
}
