#include "PulpEmbedComponent.h"

#include <pulp_view_embed.hpp>  // pulp::embed::param_descs / read_design_params (shared loop)
#include <juce_audio_processors/juce_audio_processors.h>  // bind to a real processor

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pulp_juce {

// Maps the embedded design's control keys to a juce::AudioProcessor's
// parameters and trampolines the flat-C host callbacks into JUCE calls.
// host_ctx (PulpEmbedDesc.host_ctx) is a pointer to one of these. Lives as long
// as the component; the view is torn down before it (see ~PulpEmbedComponent).
struct PulpEmbedComponent::HostBridge : private juce::AudioProcessorListener {
    explicit HostBridge(juce::AudioProcessor& p) : proc(p) {
        // Track live parameter-tree changes so the runtime accessor (v8
        // has_param / display-text) stays correct when a host swaps parameter
        // groups (paged racks, dynamic slots). audioProcessorChanged sets the
        // dirty flag; the map rebuilds lazily on the next UI-thread query.
        proc.addListener(this);
    }
    ~HostBridge() override { proc.removeListener(this); }

    juce::AudioProcessor& proc;
    PulpEmbedComponent*   owner = nullptr;  // for the host-action channel
    std::unordered_map<std::string, juce::AudioProcessorParameter*> byKey;
    std::vector<std::pair<std::string, juce::AudioProcessorParameter*>> bound;
    // Last value pushed UI<-host, keyed by REGISTRATION KEY rather than by index
    // into `bound`. An index-keyed vector cannot survive a key-set change: a
    // re-keyed or paged element inherits whatever its neighbour's index last
    // held, so the first push under its new key looks like a no-change and is
    // dropped. Keying by the key means a key the view has never pushed is simply
    // absent -> it always gets a correct first push, and a key that vanishes is
    // dropped on the next refresh.
    std::unordered_map<std::string, float> lastPushed;

    // The host->UI pump's dirty gate. Two independent things can invalidate the
    // pump's key->parameter bindings, so both are tracked:
    //
    //  keysDirty — the HOST's parameter tree moved (audioProcessorChanged). Keys
    //      the view registers may newly resolve or stop resolving, and a removed
    //      parameter would leave `bound` holding a dangling pointer.
    //  lastKeyGeneration — the VIEW's key set moved (a paged/tabbed control
    //      re-keyed an element, or a reload rebuilt the bridge). This is driven
    //      from inside the view, so the ABI's generation counter is the only
    //      signal the adapter gets; it is read once per tick as an integer
    //      compare (see pumpHostToUi).
    //
    // Without the gate the pump would re-enumerate the whole ABI key set every
    // tick at 30 Hz; with it, the steady state is a float compare per bound entry.
    std::atomic<bool> keysDirty{true};
    uint64_t lastKeyGeneration = 0;  // UI thread only
    int      keyResolveCount = 0;    // diagnostic: times the gate actually fired

    // Resolve UI->host writes against the LIVE all-parameters map (findAny),
    // NOT the static create-time byKey snapshot. Previously a
    // paged/dynamic control re-keyed after create (e.g. "slot1.gain") answered
    // has_param=true and rendered display text (metadata used findAny) but its
    // set_param/gesture writes went through byKey, missed, and silently never
    // reached the host. byKey is a subset of allById (both keyed by paramID),
    // so this only ever resolves MORE keys, never fewer.
    juce::AudioProcessorParameter* find(const char* key) { return findAny(key); }

    // ── flat-C host callbacks (UI -> host). ctx == this. ────────────────────
    static void setParam(void* ctx, const char* key, double normalized) {
        if (auto* p = static_cast<HostBridge*>(ctx)->find(key))
            p->setValueNotifyingHost((float) juce::jlimit(0.0, 1.0, normalized));
    }
    static double getParam(void* ctx, const char* key) {
        if (auto* p = static_cast<HostBridge*>(ctx)->find(key)) return p->getValue();
        // Unknown key: -1.0 sentinel (matches the iPlug2 adapter). The shim
        // treats any [0,1] return as authoritative, so returning 0.0 here forced
        // every UNBOUND imported control to 0 instead of keeping its imported
        // default. An out-of-range value tells the shim "no host opinion".
        return -1.0;
    }
    static void beginGesture(void* ctx, const char* key) {
        if (auto* p = static_cast<HostBridge*>(ctx)->find(key)) p->beginChangeGesture();
    }
    static void endGesture(void* ctx, const char* key) {
        if (auto* p = static_cast<HostBridge*>(ctx)->find(key)) p->endChangeGesture();
    }

    // ── runtime host-param accessor backing (ABI v8 adapter half) ────────────
    // A paramID-keyed view of ALL the processor's parameters (not just the ones
    // a design control bound to at create). Rebuilt lazily when the processor's
    // parameter tree changes, so dynamic/paged controls resolve late-added keys.
    std::atomic<bool> allDirty{true};
    std::unordered_map<std::string, juce::AudioProcessorParameter*> allById;
    // Display-text memo keyed by (paramID, exact normalized bits) so repeated
    // per-tick queries never re-run a plugin's getText override.
    std::unordered_map<std::string, std::string> displayCache;
    std::unordered_set<std::string> loggedMisses;  // unresolved keys logged once

    void rebuildAllIfDirty() {
        if (!allDirty.exchange(false)) return;
        allById.clear();
        displayCache.clear();  // stale once the parameter set changes
        for (auto* p : proc.getParameters())
            if (auto* wid = dynamic_cast<juce::AudioProcessorParameterWithID*>(p))
                allById[wid->paramID.toStdString()] = p;
    }
    juce::AudioProcessorParameter* findAny(const char* key) {
        rebuildAllIfDirty();
        const auto it = allById.find(key ? key : "");
        return it == allById.end() ? nullptr : it->second;
    }
    void logMissOnce(const char* key) {
        const std::string k = key ? key : "";
        if (loggedMisses.insert(k).second)
            juce::Logger::writeToLog("[pulp-embed] host-param key unresolved: " +
                                     juce::String::fromUTF8(k.c_str()));
    }
    // Formatted display text for `key` at `normalized`, memoized per (key,value).
    // Returns false when the key is unknown (and logs the miss once).
    bool displayText(const char* key, double normalized, std::string& out) {
        auto* p = findAny(key);
        if (p == nullptr) { logMissOnce(key); return false; }
        const double v = juce::jlimit(0.0, 1.0, normalized);
        uint64_t bits = 0;
        std::memcpy(&bits, &v, sizeof bits);  // exact (key, value) memo key
        std::string cacheKey(key ? key : "");
        cacheKey.push_back('\x1f');
        cacheKey.append(reinterpret_cast<const char*>(&bits), sizeof bits);
        const auto it = displayCache.find(cacheKey);
        if (it != displayCache.end()) { out = it->second; return true; }
        out = p->getText((float) v, 0).toStdString();
        displayCache.emplace(std::move(cacheKey), out);
        return true;
    }

    // ── v8 flat-C host callbacks (host_ctx == this). Trampoline into the
    //    backing above / the owning component's onHostAction. ────────────────
    static int hasParam(void* ctx, const char* key) {
        return static_cast<HostBridge*>(ctx)->findAny(key) != nullptr ? 1 : 0;
    }
    static size_t paramDisplayText(void* ctx, const char* key, double normalized,
                                   char* buf, size_t cap) {
        std::string s;
        if (!static_cast<HostBridge*>(ctx)->displayText(key, normalized, s)) return 0;
        const size_t n = s.size();
        if (buf != nullptr && cap > 0) {
            const size_t c = n < cap - 1 ? n : cap - 1;
            std::memcpy(buf, s.data(), c);
            buf[c] = '\0';
        }
        return n;
    }
    // Discrete step count for `key`, or 0 for continuous/unknown (the ABI's
    // single don't-know answer). This is the divisor authority for a discrete
    // control: a design radio with 3 visible options may be bound to a 6-step
    // parameter, and only the host knows which is real.
    //
    // JUCE has no "is continuous" predicate — AudioProcessorParameter::getNumSteps
    // returns a large SENTINEL (AudioProcessor::getDefaultNumParameterSteps(),
    // 0x7fffffff) for a parameter that never declared a step interval, so a
    // continuous param would otherwise report an absurd count. Map the sentinel,
    // and any non-positive answer, to the contract's 0.
    int32_t stepCount(const char* key) {
        auto* p = findAny(key);
        if (p == nullptr) return 0;
        const int steps = p->getNumSteps();
        if (steps >= juce::AudioProcessor::getDefaultNumParameterSteps()) return 0;
        return steps > 0 ? static_cast<int32_t>(steps) : 0;
    }
    static int32_t hostParamSteps(void* ctx, const char* key) {
        return static_cast<HostBridge*>(ctx)->stepCount(key);
    }

    static int hostAction(void* ctx, const char* action, const char* args_json) {
        auto* b = static_cast<HostBridge*>(ctx);
        if (b->owner == nullptr) return 0;
        return b->owner->dispatchHostAction(juce::String::fromUTF8(action ? action : ""),
                                            juce::String::fromUTF8(args_json ? args_json : ""))
                   ? 1 : 0;
    }

    // juce::AudioProcessorListener — only the tree-shape change matters here.
    // It invalidates BOTH derived maps: the paramID view (allById) and the
    // pump's key->parameter bindings, whose raw parameter pointers would
    // otherwise dangle if the host removed a parameter.
    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails&) override {
        allDirty.store(true);
        keysDirty.store(true);
    }
    void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override {}

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
    bridge_->owner = this;  // host-action channel routes back to onHostAction
    createView(source, logicalWidth, logicalHeight);
}

