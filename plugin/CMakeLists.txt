# ElfDrumLXR — Phase 1 JUCE plugin shell.
# Dependencies are pinned and fetched by scripts, never vendored into git:
#   ../scripts/fetch-vendor.sh   firmware  -> ../vendor/lxr-02   (commit a43cd4f)
#   ./fetch-deps.sh              JUCE 8.0.9 + r8brain (pinned commit) -> vendor/
cmake_minimum_required(VERSION 3.22)
project(ElfDrumLXR VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/JUCE/CMakeLists.txt")
    message(FATAL_ERROR "JUCE not found in plugin/vendor — run plugin/fetch-deps.sh first.")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/r8brain-free-src/CDSPResampler.h")
    message(FATAL_ERROR "r8brain not found in plugin/vendor — run plugin/fetch-deps.sh first.")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/lxr-02/Core")
    message(FATAL_ERROR "Firmware not found in vendor/lxr-02 — run scripts/fetch-vendor.sh first.")
endif()

# ---------------------------------------------------------------------------
# Engine sources: the same copy + patch pipeline as the Phase 0 Makefile.
# vendor/ stays pristine; the patched copy lives in the build tree.
# ---------------------------------------------------------------------------
set(ENGINE_SRC "${CMAKE_CURRENT_BINARY_DIR}/engine-src")
file(REMOVE_RECURSE "${ENGINE_SRC}")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/lxr-02/Core" DESTINATION "${ENGINE_SRC}")
find_program(PATCH_PROGRAM patch REQUIRED)
file(GLOB PATCH_FILES "${CMAKE_CURRENT_SOURCE_DIR}/../patches/*.patch")
list(SORT PATCH_FILES)
foreach(patch_file IN LISTS PATCH_FILES)
    execute_process(COMMAND "${PATCH_PROGRAM}" -s -p1 -d "${ENGINE_SRC}"
                    INPUT_FILE "${patch_file}"
                    RESULT_VARIABLE patch_result)
    if(NOT patch_result EQUAL 0)
        message(FATAL_ERROR "Failed to apply ${patch_file}")
    endif()
endforeach()

file(GLOB ENGINE_C_SOURCES
    "${ENGINE_SRC}/Core/Src/Soundengine/DSPAudio/*.c"
    "${ENGINE_SRC}/Core/Src/Soundengine/DSPAudio/FX/*.c"
    "${ENGINE_SRC}/Core/Src/Soundengine/DSPAudio/FX/*/*.c"
    "${ENGINE_SRC}/Core/Src/Soundengine/FxBus.c"
    "${ENGINE_SRC}/Core/Src/Soundengine/MIDI/*.c"
    "${ENGINE_SRC}/Core/Src/Soundengine/Sequencer/*.c"
    "${ENGINE_SRC}/Core/Src/menu/Cc2Text.c")
list(REMOVE_ITEM ENGINE_C_SOURCES "${ENGINE_SRC}/Core/Src/Soundengine/MIDI/Uart.c")

# Consumers link this archive WHOLE (see the WHOLE_ARCHIVE entries below):
# the per-instance swap's census (tools/gen_census.py) is taken over the
# archive, so every member object — including ones no host code references,
# e.g. MidiMessages.c — must actually be present in the final binary.
# The engine archive is linked WHOLE on Linux only: the per-instance
# swap's census is taken over the archive, so every member object must be
# present in the final binary (see EngineSwap). Elsewhere the swap is
# unavailable by design (ELF-only) and plain archive linking is correct.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(LXR_ENGINE_LINK "$<LINK_LIBRARY:WHOLE_ARCHIVE,lxr_engine>")
else()
    set(LXR_ENGINE_LINK lxr_engine)
endif()

add_library(lxr_engine STATIC
    ${ENGINE_C_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/../engine/engine.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../engine/stubs.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../engine/seqbridge.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../engine/kit.c")
set_target_properties(lxr_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(lxr_engine PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../shim"
    "${CMAKE_CURRENT_SOURCE_DIR}/../engine"
    "${ENGINE_SRC}/Core/Inc"
    "${ENGINE_SRC}/Core/Inc/Soundengine"
    "${ENGINE_SRC}/Core/Src")
# __fp16: exact IEEE half, as decided in Phase 0 (_Float16; override with -DELF_FP16=float).
set(ELF_FP16 "_Float16" CACHE STRING "Host type mapped onto the firmware's __fp16")
target_compile_definitions(lxr_engine PRIVATE "__fp16=${ELF_FP16}" "section(x)=unused")
target_compile_options(lxr_engine PRIVATE -w)
target_link_libraries(lxr_engine PRIVATE m)

# ---------------------------------------------------------------------------
# JUCE plugin + offline renderer
# ---------------------------------------------------------------------------
add_subdirectory(vendor/JUCE)

if(APPLE)
    set(ELF_FORMATS AU VST3 Standalone)
else()
    set(ELF_FORMATS VST3 Standalone)
endif()

juce_add_plugin(ElfDrumLXR
    PRODUCT_NAME "ElfDrumLXR"
    COMPANY_NAME "Elf"
    BUNDLE_ID "com.garth.elfdrumlxr"
    PLUGIN_MANUFACTURER_CODE ElfM
    PLUGIN_CODE Edlx
    FORMATS ${ELF_FORMATS}
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    AU_MAIN_TYPE kAudioUnitType_MusicDevice
    VST3_CATEGORIES "Instrument" "Drum")

target_sources(ElfDrumLXR PRIVATE
    src/PluginProcessor.cpp
    src/PluginEditor.cpp
    src/SeqEditor.cpp
    src/Phase5.cpp
    src/Phase5Editor.cpp
    src/EngineHost.cpp src/EngineSwap.cpp)
target_include_directories(ElfDrumLXR PRIVATE
    src
    vendor/r8brain-free-src)
target_compile_definitions(ElfDrumLXR PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(ElfDrumLXR PRIVATE
    ${LXR_ENGINE_LINK}
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_data_structures
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# Offline gate harness: instantiates the processor headlessly (see render/PluginRender.cpp).
juce_add_console_app(plugin_render PRODUCT_NAME "plugin-render")
target_sources(plugin_render PRIVATE
    render/PluginRender.cpp
    src/PluginProcessor.cpp
    src/EngineHost.cpp src/EngineSwap.cpp)
target_include_directories(plugin_render PRIVATE
    src
    vendor/r8brain-free-src)
target_compile_definitions(plugin_render PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    ELF_NO_EDITOR=1)
target_link_libraries(plugin_render PRIVATE
    ${LXR_ENGINE_LINK}
    juce::juce_audio_basics
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_core
    juce::juce_data_structures
    juce::juce_events)

# Morph/save proof harness (see render/PluginMorphTest.cpp).
juce_add_console_app(plugin_morph_test PRODUCT_NAME "plugin-morph-test")
target_sources(plugin_morph_test PRIVATE
    render/PluginMorphTest.cpp
    src/PluginProcessor.cpp
    src/EngineHost.cpp src/EngineSwap.cpp)
target_include_directories(plugin_morph_test PRIVATE
    src
    vendor/r8brain-free-src)
target_compile_definitions(plugin_morph_test PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    ELF_NO_EDITOR=1)
target_link_libraries(plugin_morph_test PRIVATE
    ${LXR_ENGINE_LINK}
    juce::juce_audio_basics
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_core
    juce::juce_data_structures
    juce::juce_events)

# Phase 3 sequencer harness: trigger-log equivalence vs tools/seq-ref (gate b,
# byte diff of the two logs), audio onset sanity (gate e), state v4 round-trip.
# The --wrap interception of the firmware's trigger/automation entry points is
# a GNU ld feature; the target is therefore Linux-only (the Mac runs the panel
# and render harnesses as its sanity gates).
if(NOT APPLE)
juce_add_console_app(plugin_seq_test PRODUCT_NAME "plugin-seq-test")
target_sources(plugin_seq_test PRIVATE
    render/PluginSeqTest.cpp
    src/PluginProcessor.cpp
    src/EngineHost.cpp src/EngineSwap.cpp)
target_include_directories(plugin_seq_test PRIVATE
    src
    ../tools
    vendor/r8brain-free-src)
target_compile_definitions(plugin_seq_test PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    ELF_NO_EDITOR=1)
target_link_libraries(plugin_seq_test PRIVATE
    ${LXR_ENGINE_LINK}
    juce::juce_audio_basics
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_core
    juce::juce_data_structures
    juce::juce_events)
target_link_options(plugin_seq_test PRIVATE
    -Wl,--wrap=voiceControl_noteOn,--wrap=autoNode_setDestination,--wrap=autoNode_updateValue)
endif()

# Phase 4 stem harness: all 8 output buses live (16 channels), per-stem
# isolation + mains-vs-golden proof (tools/stem-gate.sh).
juce_add_console_app(plugin_stem_test PRODUCT_NAME "plugin-stem-test")
target_sources(plugin_stem_test PRIVATE
    render/PluginStemTest.cpp
    src/PluginProcessor.cpp
    src/EngineHost.cpp src/EngineSwap.cpp)
target_include_directories(plugin_stem_test PRIVATE
    src
    vendor/r8brain-free-src)
target_compile_definitions(plugin_stem_test PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    ELF_NO_EDITOR=1)
target_link_libraries(plugin_stem_test PRIVATE
    ${LXR_ENGINE_LINK}
    juce::juce_audio_basics
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_core
    juce::juce_data_structures
    juce::juce_events)

# ---------------------------------------------------------------------------
# Phase 4: CLAP via clap-juce-extensions (optional target, dep pinned by
# fetch-deps.sh). Same processor, same gates — the CLAP smoke harness below
# loads the built .clap with a minimal independent host and renders.
# ---------------------------------------------------------------------------
option(ELF_BUILD_CLAP "Build the CLAP target via clap-juce-extensions" ON)
if(ELF_BUILD_CLAP AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/clap-juce-extensions/CMakeLists.txt")
    add_subdirectory(vendor/clap-juce-extensions)
    clap_juce_extensions_plugin(TARGET ElfDrumLXR
        CLAP_ID "com.garth.elfdrumlxr"
        CLAP_FEATURES instrument drum-machine)

    # Minimal CLAP host for the headless load/render smoke (tools not linked
    # against JUCE or the plugin — it dlopen()s the .clap like a real host).
    # Linux-only (dlfcn); the .clap itself builds on every platform.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(clap_smoke_test render/ClapSmokeTest.cpp)
        target_include_directories(clap_smoke_test PRIVATE
            vendor/clap-juce-extensions/clap-libs/clap/include)
        target_link_libraries(clap_smoke_test PRIVATE ${CMAKE_DL_LIBS})
        set_target_properties(clap_smoke_test PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/clap_smoke_test_artefacts")
    endif()
elseif(ELF_BUILD_CLAP)
    message(WARNING "clap-juce-extensions not in plugin/vendor — run plugin/fetch-deps.sh; CLAP target skipped.")
endif()

# Phase 2 panel harness: metadata cross-check (APVTS side), voice-slice
# byte+render proof, state round-trip, guarded-randomise guards.
juce_add_console_app(plugin_panel_test PRODUCT_NAME "plugin-panel-test")
target_sources(plugin_panel_test PRIVATE
    render/PanelTest.cpp
    src/PluginProcessor.cpp
    src/EngineHost.cpp src/EngineSwap.cpp)
target_include_directories(plugin_panel_test PRIVATE
    src
    vendor/r8brain-free-src)
target_compile_definitions(plugin_panel_test PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    ELF_NO_EDITOR=1)
target_link_libraries(plugin_panel_test PRIVATE
    ${LXR_ENGINE_LINK}
    juce::juce_audio_basics
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_core
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events)

# Phase 5 gate harness: one mode per bonus-programme item (see
# render/Phase5Test.cpp). Same processor + Phase 5 layer as the plugin.
juce_add_console_app(plugin_phase5_test PRODUCT_NAME "plugin-phase5-test")
target_sources(plugin_phase5_test PRIVATE
    render/Phase5Test.cpp
    src/PluginProcessor.cpp
    src/Phase5.cpp
    src/EngineHost.cpp src/EngineSwap.cpp)
target_include_directories(plugin_phase5_test PRIVATE
    src
    vendor/r8brain-free-src)
target_compile_definitions(plugin_phase5_test PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    ELF_NO_EDITOR=1)
target_link_libraries(plugin_phase5_test PRIVATE
    ${LXR_ENGINE_LINK}
    juce::juce_audio_basics
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_core
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events)

# Phase 4 (5) gate harness: two processors, interleaved blocks (see
# tools/instance-gate.sh). Same engine + processor sources as the plugin.
juce_add_console_app(plugin_instance_test PRODUCT_NAME "ElfDrumLXR Instance Test")
target_sources(plugin_instance_test PRIVATE
    render/PluginInstanceTest.cpp
    src/PluginProcessor.cpp
    src/EngineHost.cpp src/EngineSwap.cpp)
target_include_directories(plugin_instance_test PRIVATE
    src
    vendor/r8brain-free-src)
target_compile_definitions(plugin_instance_test PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    ELF_NO_EDITOR=1)
target_link_libraries(plugin_instance_test PRIVATE
    ${LXR_ENGINE_LINK}
    juce::juce_audio_basics
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_core
    juce::juce_data_structures
    juce::juce_events)
