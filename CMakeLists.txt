cmake_minimum_required(VERSION 3.16)

project(dos-gb-audio-dbg VERSION 0.1.0 LANGUAGES CXX C)

# Stage 1: Foundation & Tooling Setup.
# Standalone Dear ImGui + SDL2 + OpenGL3 desktop application.
# See docs/current_plan_debug_frontend.md.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Dependencies -----------------------------------------------------------

find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

# ALSA / audio device detection.
# SDL2 audio is the primary output path; a system ALSA is optional and only
# recorded here so later stages (mixer, multitrack recorder) can query it.
find_package(ALSA QUIET)
if(ALSA_FOUND)
  message(STATUS "ALSA found: ${ALSA_LIBRARIES}")
else()
  message(STATUS "ALSA not found - SDL2 audio output only")
endif()

# Stage 4.3/4.4: MIDI synthesizer backends (Munt MT-32 + FluidSynth GM).
# Both are REQUIRED: Mt32Device/GmDevice link them directly, with
# mock/fallback synthesis when ROMs/SoundFonts are absent at runtime.
find_package(PkgConfig REQUIRED)
pkg_check_modules(MT32EMU IMPORTED_TARGET REQUIRED mt32emu)
pkg_check_modules(FLUIDSYNTH IMPORTED_TARGET REQUIRED fluidsynth)

# --- Dear ImGui (v1.91.9b) & ImPlot (v0.16) -----------------------------------
# Third-party libraries fetched via CMake FetchContent (or local third_party/ if present).
if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()
include(FetchContent)

set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")
set(IMPLOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/implot")

if(NOT EXISTS "${IMGUI_DIR}/imgui.cpp")
  FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.9b
  )
  FetchContent_GetProperties(imgui)
  if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
    set(IMGUI_DIR "${imgui_SOURCE_DIR}")
  endif()
endif()

if(NOT EXISTS "${IMPLOT_DIR}/implot.cpp")
  FetchContent_Declare(
    implot
    GIT_REPOSITORY https://github.com/epezent/implot.git
    GIT_TAG        v0.16
  )
  FetchContent_GetProperties(implot)
  if(NOT implot_POPULATED)
    FetchContent_Populate(implot)
    set(IMPLOT_DIR "${implot_SOURCE_DIR}")
  endif()
endif()

