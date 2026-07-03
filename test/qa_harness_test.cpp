// Compiles PulpEmbedQaHarness.h (the JUCE glue over compareRgba) and exercises
// the PNG-file reference compare end to end: write real PNGs with JUCE, load them
// back, and diff. The pure pixel math is covered separately by
// qa_image_compare_test.cpp; this proves the juce::Image <-> RGBA plumbing and
// the file/decode failure handling. Exit 0 = all pass.

#include "../include/PulpEmbedQaHarness.h"

#include <juce_graphics/juce_graphics.h>

#include <cstdio>

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::fprintf(stderr, "  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

static juce::File writeSolidPng(const juce::String& name, int w, int h,
                                juce::Colour colour) {
    juce::Image img(juce::Image::ARGB, w, h, true);
    { juce::Graphics g(img); g.fillAll(colour); }
    const juce::File f = juce::File::getSpecialLocation(juce::File::tempDirectory)
                             .getChildFile(name);
    f.deleteFile();
    juce::FileOutputStream os(f);
    juce::PNGImageFormat().writeImageToStream(img, os);
    return f;
}

int main() {
    const juce::File red = writeSolidPng("pulp_qa_red.png", 16, 16, juce::Colours::red);
    const juce::File red2 = writeSolidPng("pulp_qa_red2.png", 16, 16, juce::Colours::red);
    const juce::File blue = writeSolidPng("pulp_qa_blue.png", 16, 16, juce::Colours::blue);
    const juce::File tall = writeSolidPng("pulp_qa_tall.png", 16, 20, juce::Colours::red);

    check(pulp_juce::compareRenderToReferencePng(red, red2).passed,
          "identical PNGs match");
    check(!pulp_juce::compareRenderToReferencePng(red, blue).passed,
          "different colour fails");
    check(!pulp_juce::compareRenderToReferencePng(red, tall).passed,
          "size mismatch fails");
    check(!pulp_juce::compareRenderToReferencePng(
              red, juce::File("/no/such/reference.png")).passed,
          "missing reference fails, not a vacuous pass");

    // imageToRgba round-trips a known pixel.
    const juce::Image img = juce::ImageFileFormat::loadFrom(red);
    const std::vector<std::uint8_t> rgba = pulp_juce::imageToRgba(img);
    check(rgba.size() == 16u * 16u * 4u, "imageToRgba packs W*H*4 bytes");
    check(rgba[0] == 255 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 255,
          "imageToRgba reads RGBA in order (opaque red)");

    std::fprintf(stderr, g_fail ? "FAILED (%d)\n" : "PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
