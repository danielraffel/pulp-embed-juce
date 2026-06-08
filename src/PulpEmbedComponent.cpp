#include "PulpEmbedComponent.h"

#include <juce_audio_processors/juce_audio_processors.h>  // bind to a real processor

#include <cstdlib>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulp_juce {

// Maps the embedded design's control keys to a juce::AudioProcessor's
// parameters and trampolines the flat-C host callbacks into JUCE calls.
// host_ctx (PulpEmbedDesc.host_ctx) is a pointer to one of these. Lives as long
// as the component; the view is torn down before it (see ~PulpEmbedComponent).
struct PulpEmbedComponent::HostBridge {
    explicit HostBridge(juce::AudioProcessor& p) : proc(p) {}

    juce::AudioProcessor& proc;
    std::unordered_map<std::string, juce::AudioProcessorParameter*> byKey;
    std::vector<std::pair<std::string, juce::AudioProcessorParameter*>> bound;
    std::vector<float> lastPushed;  // last value pushed UI<-host, per bound entry

    juce::AudioProcessorParameter* find(const char* key) {
        const auto it = byKey.find(key);
        return it == byKey.end() ? nullptr : it->second;
    }

    // ── flat-C host callbacks (UI -> host). ctx == this. ────────────────────
    static void setParam(void* ctx, const char* key, double normalized) {
        if (auto* p = static_cast<HostBridge*>(ctx)->find(key))
            p->setValueNotifyingHost((float) juce::jlimit(0.0, 1.0, normalized));
    }
    static double getParam(void* ctx, const char* key) {
        if (auto* p = static_cast<HostBridge*>(ctx)->find(key)) return p->getValue();
        return 0.0;
    }
    static void beginGesture(void* ctx, const char* key) {
        if (auto* p = static_cast<HostBridge*>(ctx)->find(key)) p->beginChangeGesture();
    }
    static void endGesture(void* ctx, const char* key) {
        if (auto* p = static_cast<HostBridge*>(ctx)->find(key)) p->endChangeGesture();
    }
};

PulpEmbedComponent::PulpEmbedComponent(const juce::File& source,
                                       int logicalWidth, int logicalHeight) {
    createView(source, logicalWidth, logicalHeight);
}

PulpEmbedComponent::PulpEmbedComponent(const juce::File& source,
                                       int logicalWidth, int logicalHeight,
                                       juce::AudioProcessor& processor)
    : bridge_(std::make_unique<HostBridge>(processor)) {
    createView(source, logicalWidth, logicalHeight);
}

void PulpEmbedComponent::createView(const juce::File& source,
                                    int logicalWidth, int logicalHeight) {
    setSize(logicalWidth, logicalHeight);

    PulpEmbedDesc desc{};
    desc.struct_size = sizeof(PulpEmbedDesc);
    desc.abi_version = PULP_VIEW_EMBED_ABI_VERSION;
    desc.logical_width = logicalWidth;
    desc.logical_height = logicalHeight;
    desc.scale_factor = 1.0f;
    desc.backend_pref = PULP_EMBED_BACKEND_PREF_AUTO;
    desc.design_width = logicalWidth;
    desc.design_height = logicalHeight;

    // Wire the host parameter bridge when constructed with a processor. The
    // callbacks must be in the desc at creation time (host_ctx is captured
    // then), so this happens before pulp_embed_create_*.
    if (bridge_ != nullptr) {
        desc.host_ctx = bridge_.get();
        desc.host.set_param = &HostBridge::setParam;
        desc.host.get_param = &HostBridge::getParam;
        desc.host.begin_gesture = &HostBridge::beginGesture;
        desc.host.end_gesture = &HostBridge::endGesture;
    }

    const auto path = source.getFullPathName();
    // A directory (importer `--emit js` bundle with ui.js) renders through the
    // high-fidelity scripted-UI path; a .json file uses the lightweight native
    // DesignIR path.
    const bool isBundle =
        source.isDirectory() || source.getChildFile("ui.js").existsAsFile();
    PulpEmbedResult r =
        isBundle ? pulp_embed_create_from_ui_bundle(&desc, path.toRawUTF8(), &view_)
                 : pulp_embed_create_from_design_json(&desc, path.toRawUTF8(), &view_);
    if (r != PULP_EMBED_OK || view_ == nullptr) {
        view_ = nullptr;
        return;
    }

   #if JUCE_MAC
    // Host-parents mode: JUCE owns parenting/retain/resize of Pulp's child view.
    addAndMakeVisible(nsView_);
    nsView_.setView(pulp_embed_native_handle(view_));
    // Size the wrapper to the component immediately. resized() does NOT fire if
    // the component is already at its final size when content is installed, so
    // without this the NSViewComponent (and thus Pulp's child NSView) stays 0x0
    // and never appears on screen.
    nsView_.setBounds(getLocalBounds());
   #endif

    resolveParameterBindings();

    startTimerHz(30);  // drives notify_attached retry + pulp_embed_tick + host->UI

    // Dev hot-reload: for a bundle, remember its ui.js and auto-enable the
    // watcher when PULP_EMBED_HOT_RELOAD is set in the environment.
    if (isBundle) {
        watchFile_ = source.isDirectory() ? source.getChildFile("ui.js") : source;
        if (std::getenv("PULP_EMBED_HOT_RELOAD") != nullptr)
            enableBundleHotReload(true);
    }
}