add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
  ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
  ${IMGUI_DIR}
  ${IMGUI_DIR}/backends
)
target_link_libraries(imgui PUBLIC SDL2::SDL2 OpenGL::GL)
# Third-party code builds warning-free by construction; silence warnings so a
# host -Wall default cannot fail the build on third-party sources.
target_compile_options(imgui PRIVATE -w)
set_target_properties(imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- ImPlot (v0.16) -----------------------------------------------------------

add_library(implot STATIC
  ${IMPLOT_DIR}/implot.cpp
  ${IMPLOT_DIR}/implot_items.cpp
)
target_include_directories(implot PUBLIC
  ${IMPLOT_DIR}
  ${IMGUI_DIR}
)
target_link_libraries(implot PUBLIC imgui)
target_compile_options(implot PRIVATE -w)
set_target_properties(implot PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- Stage 4.1/4.2: Synthesis cores -----------------------------------------
# NukedOPL3 + Shay Green's Gb_Snd_Emu.
# Supports local path overrides (-DNUKEDOPL_DIR, -DGB_APU_DIR), local repo detection, or FetchContent.

set(NUKEDOPL_DIR "" CACHE PATH "Directory containing nukedopl.cpp")
set(GB_APU_DIR "" CACHE PATH "Directory containing Gb_Snd_Emu (Basic_Gb_Apu.cpp)")

if(NOT NUKEDOPL_DIR)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nukedopl/nukedopl.cpp")
    set(NUKEDOPL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nukedopl")
  elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../../dos_port/tools/dosbox-x/src/hardware/nukedopl.cpp")
    set(NUKEDOPL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../dos_port/tools/dosbox-x/src/hardware")
  else()
    FetchContent_Declare(
      nukedopl_fast
      GIT_REPOSITORY https://github.com/tgies/Nuked-OPL3-fast.git
      GIT_TAG        master
    )
    FetchContent_GetProperties(nukedopl_fast)
    if(NOT nukedopl_fast_POPULATED)
      FetchContent_Populate(nukedopl_fast)
      set(NUKEDOPL_DIR "${nukedopl_fast_SOURCE_DIR}")
    endif()
  endif()
endif()

if(NOT GB_APU_DIR)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/gb_snd_emu/Basic_Gb_Apu.cpp")
    set(GB_APU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/gb_snd_emu")
  elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../../dos_port/tools/audio/.gb_snd_emu/Basic_Gb_Apu.cpp")
    set(GB_APU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../dos_port/tools/audio/.gb_snd_emu")
  else()
    FetchContent_Declare(
      gb_snd_emu
      GIT_REPOSITORY https://github.com/blarggs-audio-libraries/Gb_Snd_Emu.git
      GIT_TAG        master
    )
    FetchContent_GetProperties(gb_snd_emu)
    if(NOT gb_snd_emu_POPULATED)
      FetchContent_Populate(gb_snd_emu)
      set(GB_APU_DIR "${gb_snd_emu_SOURCE_DIR}")
    endif()
  endif()
endif()

set(NUKEDOPL_SRC "${NUKEDOPL_DIR}/nukedopl.cpp")
set(NUKEDOPL_INC "${NUKEDOPL_DIR}")

if(NOT EXISTS "${NUKEDOPL_SRC}")
  message(FATAL_ERROR "NukedOPL3 source not found in ${NUKEDOPL_DIR}")
endif()
if(NOT EXISTS "${GB_APU_DIR}/Basic_Gb_Apu.cpp")
  message(FATAL_ERROR "Gb_Snd_Emu source not found in ${GB_APU_DIR}")
endif()

add_library(nukedopl STATIC ${NUKEDOPL_SRC})
target_include_directories(nukedopl SYSTEM PUBLIC ${NUKEDOPL_INC})
target_compile_options(nukedopl PRIVATE -w)
set_target_properties(nukedopl PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(gbapu STATIC
  ${GB_APU_DIR}/Basic_Gb_Apu.cpp
  ${GB_APU_DIR}/gb_apu/Gb_Apu.cpp
  ${GB_APU_DIR}/gb_apu/Gb_Oscs.cpp
  ${GB_APU_DIR}/gb_apu/Blip_Buffer.cpp
  ${GB_APU_DIR}/gb_apu/Multi_Buffer.cpp
)
target_include_directories(gbapu SYSTEM PUBLIC ${GB_APU_DIR})
target_compile_options(gbapu PRIVATE -w)
set_target_properties(gbapu PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Stage 4.1-4.4/4.6: Concrete synthesizer backends.
set(PKMN_AUDIO_DBG_SYNTH_SOURCES
  src/devices/opl3_device.cpp
  src/devices/gb_apu_device.cpp
  src/devices/mt32_device.cpp
  src/devices/gm_device.cpp
  src/devices/imfc_synth_core.cpp
  src/devices/imfc_device.cpp
)

# --- Application --------------------------------------------------------------

# Stage 2.2-2.4: Tier-2 intermediate device/tab bases.
# Stage 7.1/7.2/7.5: concrete MIDI tabs (Mt32Tab, GmTab).
# Stage 7.3/7.4/7.6: concrete synth tabs (Opl3Tab, GbApuTab) + TrackerView.
set(PKMN_AUDIO_DBG_DEVICE_SOURCES
  src/devices/midi_device.cpp
  src/devices/fm_device.cpp
  src/devices/psg_device.cpp
  src/tabs/midi_tab.cpp
  src/tabs/fm_tab.cpp
  src/tabs/psg_tab.cpp
  src/tabs/mt32_tab.cpp
  src/tabs/gm_tab.cpp
  src/tabs/imfc_tab.cpp
  src/tabs/opl3_tab.cpp
  src/tabs/gb_apu_tab.cpp
  src/tabs/tracker_view.cpp
)

# Stage 3.1-3.4: Audio mixer + multitrack WAV recorder + AudioLog.
set(PKMN_AUDIO_DBG_AUDIO_SOURCES
  src/audio_mixer.cpp
  src/recorder.cpp
)

# Stage 5.1-5.3: Song catalog + session/playback engine + YAML watcher.
set(PKMN_AUDIO_DBG_SESSION_SOURCES
  src/song_catalog.cpp
  src/session_engine.cpp
  src/enhancement_manager.cpp
)

# Stage 6.1-6.4: Transport bar (playback controls, seek scrubber, loop/speed,
# enhancement + slot toggles, keyboard shortcuts). See
# docs/current_plan_debug_frontend.md §6.
set(PKMN_AUDIO_DBG_TRANSPORT_SOURCES
  src/transport_bar.cpp
)

# Stage 8.1/8.2: Headless CLI batch renderer + .audiolog replayer
# (arg parsing + device-native-rate WAV pipeline, no SDL/GL/ImGui).
# See docs/current_plan_debug_frontend.md §8.
# Plus the multi-project configuration + path hierarchy engine (config.h).
set(PKMN_AUDIO_DBG_CLI_SOURCES
  src/cli.cpp
  src/config.cpp
  src/headless.cpp
)

add_executable(dos-gb-audio-dbg
  src/main.cpp
  ${PKMN_AUDIO_DBG_DEVICE_SOURCES}
  ${PKMN_AUDIO_DBG_SYNTH_SOURCES}
  ${PKMN_AUDIO_DBG_AUDIO_SOURCES}
  ${PKMN_AUDIO_DBG_SESSION_SOURCES}
  ${PKMN_AUDIO_DBG_TRANSPORT_SOURCES}
  ${PKMN_AUDIO_DBG_CLI_SOURCES}
)
# Stage 2.1: src/ on the include path so tabs/devices use
# "devices/sound_device.h" / "tabs/device_tab.h" spelling.
# Third-party synth cores enter as SYSTEM includes so -Wall -Wextra stays
# clean on our code (their headers carry legacy -Wunused-value patterns).
target_include_directories(dos-gb-audio-dbg PRIVATE src)
target_include_directories(dos-gb-audio-dbg SYSTEM PRIVATE
  ${NUKEDOPL_INC}
  ${GB_APU_DIR}
)
target_link_libraries(dos-gb-audio-dbg PRIVATE
  imgui
  implot
  nukedopl
  gbapu
  PkgConfig::MT32EMU
  PkgConfig::FLUIDSYNTH
  SDL2::SDL2
  OpenGL::GL
  Threads::Threads
  ${CMAKE_DL_LIBS}
)
if(ALSA_FOUND)
  target_link_libraries(dos-gb-audio-dbg PRIVATE ${ALSA_LIBRARIES})
  target_include_directories(dos-gb-audio-dbg PRIVATE ${ALSA_INCLUDE_DIRS})
  target_compile_definitions(dos-gb-audio-dbg PRIVATE HAVE_ALSA=1)
endif()

target_compile_options(dos-gb-audio-dbg PRIVATE -Wall -Wextra)

# --- Stage 2.6: Acceptance test ------------------------------------------------
# Mock-based check for the Tier-2 intermediate bases: polymorphic dispatch,
# pre-synth MIDI muting, FM voice configuration, PSG duty/wave state, and
# lazy dormant skipping. Runs headless (no GUI, no audio hardware).

add_executable(pkmn-audio-dbg-tests
  tests/test_intermediate.cpp
  ${PKMN_AUDIO_DBG_DEVICE_SOURCES}
  ${PKMN_AUDIO_DBG_SYNTH_SOURCES}
)
target_include_directories(pkmn-audio-dbg-tests PRIVATE
  src
  ${IMGUI_DIR}
)
target_include_directories(pkmn-audio-dbg-tests SYSTEM PRIVATE
  ${NUKEDOPL_INC}
  ${GB_APU_DIR}
)
target_link_libraries(pkmn-audio-dbg-tests PRIVATE
  imgui
  nukedopl
  gbapu
  PkgConfig::MT32EMU
  PkgConfig::FLUIDSYNTH
)
target_compile_options(pkmn-audio-dbg-tests PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME intermediate_bases COMMAND pkmn-audio-dbg-tests)

# --- Stage 3.5: Acceptance test -------------------------------------------------
# Headless checks for the Stage 3 audio mixer + recorder: WAV header/file
# writing, multitrack stem null-sum, AudioLog round-trip, mixer atomics +
# render (incl. 44.1k -> 48k resample), and resampleLinear unit checks.
# No GUI, no audio hardware (mixer runs disabled/headless).

add_executable(pkmn-audio-dbg-audio-tests
  tests/test_audio.cpp
  ${PKMN_AUDIO_DBG_AUDIO_SOURCES}
)
target_include_directories(pkmn-audio-dbg-audio-tests PRIVATE
  src
  ${IMGUI_DIR}
)
target_link_libraries(pkmn-audio-dbg-audio-tests PRIVATE
  SDL2::SDL2
  Threads::Threads
)
target_compile_options(pkmn-audio-dbg-audio-tests PRIVATE -Wall -Wextra)

add_test(NAME audio_mixer_recorder COMMAND pkmn-audio-dbg-audio-tests)

# --- Stage 4.5: Acceptance test -------------------------------------------------
# Headless checks for the Stage 4 concrete synth backends: OPL3 register
# writes/rendering (incl. operator decode + shadow isolation) and GB APU
# register writes/rendering (incl. duty/wave/LFSR decode), with dormant
# voice skipping verification on both. No GUI, no audio hardware.

add_executable(pkmn-audio-dbg-synth-tests
  tests/test_synths.cpp
  ${PKMN_AUDIO_DBG_DEVICE_SOURCES}
  ${PKMN_AUDIO_DBG_SYNTH_SOURCES}
)
target_include_directories(pkmn-audio-dbg-synth-tests PRIVATE
  src
  ${IMGUI_DIR}
)
target_include_directories(pkmn-audio-dbg-synth-tests SYSTEM PRIVATE
  ${NUKEDOPL_INC}
  ${GB_APU_DIR}
)
target_link_libraries(pkmn-audio-dbg-synth-tests PRIVATE
  imgui
  nukedopl
  gbapu
  PkgConfig::MT32EMU
  PkgConfig::FLUIDSYNTH
)
target_compile_options(pkmn-audio-dbg-synth-tests PRIVATE -Wall -Wextra)

add_test(NAME synth_backends COMMAND pkmn-audio-dbg-synth-tests)

# --- Stage 4.5 (Part 4B): Acceptance test --------------------------------------
# Headless checks for the Stage 4.3/4.4 MIDI backends: MT-32 ROM resolution
# (or mock fallback), note dispatch, native partial/part telemetry, LCD
# readout, GM SoundFont loading (or mock fallback), GM patch resolution,
# master + per-channel rendering with dormant fast escape and mute gating
# on both. No GUI, no audio hardware.

add_executable(pkmn-audio-dbg-midi-tests
  tests/test_midi_devices.cpp
  src/devices/midi_device.cpp
  src/devices/mt32_device.cpp
  src/devices/gm_device.cpp
)
target_include_directories(pkmn-audio-dbg-midi-tests PRIVATE
  src
  ${IMGUI_DIR}
)
target_link_libraries(pkmn-audio-dbg-midi-tests PRIVATE
  PkgConfig::MT32EMU
  PkgConfig::FLUIDSYNTH
)
target_compile_options(pkmn-audio-dbg-midi-tests PRIVATE -Wall -Wextra)

add_test(NAME midi_backends COMMAND pkmn-audio-dbg-midi-tests)

# --- Stage 4.6: Acceptance test ------------------------------------------------
# Headless checks for the IMFC (Yamaha FB-01) backend and tab:
# init/reset, note dispatch, FB-01 SysEx voice loading, ROM presets,
# master + per-channel rendering, dormant fast escape and mute gating.
# No GUI, no audio hardware.

add_executable(pkmn-audio-dbg-imfc-tests
  tests/test_imfc_device.cpp
  src/devices/midi_device.cpp
  src/devices/imfc_synth_core.cpp
  src/devices/imfc_device.cpp
  src/tabs/midi_tab.cpp
  src/tabs/imfc_tab.cpp
)
target_include_directories(pkmn-audio-dbg-imfc-tests PRIVATE
  src
  ${IMGUI_DIR}
)
target_link_libraries(pkmn-audio-dbg-imfc-tests PRIVATE
  imgui
)
target_compile_options(pkmn-audio-dbg-imfc-tests PRIVATE -Wall -Wextra)

add_test(NAME imfc_device COMMAND pkmn-audio-dbg-imfc-tests)


# --- Stage 5.5 (Part 5A): Acceptance test --------------------------------------
# Headless checks for the Stage 5.1/5.2 song catalog + session engine: real
# repo parsing (> 50 tracks, fuzzy resolution of pallet/gym/route1/title),
# frame stepping, play/pause/seek/stop, loop wrap-around, mock SoundDevice
# event dispatch, MidiDevice note translation, end-of-track auto-stop.
# No GUI, no audio hardware.
#
# --- Stage 5.5 (Part 5B): additions -------------------------------------------
# Real Yellow note loading (Music_PalletTown mt32 baseline + Routes1 gm
# spot check, incl. a full play-through dispatch count), the compiled
# Music_PalletTown YAML enhancement (mt32 + gm targets), EnhancementManager
# revisions (snapshot/dedup/list/content/revert) plus mtime polling in a
# temp dir, and position-locked A/B switching (slot swap, enhancement
# toggle, device swap: frame preserved, outgoing MidiDevice silenced,
# incoming stream continues at current_frame).

add_executable(pkmn-audio-dbg-session-tests
  tests/test_session.cpp
  ${PKMN_AUDIO_DBG_SESSION_SOURCES}
  src/devices/midi_device.cpp
)
target_include_directories(pkmn-audio-dbg-session-tests PRIVATE
  src
  ${IMGUI_DIR}
)
# Absolute repo root so the catalog test finds constants/ + audio/headers/
# regardless of the CWD ctest runs from.
get_filename_component(DEFAULT_PKMN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(PKMN_REPO_ROOT_ABS "${DEFAULT_PKMN_ROOT}" CACHE PATH "Path to Pokemon Yellow repository root for test data")
if(NOT PKMN_REPO_ROOT_ABS OR NOT EXISTS "${PKMN_REPO_ROOT_ABS}/constants")
  if(EXISTS "${DEFAULT_PKMN_ROOT}/constants")
    set(PKMN_REPO_ROOT_ABS "${DEFAULT_PKMN_ROOT}" CACHE PATH "Path to Pokemon Yellow repository root for test data" FORCE)
  endif()
endif()
target_compile_definitions(pkmn-audio-dbg-session-tests PRIVATE
  PKMN_TEST_REPO_ROOT="${PKMN_REPO_ROOT_ABS}"
)
target_compile_options(pkmn-audio-dbg-session-tests PRIVATE -Wall -Wextra)

add_test(NAME session_engine COMMAND pkmn-audio-dbg-session-tests)

# --- Stage 6.5: Acceptance test -------------------------------------------------
# Headless checks for the Stage 6 transport bar: MM:SS.f formatting, seek
# scrubber clamping + MIDI-silenced resync, speed-multiplier pacing (clock
# divider, no paused debt), loop marker staging/wrap/stash, and keyboard
# shortcuts (Space/Home/Left/Right/E/Tab/[/] incl. a temp-dir revision walk).
# render() is never called: no GUI, no audio hardware.

add_executable(pkmn-audio-dbg-transport-tests
  tests/test_transport.cpp
  src/transport_bar.cpp
  src/session_engine.cpp
  src/enhancement_manager.cpp
  src/devices/midi_device.cpp
)
target_include_directories(pkmn-audio-dbg-transport-tests PRIVATE
  src
  ${IMGUI_DIR}
)
target_link_libraries(pkmn-audio-dbg-transport-tests PRIVATE imgui)
target_compile_options(pkmn-audio-dbg-transport-tests PRIVATE -Wall -Wextra)

add_test(NAME transport_bar COMMAND pkmn-audio-dbg-transport-tests)

# --- Stage 7.1/7.2/7.5: Acceptance test ---------------------------------------
# Headless checks for the concrete MIDI tabs: Mt32Tab 9-part strip layout +
# factory channel mapping, timbre programName, partial-state 4x8 matrix
# colours/bounds/count, LCD readout, part/channel mute toggling; GmTab
# 16-channel strip layout + drum marker, GM programName, mute/solo
# toggling, CC7/CC10 volume/pan; dormant escape + null-safety in
# pushWaveformData and headless draw safety on both. No GUI, no hardware.

add_executable(pkmn-audio-dbg-tab-tests
  tests/test_midi_tabs.cpp
  src/devices/midi_device.cpp
  src/devices/mt32_device.cpp
  src/devices/gm_device.cpp
  src/tabs/midi_tab.cpp
  src/tabs/mt32_tab.cpp
  src/tabs/gm_tab.cpp
)
target_include_directories(pkmn-audio-dbg-tab-tests PRIVATE
  src
  ${IMGUI_DIR}
)
target_link_libraries(pkmn-audio-dbg-tab-tests PRIVATE
  imgui
  PkgConfig::MT32EMU
  PkgConfig::FLUIDSYNTH
)
target_compile_options(pkmn-audio-dbg-tab-tests PRIVATE -Wall -Wextra)

add_test(NAME midi_tabs COMMAND pkmn-audio-dbg-tab-tests)

# --- Stage 7.3/7.4/7.6/7.7: Acceptance test ------------------------------------
# Headless checks for the synth tabs + tracker: Opl3Tab 18-voice labels,
# operator params, zero-crossing stabilization; GbApuTab duty fractions,
# wave RAM decoding, LFSR mode, envelope names; TrackerView note naming,
# divergence logic/colours, SimState/event comparison; dormant escape +
# null-safety and headless draw safety on all three. No GUI, no hardware.

add_executable(pkmn-audio-dbg-synth-tab-tests
  tests/test_synth_tabs.cpp
  src/devices/midi_device.cpp
  src/devices/fm_device.cpp
  src/devices/psg_device.cpp
  src/devices/opl3_device.cpp
  src/devices/gb_apu_device.cpp
  src/tabs/fm_tab.cpp
  src/tabs/psg_tab.cpp
  src/tabs/opl3_tab.cpp
  src/tabs/gb_apu_tab.cpp
  src/tabs/tracker_view.cpp
  src/session_engine.cpp
)
target_include_directories(pkmn-audio-dbg-synth-tab-tests PRIVATE
  src
  ${IMGUI_DIR}
)
target_include_directories(pkmn-audio-dbg-synth-tab-tests SYSTEM PRIVATE
  ${NUKEDOPL_INC}
  ${GB_APU_DIR}
)
target_link_libraries(pkmn-audio-dbg-synth-tab-tests PRIVATE
  imgui
  nukedopl
  gbapu
)
target_compile_options(pkmn-audio-dbg-synth-tab-tests PRIVATE -Wall -Wextra)

add_test(NAME synth_tabs COMMAND pkmn-audio-dbg-synth-tab-tests)

# --- Stage 8.3: Acceptance test ------------------------------------------------
# Headless checks for the Stage 8 CLI + replay + verification: flag parsing
# (--headless/--track/--device/--frames/--out/--replay/--no-enh/--enhancement),
# device normalization, frame/sample math, a real Music_PalletTown mt32
# headless render (WAV header, exact sample count, non-zero content,
# byte-identical across runs), and audiolog record/replay parity (replay ==
# direct SessionEngine render, null difference). No GUI, no audio hardware
# (MT32_ROM_DIR=none + SOUNDFONT=none force the deterministic mock synths).

add_executable(pkmn-audio-dbg-cli-tests
  tests/test_cli.cpp
  src/cli.cpp
  src/config.cpp
  src/headless.cpp
  src/song_catalog.cpp
  src/session_engine.cpp
  src/enhancement_manager.cpp
  src/recorder.cpp
  src/devices/midi_device.cpp
  src/devices/fm_device.cpp
  src/devices/psg_device.cpp
  src/devices/mt32_device.cpp
  src/devices/gm_device.cpp
  src/devices/opl3_device.cpp
  src/devices/gb_apu_device.cpp
  src/devices/imfc_synth_core.cpp
  src/devices/imfc_device.cpp
)
target_include_directories(pkmn-audio-dbg-cli-tests PRIVATE
  src
  ${IMGUI_DIR}
)
target_include_directories(pkmn-audio-dbg-cli-tests SYSTEM PRIVATE
  ${NUKEDOPL_INC}
  ${GB_APU_DIR}
)
target_link_libraries(pkmn-audio-dbg-cli-tests PRIVATE
  nukedopl
  gbapu
  PkgConfig::MT32EMU
  PkgConfig::FLUIDSYNTH
)
# Absolute repo root so the catalog + enhancement manager find constants/ +
# audio/headers/ + assets/midi/ regardless of the CWD ctest runs from.
target_compile_definitions(pkmn-audio-dbg-cli-tests PRIVATE
  PKMN_TEST_REPO_ROOT="${PKMN_REPO_ROOT_ABS}"
)
target_compile_options(pkmn-audio-dbg-cli-tests PRIVATE -Wall -Wextra)

add_test(NAME cli_verification COMMAND pkmn-audio-dbg-cli-tests)

# --- Config engine: Acceptance test -------------------------------------------
# Headless checks for the multi-project configuration + path hierarchy:
# default creation, save/load round-trip, multi-project resolution
# (relative join, absolute passthru, root override), CLI explicit override
# priority, PKMN_REPO_ROOT/CWD fallback, and --project/--project-dir/
# --overrides/--enhancements/--constants parsing. No GUI, no hardware.

add_executable(pkmn-audio-dbg-config-tests
  tests/test_config.cpp
  src/cli.cpp
  src/config.cpp
)
target_include_directories(pkmn-audio-dbg-config-tests PRIVATE
  src
  ${IMGUI_DIR}
)
target_compile_options(pkmn-audio-dbg-config-tests PRIVATE -Wall -Wextra)

add_test(NAME config_engine COMMAND pkmn-audio-dbg-config-tests)

# Compatibility alias
add_executable(pkmn-audio-dbg ALIAS dos-gb-audio-dbg)

install(TARGETS dos-gb-audio-dbg RUNTIME DESTINATION bin)

# Shorthand symlink dgad -> dos-gb-audio-dbg
install(CODE "
  set(DEST_BIN \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin\")
  execute_process(COMMAND \${CMAKE_COMMAND} -E create_symlink dos-gb-audio-dbg \${DEST_BIN}/dgad)
")

# Desktop file and icon
install(FILES assets/dos-gb-audio-dbg.desktop DESTINATION share/applications)
install(FILES assets/icons/dos-gb-audio-dbg.svg DESTINATION share/icons/hicolor/scalable/apps)
