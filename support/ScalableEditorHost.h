// ScalableEditorHost.h — a chrome-free container for a plugin's
// AudioProcessorEditor with a fit-to-window scaling seam.
//
// SUPPORT FILE (not built by default). Copy it into your JUCE project and wire
// it from a custom standalone (see PulpStandaloneApp.cpp) or any window that
// should present a Pulp-embed editor full-bleed. It codifies the pattern a
// shipping consumer converged on: no JUCE chrome around the editor, the editor
// resized to fill its host, and a single scale seam for "fit the design to the
// window".
//
// How scaling actually works with the embed: PulpEmbedComponent hosts a
// HEAVYWEIGHT native view (juce::NSViewComponent on macOS). The embed already
// letterboxes its content to the design viewport INTERNALLY
// (WindowHost::compute_design_viewport_transform via the shim's
// set_design_viewport), so for the embed the correct "scaling" is simply to
// resize this host to the window and let the embed letterbox — do NOT apply a
// juce AffineTransform to a heavyweight child (AppKit does not transform a
// hosted NSView's drawing). The optional uniform-scale seam below is provided
// for LIGHTWEIGHT editors (or a non-embed editor) and is a no-op (scale == 1)
// by default; keep it off for the embed and rely on host-window resize +
// PulpEmbedComponent::configureResizableEditor for aspect locking.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace pulp_juce {

class ScalableEditorHost : public juce::Component {
public:
    // Takes ownership of `editor`. `designW`/`designH` are the editor's natural
    // (design) size, used only for the optional uniform-scale seam.
    ScalableEditorHost(std::unique_ptr<juce::Component> editor, int designW, int designH)
        : editor_(std::move(editor)), designW_(designW), designH_(designH) {
        setOpaque(true);
        addAndMakeVisible(*editor_);
        setSize(designW_, designH_);
    }

    // Optional fit-to-window scaling for a LIGHTWEIGHT editor. For a
    // PulpEmbedComponent-backed editor leave this false (the embed letterboxes
    // internally; a transform on a heavyweight NSView has no effect).
    void setUniformScalingEnabled(bool enabled) {
        uniformScaling_ = enabled;
        resized();
    }

    void paint(juce::Graphics& g) override { g.fillAll(juce::Colours::black); }

    void resized() override {
        if (editor_ == nullptr) return;

        if (uniformScaling_ && designW_ > 0 && designH_ > 0) {
            // Uniform scale that fits the design box into the window (letterbox).
            const auto sx = (double) getWidth() / (double) designW_;
            const auto sy = (double) getHeight() / (double) designH_;
            const auto s = juce::jmin(sx, sy);
            editor_->setTransform(juce::AffineTransform::scale((float) s));
            // Centre the scaled design box in the window.
            const int w = juce::roundToInt(designW_ * s);
            const int h = juce::roundToInt(designH_ * s);
            editor_->setBounds(0, 0, designW_, designH_);  // pre-transform bounds
            editor_->setTopLeftPosition((getWidth() - w) / 2, (getHeight() - h) / 2);
        } else {
            // Embed path: no transform, just fill. The embed handles letterbox.
            editor_->setTransform({});
            editor_->setBounds(getLocalBounds());
        }
    }

    juce::Component* editor() noexcept { return editor_.get(); }

private:
    std::unique_ptr<juce::Component> editor_;
    int  designW_ = 0, designH_ = 0;
    bool uniformScaling_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ScalableEditorHost)
};

}  // namespace pulp_juce