PulpEmbedComponent::PulpEmbedComponent(pulp::embed::NativeViewFactory factory,
                                       int logicalWidth, int logicalHeight) {
    createViewFromFactory(std::move(factory), logicalWidth, logicalHeight);
}

PulpEmbedComponent::PulpEmbedComponent(pulp::embed::NativeViewFactory factory,
                                       int logicalWidth, int logicalHeight,
                                       juce::AudioProcessor& processor)
    : bridge_(std::make_unique<HostBridge>(processor)) {
    bridge_->owner = this;  // host-action channel routes back to onHostAction
    createViewFromFactory(std::move(factory), logicalWidth, logicalHeight);
}

PulpEmbedDesc PulpEmbedComponent::buildDesc(int logicalWidth, int logicalHeight) const {
    PulpEmbedDesc desc{};
    desc.struct_size = sizeof(PulpEmbedDesc);
    // Clamp to what the RUNTIME library supports (statically-linked shim today,
    // but a future prebuilt dylib may predate these headers). check_desc rejects
    // abi_version > library version, so a header-newer-than-library skew must
    // negotiate DOWN — the shim's struct_size/offset clamp then simply ignores
    // callbacks the older library doesn't know about.
    desc.abi_version = static_cast<uint32_t>(
        juce::jmin<uint32_t>(PULP_VIEW_EMBED_ABI_VERSION, pulp_embed_abi_version()));
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
       #if PULP_VIEW_EMBED_ABI_VERSION >= 8
        // ABI v8 tail-append: runtime host-param accessor + action channel.
        // Gated on the header version so the adapter still compiles against a
        // pre-v8 pulp_view_embed.h (the struct lacks these members there). The
        // trampolines above are unconditional and unit-tested via the
        // hostHasParam / hostParamDisplayText / dispatchHostAction seams, so the
        // backing is exercised even before the v8 ABI lands in the sibling repo.
        desc.host.has_param = &HostBridge::hasParam;
        desc.host.param_display_text = &HostBridge::paramDisplayText;
        desc.host.host_action = &HostBridge::hostAction;
       #endif
       #if PULP_VIEW_EMBED_ABI_VERSION >= 10
        // ABI v10 tail-append: the host's discrete step count per key. Gated on
        // the header version like the v8 tail above; a runtime library older than
        // v10 simply stops before this field (struct_size gating), so the
        // trampoline is never called.
        desc.host.host_param_steps = &HostBridge::hostParamSteps;
       #endif
    }
    return desc;
}

