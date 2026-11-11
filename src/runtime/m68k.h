// 68000 CPU state and instruction semantics used by the recompiled code (src/gen).
#pragma once
#include <cstdint>

struct Bus {
    virtual uint8_t rd8(uint32_t a) = 0;
    virtual uint16_t rd16(uint32_t a) = 0;
    virtual void wr8(uint32_t a, uint8_t v) = 0;
    virtual void wr16(uint32_t a, uint16_t v) = 0;
    virtual ~Bus() {}
};

template <int S> struct SzT;
template <> struct SzT<1> { static constexpr uint32_t mask = 0xffu, msb = 0x80u; };
template <> struct SzT<2> { static constexpr uint32_t mask = 0xffffu, msb = 0x8000u; };
template <> struct SzT<4> { static constexpr uint32_t mask = 0xffffffffu, msb = 0x80000000u; };

struct Cpu {
    uint32_t d[8] = {}, a[8] = {};
    bool x = false, n = false, z = false, v = false, c = false;
    bool s = true, t = false;
    uint8_t imask = 7;
    uint32_t usp = 0, ssp = 0;
    uint32_t ret = 0;            // address an rts/rte returned to (see src/recomp.py)
    int64_t cycles = 0, limit = 0;
    int id = 0;                  // 0 = main CPU, 1 = sub CPU
    uint32_t dbg_pc = 0;         // last interpreted pc (debug)
    Bus* bus = nullptr;
    uint32_t expect[512];        // return addresses host callers are waiting for (interpreter fallback)
    int nexpect = 0;
    void enter(uint32_t r) { if (nexpect < 512) expect[nexpect] = r; nexpect++; }
    void leave() { nexpect--; }

    // memory (24-bit bus) --------------------------------------------------
    uint32_t rd8(uint32_t ad) { return bus->rd8(ad & 0xffffff); }
    uint32_t rd16(uint32_t ad) { return bus->rd16(ad & 0xfffffe); }
    uint32_t rd32(uint32_t ad) { return (rd16(ad) << 16) | rd16(ad + 2); }
    void wr8(uint32_t ad, uint32_t val) { bus->wr8(ad & 0xffffff, (uint8_t)val); }
    void wr16(uint32_t ad, uint32_t val) { bus->wr16(ad & 0xfffffe, (uint16_t)val); }
    void wr32(uint32_t ad, uint32_t val) { wr16(ad, val >> 16); wr16(ad + 2, val); }
    void push16(uint32_t val) { a[7] -= 2; wr16(a[7], val); }
    void push32(uint32_t val) { a[7] -= 4; wr32(a[7], val); }
    uint32_t pop16() { uint32_t r = rd16(a[7]); a[7] += 2; return r; }
    uint32_t pop32() { uint32_t r = rd32(a[7]); a[7] += 4; return r; }

    // status register -------------------------------------------------------
    uint32_t get_ccr() const { return (x << 4) | (n << 3) | (z << 2) | (v << 1) | (uint32_t)c; }
    void set_ccr(uint32_t r) { x = r & 16; n = r & 8; z = r & 4; v = r & 2; c = r & 1; }
    uint32_t get_sr() const { return (t << 15) | (s << 13) | (imask << 8) | get_ccr(); }
    void set_sr(uint32_t r) {
        set_ccr(r);
        t = r & 0x8000;
        imask = (r >> 8) & 7;
        bool ns = r & 0x2000;
        if (ns != s) {          // swap stack pointers
            if (s) { ssp = a[7]; a[7] = usp; } else { usp = a[7]; a[7] = ssp; }
            s = ns;
        }
    }

    // conditions -------------------------------------------------------------
    bool cond_hi() const { return !c && !z; }
    bool cond_ls() const { return c || z; }
    bool cond_cc() const { return !c; }
    bool cond_cs() const { return c; }
    bool cond_ne() const { return !z; }
    bool cond_eq() const { return z; }
    bool cond_vc() const { return !v; }
    bool cond_vs() const { return v; }
    bool cond_pl() const { return !n; }
    bool cond_mi() const { return n; }
    bool cond_ge() const { return n == v; }
    bool cond_lt() const { return n != v; }
    bool cond_gt() const { return !z && n == v; }
    bool cond_le() const { return z || n != v; }

    // arithmetic / logic -----------------------------------------------------
    template <int S> void nz(uint32_t r) {
        r &= SzT<S>::mask;
        n = r & SzT<S>::msb;
        z = r == 0;
    }
    template <int S> void logic(uint32_t r) { nz<S>(r); v = false; c = false; }
    template <int S> uint32_t logic_and(uint32_t s_, uint32_t d_) { uint32_t r = (s_ & d_) & SzT<S>::mask; logic<S>(r); return r; }
    template <int S> uint32_t logic_or(uint32_t s_, uint32_t d_) { uint32_t r = (s_ | d_) & SzT<S>::mask; logic<S>(r); return r; }
    template <int S> uint32_t logic_eor(uint32_t s_, uint32_t d_) { uint32_t r = (s_ ^ d_) & SzT<S>::mask; logic<S>(r); return r; }

