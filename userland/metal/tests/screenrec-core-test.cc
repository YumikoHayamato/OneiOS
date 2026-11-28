// Host test of ScreenRecord.inc's pixel core: c++ -std=c++17 -O2 -I.. screenrec-core-test.cc && ./a.out
#define ONEIOS_SCREENREC_CORE_ONLY 1
#include "../ScreenRecord.inc"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace oneios_screenrec;
static uint8_t S[1 << 20];
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
static uint16_t floatToHalf(float f) { // enough for 0..1
    uint32_t b; memcpy(&b, &f, 4); int e = ((b >> 23) & 0xff) - 127 + 15; if (f == 0) return 0;
    return (uint16_t)(e << 10 | ((b >> 13) & 0x3ff));
}
int main() {
    // BGRA 4x2 -> BGRA 2x1: 2x2 box average
    std::vector<uint8_t> s(4 * 2 * 4), d(2 * 1 * 4, 0);
    for (int y = 0; y < 2; y++) for (int x = 0; x < 4; x++) { uint8_t *p = &s[(y * 4 + x) * 4]; p[0] = 10 * x; p[1] = 100; p[2] = 200 + y; p[3] = 255; }
    Image is{kBGRA, 1, {{s.data(), 16, 4, 2}}}, id{kBGRA, 1, {{d.data(), 8, 2, 1}}};
    Rect rs, rd;
    CHECK(rectsFor(nullptr, 4, 2, 2, 1, rs, rd));
    CHECK(copyRect(is, rs, id, rd, S));
    CHECK(d[0] == 5 && d[1] == 100 && d[2] == 201 && d[3] == 255 && d[4] == 25);
    // Rects in 16.16 fixed point, clipped
    TransformRects t{0, 0, (uint64_t)4 << 16, (uint64_t)2 << 16, 0, 0, 2, 1};
    CHECK(rectsFor(&t, 4, 2, 2, 1, rs, rd) && rs.w == 4 && rs.h == 2 && rd.w == 2 && rd.h == 1);
    TransformRects t2{(uint64_t)10 << 16, 0, (uint64_t)4 << 16, (uint64_t)2 << 16, 0, 0, 2, 1};
    CHECK(!rectsFor(&t2, 4, 2, 2, 1, rs, rd));
    // White and black to 420v / 420f
    const int W = 8, H = 4;
    std::vector<uint8_t> sw(W * H * 4, 255), yv(W * H), cv(W * H / 2);
    Image white{kBGRA, 1, {{sw.data(), W * 4, W, H}}};
    Image v420{k420v, 2, {{yv.data(), W, W, H}, {cv.data(), W, W / 2, H / 2}}};
    rectsFor(nullptr, W, H, W, H, rs, rd);
    CHECK(copyRect(white, rs, v420, rd, S));
    CHECK(yv[0] == 235 && cv[0] == 128 && cv[1] == 128);
    Image f420 = v420; f420.format = k420f;
    CHECK(copyRect(white, rs, f420, rd, S) && yv[0] == 255 && cv[0] == 128);
    std::vector<uint8_t> sb(W * H * 4, 0); for (int i = 3; i < W * H * 4; i += 4) sb[i] = 255;
    Image black{kBGRA, 1, {{sb.data(), W * 4, W, H}}};
    CHECK(copyRect(black, rs, v420, rd, S) && yv[5] == 16 && cv[2] == 128);
    // pure red, video range BT.709: Y 63, Cb 102, Cr 240
    for (int i = 0; i < W * H; i++) { sb[i * 4 + 2] = 255; }
    CHECK(copyRect(black, rs, v420, rd, S));
    printf("red 420v: Y %d Cb %d Cr %d\n", yv[0], cv[0], cv[1]);
    CHECK(abs(yv[0] - 63) <= 1 && abs(cv[0] - 102) <= 1 && abs(cv[1] - 240) <= 1);
    // 10-bit x420: white Y = 940 << 6
    std::vector<uint16_t> y10(W * H), c10(W * H / 2);
    Image x420{kx420, 2, {{(uint8_t *)y10.data(), W * 2, W, H}, {(uint8_t *)c10.data(), W * 2, W / 2, H / 2}}};
    CHECK(copyRect(white, rs, x420, rd, S) && y10[0] == (940 << 6) && c10[0] == (512 << 6));
    // RGhA source: linear 1.0 white, linear 0.214 -> sRGB ~128
    std::vector<uint16_t> h(W * H * 4);
    for (int i = 0; i < W * H; i++) { h[i * 4] = floatToHalf(1.f); h[i * 4 + 1] = floatToHalf(0.2140f); h[i * 4 + 2] = 0; h[i * 4 + 3] = floatToHalf(1.f); }
    Image hs{kRGhA, 1, {{(uint8_t *)h.data(), W * 8, W, H}}};
    std::vector<uint8_t> o(W * H * 4);
    Image ob{kBGRA, 1, {{o.data(), W * 4, W, H}}};
    CHECK(copyRect(hs, rs, ob, rd, S));
    printf("RGhA -> BGRA: B %d G %d R %d\n", o[0], o[1], o[2]);
    CHECK(o[2] == 255 && abs(o[1] - 128) <= 1 && o[0] == 0);
    CHECK(halfToFloat(0x0001) > 5.9e-8f && halfToFloat(0x0001) < 6.0e-8f);
    // refused formats
    Image bad = white; bad.format = fourcc('&', 'b', '8', 'v');
    CHECK(!copyRect(white, rs, bad, rd, S));
    // odd sizes and nearest scaling: 1440x3120 -> 721x1561 into 420v, no out-of-bounds (ASan run below)
    {
        const size_t SW = 1440, SH = 3120, DW = 721, DH = 1561;
        std::vector<uint8_t> src(SW * SH * 4, 77), Y(DW * DH), C(((DW + 1) / 2) * 2 * ((DH + 1) / 2));
        Image a{kBGRA, 1, {{src.data(), SW * 4, SW, SH}}};
        Image b{k420v, 2, {{Y.data(), DW, DW, DH}, {C.data(), ((DW + 1) / 2) * 2, (DW + 1) / 2, (DH + 1) / 2}}};
        rectsFor(nullptr, SW, SH, DW, DH, rs, rd);
        CHECK(copyRect(a, rs, b, rd, S));
    }
    // speed: the phone's case, 1440x3120 BGRA -> 720x1560 420v (box filter)
    {
        const size_t SW = 1440, SH = 3120, DW = 720, DH = 1560;
        std::vector<uint8_t> src(SW * SH * 4), Y(DW * DH), C(DW * DH / 2);
        for (size_t i = 0; i < src.size(); i++) src[i] = (uint8_t)(i * 7);
        Image a{kBGRA, 1, {{src.data(), SW * 4, SW, SH}}};
        Image b{k420v, 2, {{Y.data(), DW, DW, DH}, {C.data(), DW, DW / 2, DH / 2}}};
        rectsFor(nullptr, SW, SH, DW, DH, rs, rd);
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 20; i++) copyRect(a, rs, b, rd, S);
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count() / 20;
        Image c{kBGRA, 1, {{Y.data(), DW * 4, DW, DH / 4}}};
        printf("1440x3120 BGRA -> 720x1560 420v: %lld us a frame (this PC, one thread)\n", (long long)us);
        rectsFor(nullptr, SW, SH, SW, SH, rs, rd);
        std::vector<uint8_t> Y2(SW * SH), C2(SW * SH / 2);
        Image b2{k420v, 2, {{Y2.data(), SW, SW, SH}, {C2.data(), SW, SW / 2, SH / 2}}};
        t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 5; i++) copyRect(a, rs, b2, rd, S);
        us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count() / 5;
        printf("1440x3120 BGRA -> 1440x3120 420v: %lld us a frame (nearest, 1:1)\n", (long long)us);
        (void)c;
    }
    // bands give the same bytes as one pass (odd height, 420v and BGRA)
    {
        const size_t SW = 1440, SH = 3120, DW = 720, DH = 1559;
        std::vector<uint8_t> src(SW * SH * 4);
        for (size_t i = 0; i < src.size(); i++) src[i] = (uint8_t)(i * 13 + (i >> 9));
        Image a{kBGRA, 1, {{src.data(), SW * 4, SW, SH}}};
        for (uint32_t f : {k420v, kBGRA}) {
            const size_t cs = DW, chs = (DH + 1) / 2;
            std::vector<uint8_t> Y1(DW * DH * 4), C1(cs * chs), Y2(DW * DH * 4), C2(cs * chs);
            Image b1{f, f == kBGRA ? 1u : 2u, {{Y1.data(), f == kBGRA ? DW * 4 : DW, DW, DH}, {C1.data(), cs, DW / 2, chs}}};
            Image b2{f, f == kBGRA ? 1u : 2u, {{Y2.data(), f == kBGRA ? DW * 4 : DW, DW, DH}, {C2.data(), cs, DW / 2, chs}}};
            rectsFor(nullptr, SW, SH, DW, DH, rs, rd);
            CHECK(copyRect(a, rs, b1, rd, S));
            const size_t rows = ((DH + 3) / 4 + 1) & ~(size_t)1;
            static uint8_t S2[4][1 << 16];
            for (size_t i = 0; i < 4; i++) CHECK(i * rows >= DH || copyRect(a, rs, b2, rd, S2[i], i * rows, (i + 1) * rows));
            CHECK(Y1 == Y2 && C1 == C2);
        }
    }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