// A Debug build of the Skia/Dawn render stack runs roughly ~3x the CPU
// of Release (no -O3/NDEBUG, live asserts, no inlining). A UX-perceived "the
// embed is slow" regression in a Debug build is almost always the build type,
// not the code. Emit one loud line so anyone measuring in Debug knows to
// re-measure in Release. Once per process; compiled out entirely in Release.
static void logDebugCpuNoticeOnce() {
   #if JUCE_DEBUG
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        juce::Logger::writeToLog(
            "[pulp-embed] built Debug — expect ~3x CPU; measure Release before "
            "judging embed performance.");
   #endif
}

// Shared post-create steps for every create path: attach Pulp's child view to
// the JUCE component (macOS), resolve the host-param bindings, and start the
// 30 Hz tick. Call only after view_ is non-null.
void PulpEmbedComponent::attachAndStart() {
    logDebugCpuNoticeOnce();
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
}

void PulpEmbedComponent::createView(const juce::File& source,
                                    int logicalWidth, int logicalHeight) {
    // Do NOT force the design size here. Retain it (design viewport pin
    // + configureResizableEditor fallback), but leave the component 0x0 until
    // the owning editor drives a real size; the first non-zero resized() issues
    // the first pulp_embed_resize. Forcing the design size in the ctor and then
    // letting the editor set a (possibly zoomed) size double-rendered on reopen.
    logicalWidth_ = logicalWidth;
    logicalHeight_ = logicalHeight;

    PulpEmbedDesc desc = buildDesc(logicalWidth, logicalHeight);

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

    attachAndStart();

    // Dev hot-reload: for a bundle, remember its ui.js and auto-enable the
    // watcher when PULP_EMBED_HOT_RELOAD is set in the environment.
    if (isBundle) {
        watchFile_ = source.isDirectory() ? source.getChildFile("ui.js") : source;
        if (std::getenv("PULP_EMBED_HOT_RELOAD") != nullptr)
            enableBundleHotReload(true);
    }
}

