// Regression — the ctor must NOT force the design size; the owning editor
// drives size, and the first NON-ZERO resized() is the first pulp_embed_resize.
//
// The bug: the ctor called setSize(design) and attachAndStart sized the wrapper
// immediately, so reopening while the host had zoomed the editor rendered once
// at the design size and again at the zoomed size (a visible double-render).
//
// Headless assertions of the fix (no window / GPU needed):
//  1. Right after construction the component is 0x0 — the design size was
//     retained, not forced onto the component.
//  2. Sizing it to a NON-base size (the "owning editor drives size" contract)
//     makes the component adopt that size, and a deterministic render at that
//     size succeeds — the first paint uses the mounted size, not the design base.

#include <juce_gui_basics/juce_gui_basics.h>

#include "PulpEmbedComponent.h"

#include <pulp/view/design_frame_view.hpp>

#include <cstdio>
#include <memory>

namespace {
int g_fail = 0;
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

std::unique_ptr<pulp::view::View> makeEditor() {
    using pulp::view::DesignFrameElement;
    const std::string svg =
        R"(<svg width="400" height="300" xmlns="http://www.w3.org/2000/svg">)"
        R"(<rect x="0" y="0" width="400" height="300" fill="#222"/></svg>)";
    return std::make_unique<pulp::view::DesignFrameView>(svg,
                                                         std::vector<DesignFrameElement>{});
}
}  // namespace

int main() {
    juce::ScopedJuceInitialiser_GUI juceInit;

    constexpr int kDesignW = 400, kDesignH = 300;
    constexpr int kMountW = 700, kMountH = 525;  // non-base "already zoomed" size

    pulp_juce::PulpEmbedComponent comp(
        pulp::embed::NativeViewFactory{&makeEditor}, kDesignW, kDesignH);
    check(comp.isValid(), "component created from a native View factory");
    if (!comp.isValid()) {
        std::printf("create failed: %s\n", comp.lastError().toRawUTF8());
        std::printf("pulp-embed-juce size-on-open-test FAILED\n");
        return 1;
    }

    // (1) The ctor did NOT force the design size onto the component.
    check(comp.getWidth() == 0 && comp.getHeight() == 0,
          "ctor does not force the design size (component starts 0x0)");

    // (2) The owning editor drives size: mount at a non-base size.
    comp.setBounds(0, 0, kMountW, kMountH);
    check(comp.getWidth() == kMountW && comp.getHeight() == kMountH,
          "component adopts the size the owning editor set (not the design base)");

    // The first paint uses the mounted size: a deterministic render at it works.
    const juce::File out =
        juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("pulp-embed-juce-size-on-open.png");
    check(comp.writeRenderPng(out, kMountW, kMountH),
          "deterministic render at the mounted (non-base) size succeeds");
    out.deleteFile();

    std::printf("%s\n", g_fail == 0 ? "pulp-embed-juce size-on-open-test OK"
                                    : "pulp-embed-juce size-on-open-test FAILED");
    return g_fail == 0 ? 0 : 1;
}