    template <int S> uint32_t add(uint32_t s_, uint32_t d_) {
        const uint32_t m = SzT<S>::mask, h = SzT<S>::msb;
        s_ &= m; d_ &= m;
        uint64_t w = (uint64_t)s_ + d_;
        uint32_t r = (uint32_t)w & m;
        nz<S>(r);
        v = ((s_ ^ r) & (d_ ^ r) & h) != 0;
        c = x = (w >> (8 * S)) & 1;
        return r;
    }
    template <int S> uint32_t sub(uint32_t s_, uint32_t d_) {
        const uint32_t m = SzT<S>::mask, h = SzT<S>::msb;
        s_ &= m; d_ &= m;
        uint32_t r = (d_ - s_) & m;
        nz<S>(r);
        v = ((s_ ^ d_) & (r ^ d_) & h) != 0;
        c = x = s_ > d_;
        return r;
    }
    template <int S> void cmp(uint32_t s_, uint32_t d_) {
        bool ox = x;
        sub<S>(s_, d_);
        x = ox;
    }
    template <int S> uint32_t addx(uint32_t s_, uint32_t d_) {
        const uint32_t m = SzT<S>::mask, h = SzT<S>::msb;
        s_ &= m; d_ &= m;
        uint64_t w = (uint64_t)s_ + d_ + (x ? 1 : 0);
        uint32_t r = (uint32_t)w & m;
        n = r & h;
        if (r) z = false;
        v = ((s_ ^ r) & (d_ ^ r) & h) != 0;
        c = x = (w >> (8 * S)) & 1;
        return r;
    }
    template <int S> uint32_t subx(uint32_t s_, uint32_t d_) {
        const uint32_t m = SzT<S>::mask, h = SzT<S>::msb;
        s_ &= m; d_ &= m;
        uint64_t w = (uint64_t)d_ - s_ - (x ? 1 : 0);
        uint32_t r = (uint32_t)w & m;
        n = r & h;
        if (r) z = false;
        v = ((s_ ^ d_) & (r ^ d_) & h) != 0;
        c = x = (w >> 63) & 1;
        return r;
    }
    template <int S> uint32_t neg(uint32_t d_) { return sub<S>(d_, 0); }
    template <int S> uint32_t negx(uint32_t d_) { return subx<S>(d_, 0); }

    // BCD (N and V are undefined on the 68000; these match real hardware as Musashi models it)
    uint32_t abcd(uint32_t s_, uint32_t d_) {
        uint32_t res = (s_ & 15) + (d_ & 15) + (x ? 1 : 0);
        uint32_t fv = ~res;
        if (res > 9) res += 6;
        res += (s_ & 0xf0) + (d_ & 0xf0);
        c = x = res > 0x99;
        if (c) res -= 0xa0;
        fv &= res;
        v = fv & 0x80;
        n = res & 0x80;
        res &= 0xff;
        if (res) z = false;
        return res;
    }
    uint32_t sbcd(uint32_t s_, uint32_t d_) {
        uint32_t res = (d_ & 15) - (s_ & 15) - (x ? 1 : 0);
        uint32_t fv = ~res;
        if (res > 9) res -= 6;
        res += (d_ & 0xf0) - (s_ & 0xf0);
        c = x = res > 0x99;
        if (c) res += 0xa0;
        res &= 0xff;
        fv &= res;
        v = fv & 0x80;
        n = res & 0x80;
        if (res) z = false;
        return res;
    }
    template <int S> uint32_t nbcd(uint32_t d_) {
        uint32_t res = (0x9a - (d_ & 0xff) - (x ? 1 : 0)) & 0xff;
        if (res != 0x9a) {
            uint32_t fv = ~res;
            if ((res & 0x0f) == 0xa) res = (res & 0xf0) + 0x10;
            res &= 0xff;
            v = (fv & res) & 0x80;
            if (res) z = false;
            c = x = true;
            n = res & 0x80;
            return res;
        }
        v = c = x = false;
        n = res & 0x80;
        return d_ & 0xff;
    }

