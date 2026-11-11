// Differential test: Musashi (reference 68000 interpreter) vs recompiled snippets. Built by run.sh.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
extern "C" {
#include "m68k.h"
}
#include "../../src/runtime/m68k.h"

struct Case { uint8_t code[80]; int len; uint32_t end; void (*fn)(Cpu&, uint32_t); };
#include "cases.inc"

static uint8_t memA[1 << 24], memB[1 << 24];
static std::set<uint32_t> written;

extern "C" {
unsigned int m68k_read_memory_8(unsigned int a) { return memA[a & 0xffffff]; }
unsigned int m68k_read_memory_16(unsigned int a) { a &= 0xfffffe; return (memA[a] << 8) | memA[a + 1]; }
unsigned int m68k_read_memory_32(unsigned int a) { return (m68k_read_memory_16(a) << 16) | m68k_read_memory_16(a + 2); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }
void m68k_write_memory_8(unsigned int a, unsigned int v) { a &= 0xffffff; memA[a] = v; written.insert(a); }
void m68k_write_memory_16(unsigned int a, unsigned int v) {
    a &= 0xfffffe; memA[a] = v >> 8; memA[a + 1] = v; written.insert(a); written.insert(a + 1);
}
void m68k_write_memory_32(unsigned int a, unsigned int v) { m68k_write_memory_16(a, v >> 16); m68k_write_memory_16(a + 2, v); }
}

struct TBus : Bus {
    uint8_t rd8(uint32_t a) override { return memB[a]; }
    uint16_t rd16(uint32_t a) override { return (memB[a] << 8) | memB[a + 1]; }
    void wr8(uint32_t a, uint8_t v) override { memB[a] = v; written.insert(a); }
    void wr16(uint32_t a, uint16_t v) override { memB[a] = v >> 8; memB[a + 1] = v; written.insert(a); written.insert(a + 1); }
};

static uint32_t g_jumped;
void Cpu::jump(uint32_t t) { g_jumped = t; ret = 0xdead; }
void Cpu::miss(uint32_t) { abort(); }
bool Cpu::sync(uint32_t) { return true; }
bool Cpu::check_irq(uint32_t) { return true; }
void Cpu::exception(int, uint32_t) { ret = 0xbad; }
void Cpu::rte() { abort(); }
void Cpu::stop(uint32_t) { abort(); }
void Cpu::reset_devices() {}

static uint32_t rnd() {
    static uint64_t s = 88172645463325252ull;
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    return (uint32_t)s;
}

int main(int argc, char** argv) {
    int ncase = sizeof(g_cases) / sizeof(g_cases[0]), fails = 0, skipped = 0, reps = argc > 1 ? atoi(argv[1]) : 4;
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    for (int ci = 0; ci < ncase; ci++) {
        const Case& k = g_cases[ci];
        for (int rep = 0; rep < reps; rep++) {
            uint32_t dr[8], ar[8];
            for (int r = 0; r < 8; r++) dr[r] = rep & 1 ? rnd() : (rnd() & 0x1ff) - 0x100;
            for (int r = 0; r < 7; r++) ar[r] = 0x100000 + (rnd() & 0xfffe) + r * 0x20000;
            ar[7] = 0x700000;
            for (uint32_t a : written) { memA[a] = 0; memB[a] = 0; }
            written.clear();
            for (int r = 0; r < 7; r++)
                for (int o = -0x100; o < 0x200; o++) {
                    uint8_t v = rnd();
                    memA[ar[r] + o] = v; memB[ar[r] + o] = v; written.insert(ar[r] + o);
                }
            for (int o = 0; o < k.len; o++) memA[0x800000 + o] = memB[0x800000 + o] = k.code[o];
            uint32_t ccr = rnd() & 0x1f;
            // reference
            m68k_pulse_reset();
            for (int r = 0; r < 8; r++) m68k_set_reg((m68k_register_t)(M68K_REG_D0 + r), dr[r]);
            for (int r = 0; r < 8; r++) m68k_set_reg((m68k_register_t)(M68K_REG_A0 + r), ar[r]);
            m68k_set_reg(M68K_REG_SR, 0x2700 | ccr);
            m68k_set_reg(M68K_REG_PC, 0x800000);
            for (int guard = 0; guard < 64 && m68k_get_reg(NULL, M68K_REG_PC) != k.end; guard++) m68k_execute(1);
            // recompiled
            Cpu c; TBus bus; c.bus = &bus;
            for (int r = 0; r < 8; r++) { c.d[r] = dr[r]; c.a[r] = ar[r]; }
            c.set_ccr(ccr);
            g_jumped = 0;
            k.fn(c, 0x800000);
            if (c.ret == 0xbad) { skipped++; break; }      // took an exception (divide by zero)
            bool bad = g_jumped != k.end;
            for (int r = 0; r < 8; r++) {
                if (c.d[r] != m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_D0 + r))) bad = true;
                if (c.a[r] != m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_A0 + r))) bad = true;
            }
            uint32_t sr = m68k_get_reg(NULL, M68K_REG_SR);
            if ((sr & 0x1f) != c.get_ccr()) bad = true;
            int memdiff = 0;
            for (uint32_t a : written) if (memA[a] != memB[a]) { memdiff++; bad = true; }
            if (bad) {
                if (++fails <= 40) {
                    char buf[100];
                    printf("case %d rep %d: ccr in %02x ref %02x got %02x memdiff %d jumped %06x\n",
                           ci, rep, ccr, sr & 0x1f, c.get_ccr(), memdiff, g_jumped);
                    for (uint32_t pc = 0x800000; pc < k.end;) {
                        pc += m68k_disassemble(buf, pc, M68K_CPU_TYPE_68000);
                        printf("    %s\n", buf);
                    }
                    for (int r = 0; r < 8; r++) {
                        uint32_t rd = m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_D0 + r));
                        uint32_t ra = m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_A0 + r));
                        if (rd != c.d[r]) printf("    d%d in %08x ref %08x got %08x\n", r, dr[r], rd, c.d[r]);
                        if (ra != c.a[r]) printf("    a%d in %08x ref %08x got %08x\n", r, ar[r], ra, c.a[r]);
                    }
                }
                break;
            }
        }
    }
    printf("%d cases x %d, %d failing cases, %d skipped (exception)\n", ncase, reps, fails, skipped);
    return fails != 0;
}