void PulpEmbedComponent::resolveParameterBindings() {
    if (bridge_ == nullptr || view_ == nullptr) return;

    // Index the processor's parameters by their string ID (e.g. APVTS
    // ParameterID). Parameters without a string ID can't be addressed by the
    // design's key contract and are skipped.
    std::unordered_map<std::string, juce::AudioProcessorParameter*> byId;
    for (auto* p : bridge_->proc.getParameters())
        if (auto* wid = dynamic_cast<juce::AudioProcessorParameterWithID*>(p))
            byId[wid->paramID.toStdString()] = p;

    // Map each design control key to a matching parameter ID. Unmatched keys
    // stay visual-only (no binding) — never guessed.
    const int32_t n = pulp_embed_param_count(view_);
    for (int32_t i = 0; i < n; ++i) {
        char key[256] = {0};
        pulp_embed_param_key(view_, i, key, sizeof key);
        const auto it = byId.find(key);
        if (it == byId.end()) continue;
        bridge_->byKey[key] = it->second;
        bridge_->bound.emplace_back(key, it->second);
    }
    bridge_->lastPushed.assign(bridge_->bound.size(), -1.0f);

    // Push initial values UI<-host so controls reflect the current state on open.
    for (size_t i = 0; i < bridge_->bound.size(); ++i) {
        const float v = bridge_->bound[i].second->getValue();
        pulp_embed_param_changed(view_, bridge_->bound[i].first.c_str(), v);
        bridge_->lastPushed[i] = v;
    }
}

void PulpEmbedComponent::pumpHostToUi() {
    if (bridge_ == nullptr || view_ == nullptr) return;
    // Poll bound parameters on the UI thread (pulp_embed_param_changed is
    // UI-thread-only) and push host-side changes — automation, preset recall,
    // a sibling editor — into the matching control. Cheap float compares; fine
    // for the hundreds-of-params case at 30 Hz.
    for (size_t i = 0; i < bridge_->bound.size(); ++i) {
        const float v = bridge_->bound[i].second->getValue();
        if (v != bridge_->lastPushed[i]) {
            pulp_embed_param_changed(view_, bridge_->bound[i].first.c_str(), v);
            bridge_->lastPushed[i] = v;
        }
    }
}

int PulpEmbedComponent::boundParameterCount() const noexcept {
    return bridge_ != nullptr ? static_cast<int>(bridge_->bound.size()) : 0;
}

void PulpEmbedComponent::enableBundleHotReload(bool enable) {
    watch_ = enable && watchFile_.existsAsFile();
    if (watch_) {
        lastWrite_ = watchFile_.getLastModificationTime().toMilliseconds();
        pendingWrite_ = lastWrite_;
    }
}

