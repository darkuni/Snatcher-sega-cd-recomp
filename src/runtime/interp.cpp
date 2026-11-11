// Fallback: run guest code the recompiler did not see with Musashi, one instruction at a time,
// until execution reaches recompiled code or an address a host caller is waiting for.
// Every address it starts at is logged (misses.txt) so the next recompile can include it.
#include <cstdio>
#include "system.h"
#include "table.h"
extern "C" {
#include "../third_party/musashi/m68k.h"
}

GuestFn find_fn(Cpu& c, uint32_t a);

static Cpu* s_cur = nullptr;
static bool s_init = false;

extern "C" {
unsigned int m68k_read_memory_8(unsigned int a) { return s_cur->rd8(a); }
unsigned int m68k_read_memory_16(unsigned int a) { return s_cur->rd16(a); }
unsigned int m68k_read_memory_32(unsigned int a) { return s_cur->rd32(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return s_cur->rd16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return s_cur->rd32(a); }
void m68k_write_memory_8(unsigned int a, unsigned int v) { s_cur->wr8(a, v); }
void m68k_write_memory_16(unsigned int a, unsigned int v) { s_cur->wr16(a, v); }
void m68k_write_memory_32(unsigned int a, unsigned int v) { s_cur->wr32(a, v); }
}

static void load(Cpu& c, uint32_t pc) {
    m68k_set_reg(M68K_REG_SR, c.get_sr());
    for (int r = 0; r < 8; r++) m68k_set_reg((m68k_register_t)(M68K_REG_D0 + r), c.d[r]);
    for (int r = 0; r < 8; r++) m68k_set_reg((m68k_register_t)(M68K_REG_A0 + r), c.a[r]);
    m68k_set_reg(c.s ? M68K_REG_USP : M68K_REG_ISP, c.s ? c.usp : c.ssp);
    m68k_set_reg(M68K_REG_PC, pc);
}

static uint32_t save(Cpu& c) {
    for (int r = 0; r < 8; r++) c.d[r] = m68k_get_reg(nullptr, (m68k_register_t)(M68K_REG_D0 + r));
    uint32_t sr = m68k_get_reg(nullptr, M68K_REG_SR);
    bool s = sr & 0x2000;
    // put the stack pointers where Cpu keeps them: a[7] = active one
    c.usp = m68k_get_reg(nullptr, M68K_REG_USP);
    c.ssp = m68k_get_reg(nullptr, M68K_REG_ISP);
    c.s = s;
    c.set_ccr(sr);
    c.t = sr & 0x8000;
    c.imask = (sr >> 8) & 7;
    for (int r = 0; r < 7; r++) c.a[r] = m68k_get_reg(nullptr, (m68k_register_t)(M68K_REG_A0 + r));
    c.a[7] = s ? c.ssp : c.usp;
    return m68k_get_reg(nullptr, M68K_REG_PC) & 0xffffff;
}

static bool expected(Cpu& c, uint32_t pc) {
    for (int k = c.nexpect - 1; k >= 0 && k >= c.nexpect - 64; k--)
        if (k < 512 && c.expect[k] == pc) return true;
    return false;
}

void interpret(Cpu& c, uint32_t pc) {
    if (!g.force_interp) g.log_miss(c.id, pc);
    if (!s_init) {
        s_cur = &c;
        m68k_init();
        m68k_set_cpu_type(M68K_CPU_TYPE_68000);
        m68k_pulse_reset();
        s_init = true;
    }
    uint32_t start = pc;
    for (;;) {
        if (pc != start || c.nexpect) {
            if (expected(c, pc)) { c.ret = pc; return; }
        }
        if (c.id == CPU_SUB && (pc == 0x5f00 || pc == 0x5f02)) { c.jump(pc); return; }   // BIOS return points
        if (pc != start) {
            if (!g.force_interp)
                if (GuestFn f = find_fn(c, pc)) { f(c, pc); return; }
            if (bios_call(c, pc)) {          // reached a BIOS entry (its jsr already pushed the return)
                pc = c.ret;
                start = ~0u;
                continue;
            }
        }
        s_cur = &c;
        load(c, pc);
        int cyc = m68k_execute(1);
        pc = save(c);
        c.dbg_pc = pc;
        c.cycles += cyc;
        start = ~0u;
        if (c.cycles >= c.limit && !c.sync(pc)) return;
    }
}