void PulpEmbedComponent::createViewFromFactory(pulp::embed::NativeViewFactory factory,
                                               int logicalWidth, int logicalHeight) {
    // See createView: retain the design size, don't force it as the
    // component size. The owning editor drives size on open.
    logicalWidth_ = logicalWidth;
    logicalHeight_ = logicalHeight;

    PulpEmbedDesc desc = buildDesc(logicalWidth, logicalHeight);

    // Mount the host's compiled View (e.g. a DesignFrameView subclass). Its
    // param_key'd elements bind through the same host bridge configured above.
    // No file source, so no hot-reload watcher.
    PulpEmbedResult r = pulp::embed::pulp_embed_create_from_view(&desc, std::move(factory), &view_);
    if (r != PULP_EMBED_OK || view_ == nullptr) {
        view_ = nullptr;
        return;
    }

    attachAndStart();
}

// Re-enumerate the view's registration keys and re-resolve each against the LIVE
// parameter map, replacing any previous bindings.
//
// The view's key set is NOT fixed for its lifetime: a paged/tabbed control
// re-keys its elements at runtime, and a bundle reload rebuilds the list
// wholesale. Resolving through findAny (the same live accessor the UI->host
// writes already use) is what makes a late-added or re-keyed parameter bind at
// all — a create-time snapshot answers for keys that no longer exist and misses
// the ones that do.
void PulpEmbedComponent::refreshBoundKeys() {
    ++bridge_->keyResolveCount;
    bridge_->byKey.clear();
    bridge_->bound.clear();

    // Unmatched keys stay visual-only (no binding) — never guessed.
    const int32_t n = pulp_embed_param_count(view_);
    for (int32_t i = 0; i < n; ++i) {
        char key[256] = {0};
        pulp_embed_param_key(view_, i, key, sizeof key);
        auto* p = bridge_->findAny(key);
        if (p == nullptr) continue;
        bridge_->byKey[key] = p;
        bridge_->bound.emplace_back(key, p);
    }

    // Forget the push memo for any key the view no longer registers, so a key
    // that later comes back is pushed fresh rather than compared against a value
    // from a previous binding.
    for (auto it = bridge_->lastPushed.begin(); it != bridge_->lastPushed.end();)
        it = bridge_->byKey.count(it->first) ? std::next(it)
                                             : bridge_->lastPushed.erase(it);
}

// The dirty gate. Re-resolves only when the host's parameter tree moved
// (keysDirty) or the view's key set moved (the ABI generation counter changed) —
// otherwise this is one atomic read plus one integer compare, and the caller
// falls straight through to the value poll.
void PulpEmbedComponent::refreshBoundKeysIfDirty() {
   #if PULP_VIEW_EMBED_ABI_VERSION >= 10
    const uint64_t gen = pulp_embed_param_key_generation(view_);
   #else
    // Pre-v10 runtimes expose no key-set signal, so a view-driven re-key is
    // invisible here and only a host tree change re-resolves.
    const uint64_t gen = bridge_->lastKeyGeneration;
   #endif
    const bool hostMoved = bridge_->keysDirty.exchange(false);
    if (!hostMoved && gen == bridge_->lastKeyGeneration) return;
    bridge_->lastKeyGeneration = gen;
    refreshBoundKeys();
}

