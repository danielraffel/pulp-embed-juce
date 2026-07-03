/*
 * PulpEmbedQaHarness.h — a frame-gated QA scenario runner over a
 * PulpEmbedComponent.
 *
 * The standalone, plugin, and synthetic-rack self-checks each hand-rolled the
 * same dance: wait until the embed opens, let a few live frames render, drive a
 * command, capture a PNG, and tally pass/fail into a process return code. This
 * factors that into one reusable runner and adds the step the copies never
 * had — comparing the capture against a committed reference image
 * (PulpEmbedImageCompare.h).
 *
 * Timing note (the writeCapturePng gotcha, see README): a capture reflects the
 * last COMPLETED frame, so the runner drives state and then waits `settleFrames`
 * message-loop ticks before it captures — it never captures on the same tick it
 * changed something.
 *
 * The pixel math lives in PulpEmbedImageCompare.h and is unit-tested standalone
 * (test/qa_image_compare_test.cpp); this header is the JUCE/Component glue on
 * top, exercised by the Standalone self-checks in CI.
 */
#ifndef PULP_EMBED_QA_HARNESS_H
#define PULP_EMBED_QA_HARNESS_H

#include "PulpEmbedComponent.h"
#include "PulpEmbedImageCompare.h"

#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdio>
#include <functional>
#include <utility>
#include <vector>

namespace pulp_juce {

// Pull a juce::Image into a tightly-packed RGBA8 buffer so compareRgba() can read
// it independent of the image's internal pixel format.
inline std::vector<std::uint8_t> imageToRgba(const juce::Image& img) {
    std::vector<std::uint8_t> out;
    if (!img.isValid()) return out;
    const int w = img.getWidth(), h = img.getHeight();
    out.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);
    std::size_t i = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const juce::Colour c = img.getPixelAt(x, y);
            out[i++] = c.getRed();
            out[i++] = c.getGreen();
            out[i++] = c.getBlue();
            out[i++] = c.getAlpha();
        }
    }
    return out;
}

// Compare a rendered PNG against a reference PNG on disk. A missing or
// undecodable file, or a size mismatch, is a hard fail (passed == false) — never
// a vacuous pass.
inline ImageCompareResult compareRenderToReferencePng(const juce::File& rendered,
                                                      const juce::File& reference,
                                                      int channelTolerance = 0,
                                                      double maxDiffFraction = 0.0) {
    const juce::Image a = juce::ImageFileFormat::loadFrom(rendered);
    const juce::Image b = juce::ImageFileFormat::loadFrom(reference);
    if (!a.isValid() || !b.isValid()) return {};
    const std::vector<std::uint8_t> ra = imageToRgba(a);
    const std::vector<std::uint8_t> rb = imageToRgba(b);
    return compareRgba(ra.data(), a.getWidth(), a.getHeight(),
                       rb.data(), b.getWidth(), b.getHeight(),
                       channelTolerance, maxDiffFraction);
}

// Drives a scripted QA scenario against a live PulpEmbedComponent on the message
// loop, then reports pass/fail. Mirrors the self-checks' timing exactly:
// wait for isOpened() (bounded by settleWaits), let settleFrames render, run the
// caller's steps, capture, optionally compare a reference, and call onDone once.
//
// Lifetime: run() is asynchronous — it arms a juce::Timer and returns; onDone
// fires several message-loop ticks later. The harness IS the Timer, so it must
// stay alive until onDone runs. Hold it as a member of the object that owns the
// editor (as the synthetic-rack self-check does), NOT as a local that goes out of
// scope when run() returns — a destroyed harness is a Timer callback into freed
// memory. `component` must likewise outlive the run (non-owning pointer).
class PulpEmbedQaHarness : private juce::Timer {
public:
    struct Options {
        int settleWaits = 40;        // max ticks to wait for isOpened()
        int settleFrames = 6;        // live frames to render before capturing
        int intervalMs = 250;        // message-loop tick period
        int width = 0;               // deterministic-render size (0 = component's)
        int height = 0;
        int channelTolerance = 0;    // per-channel abs delta a pixel may drift
        double maxDiffFraction = 0.0;  // fraction of pixels allowed to differ
    };

