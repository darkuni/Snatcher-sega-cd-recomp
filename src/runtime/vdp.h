// Mega Drive VDP (YM7101): ports, DMA, and a per-scanline renderer to an RGB framebuffer.
#pragma once
#include <cstdint>

struct Vdp {
    uint8_t vram[0x10000];
    uint16_t cram[64];
    uint16_t vsram[40];
    uint8_t reg[32];

    bool pending = false;       // first word of a two-word command written
    uint16_t cmd1 = 0;
    uint8_t code = 0;           // CD5..CD0
    uint32_t addr = 0;
    bool fill_pending = false;  // DMA fill waits for the next data port write
    bool vint_flag = false;     // status bit 7 (F)
    int hcounter_line = 0;      // H-int counter (reg 10)

    static const int W = 320, H = 240;
    uint32_t fb[W * H];         // 0x00RRGGBB, visible area top-left
    int width() const { return (reg[12] & 0x81) ? 320 : 256; }
    int height() const { return (reg[1] & 0x08) ? 240 : 224; }

    void reset();
    uint16_t status(int line) const;
    void ctrl_write(uint16_t v);
    void data_write(uint16_t v);
    uint16_t data_read();
    void render_line(int line);
    bool vint_enabled() const { return reg[1] & 0x20; }
    bool hint_enabled() const { return reg[0] & 0x10; }

private:
    void do_dma();
    void write_target(uint16_t v);
    void advance() { addr = (addr + reg[15]) & 0xffff; }
};