PulpEmbedComponent::~PulpEmbedComponent() {
    stopTimer();
   #if JUCE_MAC
    // Null JUCE's retained reference to Pulp's child BEFORE destroying Pulp,
    // so JUCE removes/releases its wrapper while Pulp's host still owns the
    // NSView (avoids a dangling/double-free). Then Pulp tears down in order.
    nsView_.setView(nullptr);
   #endif
    if (view_ != nullptr) {
        // Destroy the view (and thus any in-flight host callbacks) before
        // bridge_ is freed — bridge_ is a member destroyed after this body, so
        // host_ctx stays valid for the lifetime of the view.
        pulp_embed_destroy(view_);
        view_ = nullptr;
    }
}

juce::String PulpEmbedComponent::lastError() const {
    if (view_ == nullptr) {
        char buf[512];
        pulp_embed_last_create_error(buf, sizeof(buf));
        return juce::String::fromUTF8(buf);
    }
    char buf[512];
    pulp_embed_last_error(view_, buf, sizeof(buf));
    return juce::String::fromUTF8(buf);
}

bool PulpEmbedComponent::isGpuBacked() const noexcept {
    return view_ != nullptr &&
           pulp_embed_active_backend(view_) == PULP_EMBED_BACKEND_GPU;
}

static bool write_png(const juce::File& out, const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) return false;
    out.deleteFile();
    return out.replaceWithData(bytes.data(), bytes.size());
}

bool PulpEmbedComponent::writeCapturePng(const juce::File& out) {
    if (view_ == nullptr) return false;
    size_t need = 0;
    if (pulp_embed_capture_png(view_, nullptr, 0, &need) != PULP_EMBED_OK || need == 0)
        return false;
    std::vector<uint8_t> png(need);
    if (pulp_embed_capture_png(view_, png.data(), png.size(), &need) != PULP_EMBED_OK)
        return false;
    return write_png(out, png);
}

bool PulpEmbedComponent::writeRenderPng(const juce::File& out, int width, int height) {
    if (view_ == nullptr) return false;
    size_t need = 0;
    if (pulp_embed_render_png(view_, width, height, 1.0f, nullptr, 0, &need) != PULP_EMBED_OK
        || need == 0)
        return false;
    std::vector<uint8_t> png(need);
    if (pulp_embed_render_png(view_, width, height, 1.0f, png.data(), png.size(), &need)
        != PULP_EMBED_OK)
        return false;
    return write_png(out, png);
}

void PulpEmbedComponent::resized() {
    if (view_ == nullptr) return;
   #if JUCE_MAC
    nsView_.setBounds(getLocalBounds());
   #endif
    const float scale =
        (float) juce::Desktop::getInstance().getDisplays()
            .getPrimaryDisplay() ->scale;
    pulp_embed_resize(view_, getWidth(), getHeight(), scale > 0.0f ? scale : 1.0f);
}

void PulpEmbedComponent::timerCallback() {
    if (view_ == nullptr) return;
    if (!opened_) {
        // Retry until the child is actually in a live window hierarchy.
        if (pulp_embed_notify_attached(view_) == PULP_EMBED_OK)
            opened_ = true;
    }
    pulp_embed_tick(view_);
    pumpHostToUi();

    // Dev hot-reload: poll the bundle's ui.js mtime; apply a change only after it
    // has been stable for one tick (debounce vs a mid-write save). reload_bundle
    // is probe-first/last-good, so a bad edit leaves the running editor intact.
    if (watch_) {
        const auto m = watchFile_.getLastModificationTime().toMilliseconds();
        if (m != lastWrite_) {
            if (m == pendingWrite_ &&
                pulp_embed_reload_bundle(view_, nullptr) == PULP_EMBED_OK)
                lastWrite_ = m;        // applied; else retry once it's stable
            pendingWrite_ = m;
        }
    }
}

}  // namespace pulp_juce
