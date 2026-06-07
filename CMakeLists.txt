# pulp-embed-juce — JUCE adapter for the pulp_view_embed C ABI.
#
# Provides PulpEmbedComponent (a juce::Component) that embeds a Pulp-imported
# design. Depends ONLY on the flat C ABI header — no Pulp C++ types.
#
# Configure (needs an installed Pulp SDK and the pulp-view-embed checkout):
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_PREFIX_PATH=/path/to/pulp-sdk-install \
#     -DPULP_VIEW_EMBED_DIR=/path/to/pulp-view-embed
cmake_minimum_required(VERSION 3.24)
project(pulp_embed_juce VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(PULP_EMBED_JUCE_BUILD_EXAMPLE "Build the standalone example app" OFF)
option(PULP_EMBED_JUCE_BUILD_TESTS "Build the headless adapter tests" ON)

if(PULP_EMBED_JUCE_BUILD_TESTS)
    enable_testing()
endif()

# ── the embedding C ABI library (separate repo) ──────────────────────────
set(PULP_VIEW_EMBED_DIR "" CACHE PATH "Path to the pulp-view-embed checkout")
if(NOT PULP_VIEW_EMBED_DIR OR NOT EXISTS "${PULP_VIEW_EMBED_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Set -DPULP_VIEW_EMBED_DIR to the pulp-view-embed checkout")
endif()
add_subdirectory("${PULP_VIEW_EMBED_DIR}" pulp-view-embed-build)

# ── JUCE ─────────────────────────────────────────────────────────────────
# Skip the fetch when the parent project has already added JUCE (e.g. a host
# plugin add_subdirectory()s its own JUCE checkout, then add_subdirectory()s
# this adapter). FetchContent_MakeAvailable is NOT idempotent across an
# already-populated JUCE source tree — re-adding it triggers duplicate target
# errors (juce_osc, juce_product_unlocking, juce_video, juce_build_tools,
# juceaide, …). Probe for a stable JUCE target name to detect that case.
if(NOT TARGET juce::juce_core)
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
else()
    message(STATUS "pulp-embed-juce: parent project already added JUCE — skipping FetchContent")
endif()

# ── the adapter library ──────────────────────────────────────────────────
add_library(pulp_embed_juce STATIC src/PulpEmbedComponent.cpp)
target_include_directories(pulp_embed_juce
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(pulp_embed_juce
    PUBLIC pulp_view_embed
           juce::juce_gui_basics
           juce::juce_gui_extra        # juce::NSViewComponent
           juce::juce_audio_processors # juce::AudioProcessor (the param-bridge ctor)
    PRIVATE juce::juce_recommended_config_flags)
target_compile_definitions(pulp_embed_juce PUBLIC
    JUCE_GLOBAL_MODULE_SETTINGS_ENABLED=0)

# The importer JS bundle dir (high-fidelity scripted-UI render) baked into the
# example/plugin so it loads the same Figma fixture the iPlug2 adapter uses.
set(PULP_EMBED_DEMO_IR "${PULP_VIEW_EMBED_DIR}/fixtures/figma-vst-style/bundle")

# ── optional standalone example ──────────────────────────────────────────
if(PULP_EMBED_JUCE_BUILD_EXAMPLE)
    juce_add_gui_app(pulp-embed-juce-demo
        PRODUCT_NAME "Pulp Embed (JUCE)"
        VERSION 0.1.0
        BUNDLE_ID dev.pulp.embed.juce.demo)
    target_sources(pulp-embed-juce-demo PRIVATE examples/standalone/Main.cpp)
    target_link_libraries(pulp-embed-juce-demo PRIVATE
        pulp_embed_juce
        juce::juce_gui_extra
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
    target_compile_definitions(pulp-embed-juce-demo PRIVATE
        PULP_EMBED_DEMO_IR="${PULP_EMBED_DEMO_IR}")
endif()

# ── real plugin (VST3 + AU) whose editor hosts PulpEmbedComponent ─────────
# Built by default so `cmake --build build` produces the plugin formats; the
# format validators (auval / pluginval / load smokes) run against these.
option(PULP_EMBED_JUCE_BUILD_PLUGIN "Build the VST3/AU plugin example" ON)
if(PULP_EMBED_JUCE_BUILD_PLUGIN)
    juce_add_plugin(PulpEmbedJucePlugin
        PRODUCT_NAME "PulpEmbedJuce"
        COMPANY_NAME "Pulp"
        BUNDLE_ID dev.pulp.embed.juce.plugin
        PLUGIN_MANUFACTURER_CODE Pulp
        PLUGIN_CODE Pemj            # unique 4-char code (distinct from iPlug2 'Pemb')
        # Standalone is the plugin wrapped as an app: it builds the SAME
        # AudioProcessorEditor the VST3/AU host opens, so a headless self-check
        # of the Standalone proves the embed renders in the real plugin editor.
        FORMATS VST3 AU Standalone
        VST3_CATEGORIES Fx
        AU_MAIN_TYPE kAudioUnitType_Effect
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        COPY_PLUGIN_AFTER_BUILD FALSE   # follow "validate before install"; install is explicit
        VERSION 0.1.0)

    juce_generate_juce_header(PulpEmbedJucePlugin)

    target_sources(PulpEmbedJucePlugin PRIVATE
        examples/plugin/PluginProcessor.cpp
        examples/plugin/PluginEditor.cpp)
    target_include_directories(PulpEmbedJucePlugin PRIVATE examples/plugin)
    target_compile_definitions(PulpEmbedJucePlugin PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)
    target_compile_definitions(PulpEmbedJucePlugin PRIVATE
        PULP_EMBED_DEMO_IR="${PULP_EMBED_DEMO_IR}")
    target_link_libraries(PulpEmbedJucePlugin PRIVATE
        pulp_embed_juce
        juce::juce_audio_utils
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
endif()

# ── headless adapter tests ────────────────────────────────────────────────
# A console self-check of the ABI v6 string API (capture/restore round-trip).
# Loads the FAITHFUL DesignIR fixture (text_fields live on that lane).
if(PULP_EMBED_JUCE_BUILD_TESTS)
    set(PULP_EMBED_FAITHFUL_IR
        "${PULP_VIEW_EMBED_DIR}/fixtures/figma-vst-style-faithful/design.ir.json")
    juce_add_console_app(pulp-embed-juce-string-test
        PRODUCT_NAME "pulp-embed-juce-string-test")
    juce_generate_juce_header(pulp-embed-juce-string-test)
    target_sources(pulp-embed-juce-string-test PRIVATE examples/string-test/main.cpp)
    target_compile_definitions(pulp-embed-juce-string-test PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        PULP_EMBED_FAITHFUL_IR="${PULP_EMBED_FAITHFUL_IR}")
    target_link_libraries(pulp-embed-juce-string-test PRIVATE
        pulp_embed_juce
        juce::juce_gui_extra
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
    # GNU ld single-pass: fontconfig must come after libskia.a (Linux SDK export).
    if(COMMAND pulp_link_fontconfig_after_skia)
        pulp_link_fontconfig_after_skia(pulp-embed-juce-string-test)
    endif()
    add_test(NAME string-test COMMAND pulp-embed-juce-string-test)
endif()