void PulpEmbedComponent::resolveParameterBindings() {
    if (bridge_ == nullptr || view_ == nullptr) return;
    bridge_->keysDirty.store(true);
    refreshBoundKeysIfDirty();

    // Push initial values UI<-host so controls reflect the current state on open.
    for (const auto& [key, p] : bridge_->bound) {
        const float v = p->getValue();
        pulp_embed_param_changed(view_, key.c_str(), v);
        bridge_->lastPushed[key] = v;
    }
}

void PulpEmbedComponent::pumpHostToUi() {
    if (bridge_ == nullptr || view_ == nullptr) return;
    // Re-resolve the key set only when something can have changed it; the
    // steady-state pump below stays a cheap float compare per bound entry.
    refreshBoundKeysIfDirty();

    // Poll bound parameters on the UI thread (pulp_embed_param_changed is
    // UI-thread-only) and push host-side changes — automation, preset recall,
    // a sibling editor — into the matching control. Fine for the
    // hundreds-of-params case at 30 Hz.
    for (const auto& [key, p] : bridge_->bound) {
        const float v = p->getValue();
        // An absent memo entry is a key this view has not pushed yet (a fresh
        // bind, or one re-keyed since the last refresh) — it must always push.
        const auto memo = bridge_->lastPushed.find(key);
        if (memo != bridge_->lastPushed.end() && memo->second == v) continue;
        pulp_embed_param_changed(view_, key.c_str(), v);
        bridge_->lastPushed[key] = v;
    }
}

int PulpEmbedComponent::boundParameterCount() const noexcept {
    return bridge_ != nullptr ? static_cast<int>(bridge_->bound.size()) : 0;
}

// ── runtime host-param accessor (ABI v8 adapter half) ────────────────────────

bool PulpEmbedComponent::hostHasParam(const juce::String& key) const {
    return bridge_ != nullptr && bridge_->findAny(key.toRawUTF8()) != nullptr;
}

juce::String PulpEmbedComponent::hostParamDisplayText(const juce::String& key,
                                                      double normalized) const {
    if (bridge_ == nullptr) return {};
    std::string out;
    if (!bridge_->displayText(key.toRawUTF8(), normalized, out)) return {};
    return juce::String::fromUTF8(out.c_str());
}

int PulpEmbedComponent::hostParamStepCount(const juce::String& key) const {
    if (bridge_ == nullptr) return 0;
    return static_cast<int>(bridge_->stepCount(key.toRawUTF8()));
}

void PulpEmbedComponent::syncFromHost() { pumpHostToUi(); }

int PulpEmbedComponent::keyResolveCount() const noexcept {
    return bridge_ != nullptr ? bridge_->keyResolveCount : 0;
}

bool PulpEmbedComponent::hostWriteParam(const juce::String& key, double normalized) {
    if (bridge_ == nullptr) return false;
    auto* p = bridge_->find(key.toRawUTF8());  // live resolve (== findAny)
    if (p == nullptr) return false;
    p->setValueNotifyingHost((float) juce::jlimit(0.0, 1.0, normalized));
    return true;
}
bool PulpEmbedComponent::hostBeginGesture(const juce::String& key) {
    if (bridge_ == nullptr) return false;
    auto* p = bridge_->find(key.toRawUTF8());
    if (p == nullptr) return false;
    p->beginChangeGesture();
    return true;
}
bool PulpEmbedComponent::hostEndGesture(const juce::String& key) {
    if (bridge_ == nullptr) return false;
    auto* p = bridge_->find(key.toRawUTF8());
    if (p == nullptr) return false;
    p->endChangeGesture();
    return true;
}

// ── host action/command channel (ABI v8 adapter half) ────────────────────────

bool PulpEmbedComponent::dispatchHostAction(const juce::String& action,
                                            const juce::String& argsJson) {
    if (!onHostAction) return false;
    // Parse the args to a juce::var (null var when empty / malformed — the
    // handler still fires so a no-arg action works).
    juce::var args;
    if (argsJson.isNotEmpty()) {
        const auto parsed = juce::JSON::parse(argsJson);
        if (!parsed.isVoid()) args = parsed;
    }
    return onHostAction(action, args);
}

// ── resizable editor helper ──────────────────────────────────────────────────

