// PulpStandaloneApp.cpp — a custom JUCE standalone app that presents a plugin's
// AudioProcessorEditor chrome-free, with a native title bar and native
// full-screen, instead of the stock juce::StandaloneFilterApp chrome.
//
// SUPPORT FILE (not built by default). Copy it into your JUCE project and wire
// it in CMake:
//
//   target_compile_definitions(<your Standalone target> PRIVATE
//       JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1)
//   target_sources(<your Standalone target> PRIVATE
//       support/PulpStandaloneApp.cpp)
//
// With JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1 the JUCE audio-plugin-client
// Standalone wrapper does NOT emit its own main()/application; this file
// provides one. It reuses juce::StandalonePluginHolder for the plugin instance
// + audio-device management (so device selection, state, and the plugin
// lifecycle still work), but hosts the editor in a bare window with no JUCE
// tool-bar / options-button chrome.
//
// Why this exists: stock juce::StandaloneFilterWindow forces a chrome strip and
// blocks native full-screen — wrong for a design-first Pulp editor whose whole
// point is a full-bleed GPU surface. This is the small amount of AppKit-free
// glue a consumer would otherwise re-derive.

#include <juce_audio_plugin_client/juce_audio_plugin_client.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "ScalableEditorHost.h"

#include <memory>

namespace pulp_juce {

// A frameless-content DocumentWindow: native title bar (so it participates in
// native full-screen and the traffic-light buttons), but no JUCE chrome around
// the editor. Owns a StandalonePluginHolder for the plugin + audio device.
class PulpStandaloneWindow : public juce::DocumentWindow {
public:
    PulpStandaloneWindow()
        : juce::DocumentWindow(JUCE_APPLICATION_NAME_STRING,
                               juce::Colours::black,
                               juce::DocumentWindow::allButtons) {
        setUsingNativeTitleBar(true);   // native full-screen + traffic lights

        // StandalonePluginHolder builds the AudioProcessor and wires the audio
        // device manager. It reads/writes device + plugin state under the app's
        // properties, so standalone runs behave like the stock wrapper.
        holder_ = std::make_unique<juce::StandalonePluginHolder>(
            /*settingsToUse*/ nullptr, /*takeOwnershipOfSettings*/ true);

        if (auto* proc = holder_->processor.get()) {
            if (auto* editor = proc->createEditorIfNeeded()) {
                const int w = editor->getWidth()  > 0 ? editor->getWidth()  : 1000;
                const int h = editor->getHeight() > 0 ? editor->getHeight() : 600;
                // Wrap the editor in the chrome-free scaling host. Keep uniform
                // scaling OFF for a Pulp-embed editor (it letterboxes itself).
                auto host = std::make_unique<ScalableEditorHost>(
                    std::unique_ptr<juce::Component>(editor), w, h);
                setContentOwned(host.release(), true);
                centreWithSize(w, h);
            }
        }

        setResizable(true, /*useBottomRightCornerResizer*/ false);
        setVisible(true);
    }

    ~PulpStandaloneWindow() override {
        clearContentComponent();
        holder_.reset();
    }

    void closeButtonPressed() override {
        juce::JUCEApplicationBase::quit();
    }

    // F11 / native full-screen toggle.
    bool keyPressed(const juce::KeyPress& key) override {
        if (key == juce::KeyPress::F11Key) {
            setFullScreen(!isFullScreen());
            return true;
        }
        return juce::DocumentWindow::keyPressed(key);
    }

private:
    std::unique_ptr<juce::StandalonePluginHolder> holder_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PulpStandaloneWindow)
};

class PulpStandaloneApp : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override          { return true; }

    void initialise(const juce::String&) override { window_ = std::make_unique<PulpStandaloneWindow>(); }
    void shutdown() override                       { window_ = nullptr; }

private:
    std::unique_ptr<PulpStandaloneWindow> window_;
};

}  // namespace pulp_juce

// JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP suppresses the stock wrapper's app, so
// we declare ours here.
START_JUCE_APPLICATION(pulp_juce::PulpStandaloneApp)