    // multiply / divide
    uint32_t mulu(uint32_t s_, uint32_t d_) { uint32_t r = (s_ & 0xffff) * (d_ & 0xffff); logic<4>(r); return r; }
    uint32_t muls(uint32_t s_, uint32_t d_) {
        uint32_t r = (uint32_t)((int32_t)(int16_t)s_ * (int32_t)(int16_t)d_);
        logic<4>(r);
        return r;
    }
    bool divu(uint32_t s_, uint32_t& d_, uint32_t next) {
        s_ &= 0xffff;
        if (!s_) { exception(5, next); return ret == next; }
        uint32_t q = d_ / s_, r = d_ % s_;
        if (q > 0xffff) { v = true; return true; }      // overflow: N, Z, C undefined - left as is
        c = false;
        d_ = (r << 16) | q;
        nz<2>(q); v = false;
        return true;
    }
    bool divs(uint32_t s_, uint32_t& d_, uint32_t next) {
        int32_t sv = (int16_t)s_;
        if (!sv) { exception(5, next); return ret == next; }
        int32_t dv = (int32_t)d_;
        if (dv == INT32_MIN && sv == -1) { v = true; return true; }     // quotient does not fit
        int32_t q = dv / sv, r = dv % sv;
        if (q < -32768 || q > 32767) { v = true; return true; }
        c = false;
        d_ = ((uint32_t)(uint16_t)r << 16) | (uint16_t)q;
        nz<2>((uint32_t)q); v = false;
        return true;
    }
    bool chk(uint32_t bound, uint32_t dv, uint32_t next) {
        int16_t b = (int16_t)bound, val = (int16_t)dv;
        if (val < 0) { n = true; exception(6, next); return ret == next; }
        if (val > b) { n = false; exception(6, next); return ret == next; }
        return true;
    }

    // shifts / rotates (cnt 0..63)
    template <int S> uint32_t asl(uint32_t cnt, uint32_t d_) {
        const uint32_t m = SzT<S>::mask, h = SzT<S>::msb;
        uint32_t r = d_ & m;
        bool vv = false;
        if (!cnt) { c = false; nz<S>(r); v = false; return r; }
        for (uint32_t k = 0; k < cnt; k++) {
            bool out = r & h;
            r = (r << 1) & m;
            if ((bool)(r & h) != out) vv = true;
            c = x = out;
        }
        v = vv; nz<S>(r);
        return r;
    }
    template <int S> uint32_t asr(uint32_t cnt, uint32_t d_) {
        const uint32_t m = SzT<S>::mask, h = SzT<S>::msb;
        uint32_t r = d_ & m;
        if (!cnt) { c = false; nz<S>(r); v = false; return r; }
        for (uint32_t k = 0; k < cnt; k++) { c = x = r & 1; r = (r >> 1) | (r & h); }
        v = false; nz<S>(r);
        return r;
    }
    template <int S> uint32_t lsl(uint32_t cnt, uint32_t d_) {
        const uint32_t m = SzT<S>::mask, h = SzT<S>::msb;
        uint32_t r = d_ & m;
        if (!cnt) { c = false; nz<S>(r); v = false; return r; }
        for (uint32_t k = 0; k < cnt; k++) { c = x = r & h; r = (r << 1) & m; }
        v = false; nz<S>(r);
        return r;
    }
    template <int S> uint32_t lsr(uint32_t cnt, uint32_t d_) {
        const uint32_t m = SzT<S>::mask;
        uint32_t r = d_ & m;
        if (!cnt) { c = false; nz<S>(r); v = false; return r; }
        for (uint32_t k = 0; k < cnt; k++) { c = x = r & 1; r >>= 1; }
        v = false; nz<S>(r);
        return r;
    }
    template <int S> uint32_t rol(uint32_t cnt, uint32_t d_) {
        const uint32_t m = SzT<S>::mask, h = SzT<S>::msb;
        uint32_t r = d_ & m;
        c = false;
        for (uint32_t k = 0; k < cnt; k++) { c = r & h; r = ((r << 1) | (c ? 1 : 0)) & m; }
        v = false; nz<S>(r);
        return r;
    }
    template <int S> uint32_t ror(uint32_t cnt, uint32_t d_) {
        const uint32_t m = SzT<S>::mask, h = SzT<S>::msb;
        uint32_t r = d_ & m;
        c = false;
        for (uint32_t k = 0; k < cnt; k++) { c = r & 1; r = (r >> 1) | (c ? h : 0); }
        v = false; nz<S>(r);
        return r;
    }
    template <int S> uint32_t roxl(uint32_t cnt, uint32_t d_) {
        const uint32_t m = SzT<S>::mask, h = SzT<S>::msb;
        uint32_t r = d_ & m;
        for (uint32_t k = 0; k < cnt; k++) { bool out = r & h; r = ((r << 1) | (x ? 1 : 0)) & m; x = out; }
        c = x; v = false; nz<S>(r);
        return r;
    }
    template <int S> uint32_t roxr(uint32_t cnt, uint32_t d_) {
        const uint32_t m = SzT<S>::mask, h = SzT<S>::msb;
        uint32_t r = d_ & m;
        for (uint32_t k = 0; k < cnt; k++) { bool out = r & 1; r = (r >> 1) | (x ? h : 0); x = out; }
        c = x; v = false; nz<S>(r);
        return r;
    }

    // control flow / system (implemented in cpu.cpp) ------------------------
    void jump(uint32_t target);                        // run guest code at target until it returns
    void miss(uint32_t pc);                            // entered a function at an unknown point
    bool sync(uint32_t resume);                        // hand off to the other CPU / hardware
    bool check_irq(uint32_t resume);                   // after SR writes: take a pending interrupt
    void exception(int vec, uint32_t resume);          // trap / illegal / divide by zero ...
    void rte();
    void stop(uint32_t resume);
    void reset_devices();
};