    // One named QA assertion. `run` applies state and/or checks a condition and
    // returns pass/fail. Runs on the message thread after the embed has settled.
    struct Step {
        juce::String name;
        std::function<bool()> run;
    };

    struct Result {
        int failures = 0;
        bool opened = false;
        bool deterministicWritten = false;
        bool referenceMatched = true;  // true when no reference was supplied
        bool ok() const { return failures == 0; }
    };

    // Non-owning: `component` must outlive the run. `renderPath` receives the
    // deterministic raster. If `referencePath` exists, the raster is compared
    // against it and a mismatch is a failure. `onDone` fires exactly once.
    // Overload with default Options — a `= {}` default argument can't be used
    // for a nested type whose in-class initializers reference the still-
    // incomplete enclosing class, so delegate through a function body instead.
    void run(PulpEmbedComponent& component,
             std::vector<Step> steps,
             juce::File renderPath,
             juce::File referencePath,
             std::function<void(const Result&)> onDone) {
        run(component, std::move(steps), std::move(renderPath),
            std::move(referencePath), std::move(onDone), Options{});
    }

    void run(PulpEmbedComponent& component,
             std::vector<Step> steps,
             juce::File renderPath,
             juce::File referencePath,
             std::function<void(const Result&)> onDone,
             Options options) {
        component_ = &component;
        steps_ = std::move(steps);
        renderPath_ = std::move(renderPath);
        referencePath_ = std::move(referencePath);
        onDone_ = std::move(onDone);
        opts_ = options;
        waits_ = frames_ = postFrames_ = 0;
        stepsRun_ = false;
        result_ = {};
        startTimer(opts_.intervalMs);
    }

private:
    void finish() {
        stopTimer();
        component_ = nullptr;
        if (onDone_) onDone_(result_);
    }

    void pass(bool ok, const char* what) {
        std::fprintf(stderr, "  [%s] %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok) ++result_.failures;
    }

    void timerCallback() override {
        if (component_ == nullptr) { stopTimer(); return; }
        if (!component_->isOpened()) {
            if (++waits_ < opts_.settleWaits) return;  // keep waiting to open
        }
        result_.opened = component_->isOpened();
        if (++frames_ < opts_.settleFrames) return;    // let live frames render

        // Phase 1: run the caller's steps ONCE, then yield. A step that changes
        // host/APVTS state needs the component's own tick + pumpHostToUi to
        // propagate AND a frame to render before we capture — capturing in this
        // same callback would grab the pre-step frame (the writeCapturePng
        // gotcha). So we return and let settleFrames more frames render first.
        if (!stepsRun_) {
            if (!result_.opened) pass(false, "embed opened");
            for (auto& s : steps_)
                pass(s.run ? s.run() : false, s.name.toRawUTF8());
            stepsRun_ = true;
            postFrames_ = 0;
            return;
        }
        if (++postFrames_ < opts_.settleFrames) return;  // let step effects render

        // Phase 2: the step effects have rendered — capture and compare.
        const int w = opts_.width > 0 ? opts_.width : component_->getWidth();
        const int h = opts_.height > 0 ? opts_.height : component_->getHeight();
        result_.deterministicWritten = component_->writeRenderPng(renderPath_, w, h);
        pass(result_.deterministicWritten, "deterministic raster written");

        if (referencePath_.existsAsFile()) {
            const ImageCompareResult cmp = compareRenderToReferencePng(
                renderPath_, referencePath_, opts_.channelTolerance,
                opts_.maxDiffFraction);
            result_.referenceMatched = cmp.passed;
            if (!cmp.passed)
                std::fprintf(stderr,
                    "  [FAIL] reference mismatch (maxDelta=%d diffFraction=%.4f)\n",
                    cmp.maxChannelDelta, cmp.diffPixelFraction);
            else
                std::fprintf(stderr, "  [PASS] matches reference\n");
            pass(cmp.passed, "capture matches reference");
        }

        finish();
    }

    PulpEmbedComponent* component_ = nullptr;
    std::vector<Step> steps_;
    juce::File renderPath_, referencePath_;
    std::function<void(const Result&)> onDone_;
    Options opts_;
    int waits_ = 0, frames_ = 0, postFrames_ = 0;
    bool stepsRun_ = false;
    Result result_;
};

}  // namespace pulp_juce

#endif  // PULP_EMBED_QA_HARNESS_H
