#include "PulpEmbedComponent.h"

#include <pulp_view_embed.hpp>  // pulp::embed::param_descs / read_design_params (shared loop)
#include <juce_audio_processors/juce_audio_processors.h>  // bind to a real processor

#include <cstdlib>
#include <cstring>
#include <functional>
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

    // ── ABI v6 text-field string bridge (text_field <-> plugin STATE) ────────
    // text_fields carry a string (preset name / label / search), not a
    // normalized value, so they ride a side-channel keyed by the design key.
    // `strings` is the authoritative store get_string seeds from (empty = keep
    // the imported default); `onString` is an optional live-edit notification.
    std::unordered_map<std::string, std::string> strings;
    std::function<void(const juce::String&, const juce::String&)> onString;

    static void setString(void* ctx, const char* key, const char* utf8) {
        auto* b = static_cast<HostBridge*>(ctx);
        const std::string k = key ? key : "";
        const std::string v = utf8 ? utf8 : "";
        b->strings[k] = v;
        if (b->onString)
            b->onString(juce::String::fromUTF8(k.c_str()), juce::String::fromUTF8(v.c_str()));
    }
    static int32_t getString(void* ctx, const char* key, char* out, int32_t cap) {
        auto* b = static_cast<HostBridge*>(ctx);
        const auto it = b->strings.find(key ? key : "");
        if (it == b->strings.end()) return -1;  // no host opinion: keep imported default
        const int32_t n = static_cast<int32_t>(it->second.size());
        if (out && cap > 0) {
            const int32_t c = n < cap - 1 ? n : cap - 1;
            std::memcpy(out, it->second.data(), static_cast<size_t>(c));
            out[c] = '\0';
        }
        return n;
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
        // ABI v6 string side-channel (text_field <-> plugin state), same host_ctx.
        desc.host.set_string = &HostBridge::setString;
        desc.host.get_string = &HostBridge::getString;
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

    // Opt into mouse-move tracking even outside drag — without this, JUCE
    // only delivers mouseMove during a press-drag, and pulp-view-embed
    // never learns the cursor position for CSS :hover dispatch.
    setMouseClickGrabsKeyboardFocus(false);

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

// ── text-field string state (ABI v6) ────────────────────────────────────────

int PulpEmbedComponent::stringFieldCount() const noexcept {
    return view_ != nullptr ? pulp_embed_string_param_count(view_) : 0;
}

juce::String PulpEmbedComponent::stringFieldKey(int index) const {
    char buf[256] = {0};
    if (view_ != nullptr) pulp_embed_string_param_key(view_, index, buf, sizeof buf);
    return juce::String::fromUTF8(buf);
}

juce::String PulpEmbedComponent::stringValue(const juce::String& key) const {
    if (view_ == nullptr) return {};
    // Two-call sizing so arbitrarily long values round-trip.
    const size_t need = pulp_embed_get_string(view_, key.toRawUTF8(), nullptr, 0);
    if (need == 0) return {};
    std::vector<char> buf(need + 1, '\0');
    pulp_embed_get_string(view_, key.toRawUTF8(), buf.data(), buf.size());
    return juce::String::fromUTF8(buf.data());
}

bool PulpEmbedComponent::setStringValue(const juce::String& key, const juce::String& value) {
    return view_ != nullptr &&
           pulp_embed_set_string(view_, key.toRawUTF8(), value.toRawUTF8()) == PULP_EMBED_OK;
}

juce::StringPairArray PulpEmbedComponent::captureStringState() const {
    juce::StringPairArray out;
    const int n = stringFieldCount();
    for (int i = 0; i < n; ++i) {
        const juce::String k = stringFieldKey(i);
        out.set(k, stringValue(k));
    }
    return out;
}

void PulpEmbedComponent::restoreStringState(const juce::StringPairArray& state) {
    const auto& keys = state.getAllKeys();
    for (const auto& k : keys) setStringValue(k, state[k]);
}

void PulpEmbedComponent::setStringChangeHandler(
        std::function<void(const juce::String&, const juce::String&)> fn) {
    if (bridge_ != nullptr) bridge_->onString = std::move(fn);
}

// Convert the shared framework-neutral descriptor (std::string) to JUCE's
// juce::String variant. The enumeration loop itself lives once in
// pulp_view_embed.hpp (pulp::embed::param_descs / read_design_params) — this is
// the only JUCE-specific part.
static PulpEmbedComponent::DesignParamDesc toJuce(const pulp::embed::ParamDesc& p) {
    PulpEmbedComponent::DesignParamDesc d;
    d.key = juce::String::fromUTF8(p.key.c_str());
    d.widgetKind = juce::String::fromUTF8(p.widget_kind.c_str());
    d.isDiscrete = p.is_discrete;
    d.optionCount = p.option_count;
    d.defaultNorm = p.default_norm;
    d.name = juce::String::fromUTF8(p.name.c_str());
    d.unit = juce::String::fromUTF8(p.unit.c_str());
    return d;
}

std::vector<PulpEmbedComponent::DesignParamDesc> PulpEmbedComponent::designParams() const {
    std::vector<DesignParamDesc> out;
    for (const auto& p : pulp::embed::param_descs(view_)) out.push_back(toJuce(p));
    return out;
}

std::vector<PulpEmbedComponent::DesignParamDesc>
PulpEmbedComponent::readDesignParams(const juce::File& source, int logicalWidth,
                                     int logicalHeight) {
    const bool isBundle =
        source.isDirectory() || source.getChildFile("ui.js").existsAsFile();
    std::vector<DesignParamDesc> out;
    for (const auto& p : pulp::embed::read_design_params(
             source.getFullPathName().toStdString(), isBundle, logicalWidth, logicalHeight))
        out.push_back(toJuce(p));
    return out;
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

// ── Hover routing ────────────────────────────────────────────────────────
//
// pulp-view-embed has no platform mouse-move tracking — without forwarding
// these events, `View::set_hovered` is never called in the embedded plugin
// context and CSS :hover / onMouseEnter / onMouseLeave never fire (even
// though `registerHover(id)` correctly arms the lambdas).
//
// Coordinates: JUCE delivers MouseEvent positions in this component's local
// coords (top-left origin, logical pixels). That matches Pulp's root-view
// coord space when the wrapping NSViewComponent fills this component
// (which it does in createView), so we forward the raw x/y.
//
// JUCE's `mouseMove` fires whenever the cursor is over the component (no
// button required) as long as `setInterceptsMouseClicks` allows it AND a
// parent has called `addMouseListener` (the host editor typically does) —
// no extra opt-in needed for the default JUCE mouse-move dispatch.

void PulpEmbedComponent::mouseMove(const juce::MouseEvent& e) {
    if (view_ == nullptr) return;
    pulp_embed_dispatch_mouse_move(view_,
                                   static_cast<double>(e.position.x),
                                   static_cast<double>(e.position.y));
}

void PulpEmbedComponent::mouseEnter(const juce::MouseEvent& e) {
    if (view_ == nullptr) return;
    pulp_embed_dispatch_mouse_move(view_,
                                   static_cast<double>(e.position.x),
                                   static_cast<double>(e.position.y));
}

void PulpEmbedComponent::mouseExit(const juce::MouseEvent&) {
    if (view_ == nullptr) return;
    pulp_embed_dispatch_mouse_exit(view_);
}

}  // namespace pulp_juce
