/*
 * PulpEmbedImageCompare.h — the reference-compare half of the QA harness.
 *
 * A capture proves the UI *rendered*; a compare proves it rendered *the right
 * thing*. The self-checks only ever asserted a capture was written — the
 * "matches a known-good reference" step was left to whoever ran them by hand.
 * This is that step, as code.
 *
 * The core is a dependency-free tolerance compare over two tightly-packed RGBA8
 * buffers, so the pixel math is unit-testable with plain arrays — no JUCE, no
 * GPU, no SDK (see test/qa_image_compare_test.cpp). PulpEmbedQaHarness.h adds the
 * juce::Image / PNG-file glue on top for the live harness.
 *
 * Compare the DETERMINISTIC raster (PulpEmbedComponent::writeRenderPng) against a
 * committed reference, not the live GPU back buffer (writeCapturePng): the raster
 * is CPU-stable across machines, so a reference diff is meaningful on a hosted CI
 * runner that has no GPU. Reserve live-capture compares for a GPU lane.
 */
#ifndef PULP_EMBED_IMAGE_COMPARE_H
#define PULP_EMBED_IMAGE_COMPARE_H

#include <cstddef>
#include <cstdint>

namespace pulp_juce {

struct ImageCompareResult {
    bool sizeMatched = false;       // the two buffers describe the same WxH
    int maxChannelDelta = 0;        // largest per-channel |a-b| seen (0..255)
    std::size_t differingPixels = 0;
    std::size_t totalPixels = 0;
    double diffPixelFraction = 1.0; // differingPixels / totalPixels (1.0 if none)
    bool passed = false;            // sizeMatched && diffPixelFraction <= max

    // Convenience for `if (compare(...))`.
    explicit operator bool() const { return passed; }
};

// Compare two tightly-packed RGBA8 buffers of the same dimensions. A pixel is
// "differing" when any of its four channels differs by more than
// `channelTolerance` (0 = exact). The compare passes when the sizes match AND the
// fraction of differing pixels is at most `maxDiffFraction` (0.0 = pixel-perfect;
// a small fraction absorbs incidental antialiasing/subpixel noise).
//
// `a`/`b` must each point to at least `width*height*4` bytes. A zero-area image
// or a null buffer is treated as a non-match (passed == false) rather than a
// vacuous pass, so a harness can't silently "pass" on an empty capture.
inline ImageCompareResult compareRgba(const std::uint8_t* a, const std::uint8_t* b,
                                      int width, int height,
                                      int channelTolerance = 0,
                                      double maxDiffFraction = 0.0) {
    ImageCompareResult r;
    if (a == nullptr || b == nullptr || width <= 0 || height <= 0)
        return r;  // passed == false; nothing meaningful to compare

    r.sizeMatched = true;
    r.totalPixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    const std::size_t bytes = r.totalPixels * 4u;
    for (std::size_t px = 0, i = 0; i < bytes; i += 4u, ++px) {
        int pixelDelta = 0;
        for (std::size_t c = 0; c < 4u; ++c) {
            const int d = static_cast<int>(a[i + c]) - static_cast<int>(b[i + c]);
            const int ad = d < 0 ? -d : d;
            if (ad > pixelDelta) pixelDelta = ad;
        }
        if (pixelDelta > r.maxChannelDelta) r.maxChannelDelta = pixelDelta;
        if (pixelDelta > channelTolerance) ++r.differingPixels;
    }
    r.diffPixelFraction = r.totalPixels
        ? static_cast<double>(r.differingPixels) / static_cast<double>(r.totalPixels)
        : 1.0;
    r.passed = r.sizeMatched && r.diffPixelFraction <= maxDiffFraction;
    return r;
}

// Overload for buffers of possibly-different dimensions: a size mismatch is a
// hard fail (no resampling — a QA reference must match the captured geometry).
inline ImageCompareResult compareRgba(const std::uint8_t* a, int aw, int ah,
                                      const std::uint8_t* b, int bw, int bh,
                                      int channelTolerance = 0,
                                      double maxDiffFraction = 0.0) {
    if (aw != bw || ah != bh || aw <= 0 || ah <= 0) {
        ImageCompareResult r;  // sizeMatched=false, passed=false
        r.totalPixels = static_cast<std::size_t>(aw > 0 ? aw : 0)
                      * static_cast<std::size_t>(ah > 0 ? ah : 0);
        return r;
    }
    return compareRgba(a, b, aw, ah, channelTolerance, maxDiffFraction);
}

}  // namespace pulp_juce

#endif  // PULP_EMBED_IMAGE_COMPARE_H
