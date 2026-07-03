// Unit test for compareRgba() — the reference-compare core of the QA harness.
// Pure RGBA tolerance math over plain arrays: no JUCE, no GPU, no SDK. Exit 0 =
// all pass.

#include "../include/PulpEmbedImageCompare.h"

#include <cstdio>
#include <vector>

using pulp_juce::compareRgba;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

static std::vector<std::uint8_t> solid(int w, int h, std::uint8_t r,
                                       std::uint8_t g, std::uint8_t b,
                                       std::uint8_t a = 255) {
    std::vector<std::uint8_t> px(
        static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);
    for (std::size_t i = 0; i < px.size(); i += 4) {
        px[i] = r; px[i + 1] = g; px[i + 2] = b; px[i + 3] = a;
    }
    return px;
}

int main() {
    // Identical buffers -> pixel-perfect pass, zero delta.
    {
        auto a = solid(4, 4, 10, 20, 30);
        auto res = compareRgba(a.data(), a.data(), 4, 4);
        check(res.passed, "identical -> passed");
        check(res.maxChannelDelta == 0, "identical -> zero delta");
        check(res.differingPixels == 0 && res.totalPixels == 16,
              "identical -> no differing pixels, 16 total");
    }

    // One channel changed beyond exact tolerance -> a single differing pixel,
    // pixel-perfect compare fails.
    {
        auto a = solid(4, 4, 10, 20, 30);
        auto b = a; b[4 * 5 + 1] = 60;  // pixel 5, green 20 -> 60 (delta 40)
        auto res = compareRgba(a.data(), b.data(), 4, 4);
        check(!res.passed, "one changed pixel -> fails pixel-perfect");
        check(res.maxChannelDelta == 40, "reports max channel delta 40");
        check(res.differingPixels == 1, "exactly one differing pixel");
    }

    // A change within channelTolerance is not counted as differing.
    {
        auto a = solid(4, 4, 100, 100, 100);
        auto b = a; b[0] = 103;  // delta 3
        auto tight = compareRgba(a.data(), b.data(), 4, 4, /*tol*/ 2);
        check(!tight.passed && tight.differingPixels == 1, "delta 3 > tol 2 differs");
        auto loose = compareRgba(a.data(), b.data(), 4, 4, /*tol*/ 4);
        check(loose.passed && loose.differingPixels == 0, "delta 3 <= tol 4 ignored");
    }

    // maxDiffFraction absorbs a bounded number of stray pixels.
    {
        auto a = solid(10, 10, 0, 0, 0);      // 100 pixels
        auto b = a;
        for (std::size_t p = 0; p < 4u; ++p) b[p * 4] = 255;  // 4 differing px = 4%
        auto strict = compareRgba(b.data(), a.data(), 10, 10, 0, /*maxFrac*/ 0.0);
        check(!strict.passed, "4% diff fails at 0 tolerance fraction");
        auto lenient = compareRgba(b.data(), a.data(), 10, 10, 0, /*maxFrac*/ 0.05);
        check(lenient.passed, "4% diff passes under 5% fraction");
        check(lenient.diffPixelFraction > 0.039 && lenient.diffPixelFraction < 0.041,
              "diff fraction reported ~0.04");
    }

    // Size mismatch is a hard fail, never a vacuous pass.
    {
        auto a = solid(4, 4, 1, 1, 1);
        auto b = solid(4, 5, 1, 1, 1);
        auto res = compareRgba(a.data(), 4, 4, b.data(), 4, 5);
        check(!res.passed && !res.sizeMatched, "size mismatch -> hard fail");
    }

    // A null/empty buffer must not vacuously pass an empty capture.
    {
        auto res = compareRgba(nullptr, nullptr, 4, 4);
        check(!res.passed, "null buffers -> not passed");
        auto a = solid(1, 1, 0, 0, 0);
        auto zero = compareRgba(a.data(), a.data(), 0, 0);
        check(!zero.passed, "zero-area -> not passed");
    }

    std::printf(g_fail ? "\nFAILED (%d)\n" : "\nPASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