void PulpEmbedComponent::configureResizableEditor(juce::AudioProcessorEditor& editor) {
    if (view_ == nullptr) return;

    PulpEmbedSizeHints hints{};
    if (pulp_embed_size_hints(view_, &hints) != PULP_EMBED_OK) return;
    if (!hints.resizable) return;  // design opts out — leave the editor fixed

    // Host-window resize only: the heavyweight embed NSView covers JUCE's
    // lightweight corner grip, so useBottomRightCornerResizer is false. The
    // embed letterboxes content to the design viewport itself — we only
    // constrain the host window here (no transform re-derivation).
    editor.setResizable(true, /*useBottomRightCornerResizer*/ false);

    constrainer_ = std::make_unique<juce::ComponentBoundsConstrainer>();
    const int prefW = hints.preferred_width  > 0 ? hints.preferred_width  : logicalWidth_;
    const int prefH = hints.preferred_height > 0 ? hints.preferred_height : logicalHeight_;
    const int minW = hints.min_width  > 0 ? hints.min_width  : prefW;
    const int minH = hints.min_height > 0 ? hints.min_height : prefH;
    // 0 = unbounded in the hints; JUCE wants a concrete cap, so use a large one.
    const int maxW = hints.max_width  > 0 ? hints.max_width  : 32768;
    const int maxH = hints.max_height > 0 ? hints.max_height : 32768;
    constrainer_->setSizeLimits(minW, minH, maxW, maxH);
    if (hints.aspect_ratio > 0.0f)
        constrainer_->setFixedAspectRatio((double) hints.aspect_ratio);
    editor.setConstrainer(constrainer_.get());

    // Size-on-open: the design's preferred size (the constrainer keeps it legal).
    editor.setSize(prefW, prefH);
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

double PulpEmbedComponent::controlValue(int index) const {
    if (view_ == nullptr || index < 0 || index >= pulp_embed_param_count(view_))
        return -1.0;
    return pulp_embed_param_value(view_, index);
}

namespace {
// Drive-loop tuning. The loop MEASURES the control's response instead of
// modelling it, so these only bound the search, they do not encode any law.
constexpr double kDriveTolerance = 1.0e-3;  // "arrived" (values are normalized)
constexpr double kDriveProbePx   = 12.0;    // first probe travel, view px
constexpr int    kDriveMaxSteps  = 16;      // secant refinements before giving up
constexpr double kDriveDeadZone  = 1.0e-9;  // below this a probe moved nothing
}  // namespace

bool PulpEmbedComponent::simulateParamDragToValue(const juce::String& key,
                                                  double normalized) {
    if (view_ == nullptr) return false;

    const int index = indexOfKey(key);
    if (index < 0) {
        // A key the view does not register. Loud, because the caller believes this
        // control exists — a quiet false would let a typo'd or renamed key look
        // like a tested control forever.
        juce::Logger::writeToLog("PulpEmbedComponent::simulateParamDragToValue: no control "
                                 "registered under key '" + key + "'");
        return false;
    }

    double target = juce::jlimit(0.0, 1.0, normalized);
    // Snap onto the HOST's step grid for a discrete parameter. The count is a
    // number of steps, so the last index -- and the divisor -- is count - 1.
    // The design's own option count is NOT the authority here (a design may draw
    // fewer options than the parameter has steps), which is why this asks the host.
    //
    // Read through the embed rather than hostParamStepCount(): both ultimately
    // come from this component -- the v10 callback trampolines into it -- but the
    // embed answers from the snapshot it refreshes at tick, and that snapshot is
    // what the VIEW scales the control by. This drive presses the control and then
    // verifies arrival with controlValue(), a view-side read, so its target has to
    // sit on the grid the view is actually on. Snapping to the live map instead
    // would, in the window between a host-side change and the next tick, compute a
    // target the control cannot land on and then report the miss as a failure.
    const int steps = static_cast<int>(pulp_embed_param_steps(view_, key.toRawUTF8()));
    if (steps > 1) {
        const double divisor = static_cast<double>(steps - 1);
        target = std::round(target * divisor) / divisor;
    }

    double hx = 0.0, hy = 0.0;
    if (pulp_embed_param_hit_point(view_, index, &hx, &hy) != PULP_EMBED_OK) {
        char err[512] = {0};
        pulp_embed_last_error(view_, err, sizeof err);
        juce::Logger::writeToLog("PulpEmbedComponent::simulateParamDragToValue: cannot locate "
                                 "control '" + key + "': " + juce::String::fromUTF8(err));
        return false;
    }

    // Already there: do nothing at all, and in particular do NOT press. A press is
    // not a neutral probe — on a latching control (a toggle) it FLIPS, so pressing
    // first and asking questions after would drive a correct control to the wrong
    // value and then report failure. Nothing to do is a success.
    if (std::abs(controlValue(index) - target) <= kDriveTolerance) return true;

    // From here on a gesture is open, so every exit must release it: leaving the
    // pointer captured would strand the host's undo bracket open and wedge the
    // next drive.
    pulp_embed_dispatch_mouse_down(view_, hx, hy);

    double y0 = hy, v0 = controlValue(index);
    const auto release = [&](double y) {
        pulp_embed_dispatch_mouse_up(view_, hx, y);
        return std::abs(controlValue(index) - target) <= kDriveTolerance;
    };
    // A latching control commits on the press itself and ignores the drag, so it
    // has already arrived (or never will) by now.
    if (std::abs(v0 - target) <= kDriveTolerance) return release(y0);

    // Probe once TOWARDS the target to measure the control's response. Probing
    // toward it keeps the sample off the far clamp, where the response would read
    // as flat. Which way is "increase" is the control's business, so if the first
    // direction moves nothing, try the other before concluding it is dead.
    const auto probe = [&](double dir) {
        const double y = hy - dir * kDriveProbePx;
        pulp_embed_dispatch_mouse_drag(view_, hx, y);
        return std::pair<double, double>{y, controlValue(index)};
    };
    auto [y1, v1] = probe(target > v0 ? 1.0 : -1.0);
    if (std::abs(v1 - v0) < kDriveDeadZone) std::tie(y1, v1) = probe(target > v0 ? -1.0 : 1.0);
    if (std::abs(v1 - v0) < kDriveDeadZone) {
        juce::Logger::writeToLog("PulpEmbedComponent::simulateParamDragToValue: control '" + key +
                                 "' did not respond to a drag (is it enabled / draggable?)");
        release(y1);
        return false;
    }

    // Secant search on the control's OWN response curve: each drag is measured,
    // never predicted. This is what keeps the drive independent of the drag law --
    // sensitivity, direction and scaling can all change in the view without
    // touching this, and a law that regressed would fail to converge rather than
    // be faithfully reproduced by a matching inverse here.
    for (int step = 0; step < kDriveMaxSteps && std::abs(v1 - target) > kDriveTolerance; ++step) {
        const double slope = (v1 - v0) / (y1 - y0);
        if (std::abs(slope) < kDriveDeadZone) break;  // flat: clamped or stuck
        const double y2 = y1 + (target - v1) / slope;
        pulp_embed_dispatch_mouse_drag(view_, hx, y2);
        y0 = y1; v0 = v1;
        y1 = y2; v1 = controlValue(index);
    }

    const bool arrived = release(y1);
    if (!arrived)
        juce::Logger::writeToLog("PulpEmbedComponent::simulateParamDragToValue: control '" + key +
                                 "' stopped at " + juce::String(controlValue(index), 4) +
                                 ", short of " + juce::String(target, 4));
    return arrived;
}

int PulpEmbedComponent::indexOfKey(const juce::String& key) const {
    if (view_ == nullptr) return -1;
    const int32_t n = pulp_embed_param_count(view_);
    for (int32_t i = 0; i < n; ++i) {
        char buf[256] = {0};
        pulp_embed_param_key(view_, i, buf, sizeof buf);
        if (key == juce::String::fromUTF8(buf)) return i;
    }
    return -1;
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
    // Ignore zero-size layout passes; the first NON-ZERO resized() is the
    // first pulp_embed_resize (the ctor no longer forces the design size).
    if (getWidth() <= 0 || getHeight() <= 0) return;
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

void PulpEmbedComponent::mouseDown(const juce::MouseEvent& e) {
    if (view_ == nullptr) return;
    pulp_embed_dispatch_mouse_down(view_,
                                   static_cast<double>(e.position.x),
                                   static_cast<double>(e.position.y));
}

void PulpEmbedComponent::mouseDrag(const juce::MouseEvent& e) {
    if (view_ == nullptr) return;
    pulp_embed_dispatch_mouse_drag(view_,
                                   static_cast<double>(e.position.x),
                                   static_cast<double>(e.position.y));
}

void PulpEmbedComponent::mouseUp(const juce::MouseEvent& e) {
    if (view_ == nullptr) return;
    pulp_embed_dispatch_mouse_up(view_,
                                 static_cast<double>(e.position.x),
                                 static_cast<double>(e.position.y));
}

}  // namespace pulp_juce
