// Stage 1: Application entry point for pkmn-audio-dbg.
// SDL2 window + OpenGL3 context + Dear ImGui/ImPlot placeholder UI.
// See docs/current_plan_debug_frontend.md (Stage 1.3).
//
// Stage 7.7: wires the four concrete device tabs (Opl3Tab, GbApuTab,
// Mt32Tab, GmTab) plus the Schism-style TrackerView into the device tab
// bar. Selecting a tab routes the SessionEngine's event stream and the
// AudioMixer's PCM pull to that device; a per-frame scope feed pushes
// live per-channel audio into the active tab so carrier/channel
// oscilloscopes trace interactively.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include <SDL.h>
#include <SDL_opengl.h>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"

// Stage 2.1 + 2.5: Tier-1 universal bases (compile-checked here; concrete
// devices/tabs in later stages subclass these).
#include "devices/sound_device.h"
#include "tabs/device_tab.h"
// Stage 2.2-2.4: Tier-2 intermediate bases (compile-checked here; concrete
// MT-32/GM/OPL3/GB-APU backends in Stages 4/7 subclass these).
#include "devices/fm_device.h"
#include "devices/midi_device.h"
#include "devices/psg_device.h"
#include "tabs/fm_tab.h"
#include "tabs/midi_tab.h"
#include "tabs/psg_tab.h"
// Stage 4: concrete synthesizer backends.
#include "devices/gb_apu_device.h"
#include "devices/gm_device.h"
#include "devices/mt32_device.h"
#include "devices/opl3_device.h"
// Stage 5: session engine, song catalog, enhancement watcher.
// Stage 6: transport bar (playback controls, seek, loop, speed, shortcuts).
// Stage 8: headless CLI batch renderer + .audiolog replayer.
#include "audio_mixer.h"
#include "cli.h"
#include "config.h"
#include "enhancement_manager.h"
#include "headless.h"
#include "session_engine.h"
#include "song_catalog.h"
#include "transport_bar.h"
// Stage 7: concrete device tabs + Schism-style tracker view.
#include "tabs/gb_apu_tab.h"
#include "tabs/gm_tab.h"
#include "tabs/mt32_tab.h"
#include "tabs/opl3_tab.h"
#include "tabs/tracker_view.h"

namespace {

constexpr char kWindowTitle[] = "DOS GB Audio Debugger (dgad)";
constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 720;
// 60 Hz frame pacing: ~16.6 ms per frame.
constexpr Uint32 kFrameBudgetMs = 16;

constexpr const char* kDeviceTabs[] = {"OPL3", "GB-APU", "MT-32",
                                       "General MIDI"};

// Stage 7.7: builds the driver-simulation view for the tracker from the
// engine's dispatched stream: note-ons stamped at the current frame become
// active SimNotes. Chip state comes from the device snapshot at draw time.
audio_dbg::SimState buildSimState(const audio_dbg::SessionEngine& engine) {
  audio_dbg::SimState sim;
  const std::uint32_t cur = engine.currentFrame();
  sim.driver_frame = cur;
  for (const audio_dbg::SimNoteEvent& e : engine.events()) {
    if (e.frame > cur) break;
    if (e.is_note_on) {
      const std::uint32_t dur = e.duration_frames > 0 ? e.duration_frames : 1;
      if (cur >= e.frame && cur < e.frame + dur) {
        audio_dbg::SimNote n;
        n.channel = e.channel;
        n.note = e.note;
        n.velocity = e.velocity;
        n.active = true;
        n.start_frame = e.frame;
        n.length_frames = e.duration_frames;
        bool found = false;
        for (auto& existing : sim.notes) {
          if (existing.channel == n.channel) {
            existing = n;
            found = true;
            break;
          }
        }
        if (!found) {
          sim.notes.push_back(n);
        }
      }
    } else {
      for (auto it = sim.notes.begin(); it != sim.notes.end();) {
        if (it->channel == e.channel && it->note == e.note &&
            it->start_frame <= e.frame) {
          it = sim.notes.erase(it);
        } else {
          ++it;
        }
      }
    }
  }
  return sim;
}

// Stage 6.5: loads the selected track's GB baseline into the engine (silent
// no-op when the repo/midi file is unavailable).
// Stops the engine, replaces the event stream, sizes total_frames past the
// last event, arms the YAML watcher, and applies the compiled enhancement
// layer when the python bridge resolves one.
// Also performs the stateful MT-32 custom-timbre handover: previous track's
// cleanup then new track's setup (enhancement_sysex.py), before any Program
// Change is dispatched. `mt32` may be null (no synth handover).
void loadTrackBaseline(audio_dbg::SessionEngine& engine,
                       audio_dbg::EnhancementManager& enh_mgr,
                       audio_dbg::SongCatalog& catalog, const char* constant,
                       const char* target = "mt32",
                       audio_dbg::Mt32Device* mt32 = nullptr) {
  const audio_dbg::SongInfo* info = catalog.findTrack(constant);
  if (info == nullptr) return;
  // Custom timbres are stateful across track loads: send the PREVIOUS track's
  // factory-restore cleanup then THIS track's setup, both produced by the
  // Python bridge (enhancement_sysex.py -> midi_to_stream.build_song_sysex).
  // Runs before the baseline check so a no-custom-timbre track still clears
  // the previous track's Patch Memory rewrites. This happens before the
  // Program Change stream plays -- MUNT latches Patch Memory at PC time.
  if (mt32 != nullptr) {
    audio_dbg::SongTimbreSysex sx =
        enh_mgr.loadSongTimbreSysex(info->header_label);
    mt32->applySongTimbres(info->header_label, sx.setup, sx.cleanup);
  }
  audio_dbg::MidiFileData midi =
      enh_mgr.parseMidiFile(enh_mgr.midiPathFor(info->header_label, target));
  std::vector<audio_dbg::SimNoteEvent> base = std::move(midi.notes);
  if (base.empty()) return;
  // Embedded SysEx retained by the SMF parser reaches the synth too, before
  // setEvents()/syncDeviceState() replays the frame-0 Program Changes that
  // latch Patch Memory.
  if (mt32 != nullptr && !midi.sysex.empty()) {
    mt32->sendSysExMessages(midi.sysex);
  }
  std::uint32_t max_frame = 0;
  for (const audio_dbg::SimNoteEvent& e : base) {
    max_frame = std::max(max_frame, e.frame);
  }
  engine.stop();
  engine.setEvents(base);
  engine.setTotalFrames(max_frame + 1);
  engine.setLoop(0, 0);
  enh_mgr.watchSong(info->header_label);
  std::vector<audio_dbg::SimNoteEvent> enh =
      enh_mgr.compileEnhancement(info->header_label, target);
  engine.setEnhancementEvents(std::move(enh));
}

// --- Audio-clock tick shim (transport clock rework) -------------------------
// The session engine's 60 Hz ticks are driven by the AUDIO callback, not by
// the UI frame loop. `AudioMixer`'s device pointer is this thin forwarder:
// render() splits each callback request into chunks bounded by the next tick
// deadline, renders each chunk through the real device, and dispatches the
// due tick(s) between chunks. Every tick therefore has a non-zero audio
// render interval around it -- the condition that stops MUNT silently
// dropping a zero-delta NoteOn+NoteOff -- and the tempo is a function of
// rendered samples alone: one tick per (device_rate / 60) samples at 1.0x,
// divided by the transport speed multiplier. The UI no longer owns the clock.
//
// SDL_GetQueuedAudioSize is unusable here: AudioMixer opens a CALLBACK
// device, not an SDL_QueueAudio device, so the queued-byte accounting is
// always 0. Counting at the device render boundary IS "audio-callback
// rendered-sample accounting" and needs no AudioMixer edit.
class AudioClockShim : public audio_dbg::SoundDevice {
 public:
  AudioClockShim() : SoundDevice(-1, "transport-clock", 0) {}

  void setEngine(audio_dbg::SessionEngine* engine) { engine_ = engine; }
  // Swaps the wrapped device + its native render rate (mixer lock held).
  void setInner(audio_dbg::SoundDevice* dev, int rate) {
    inner_ = dev;
    clock_.setSampleRate(rate);
  }
  void setSpeed(double speed) { clock_.setSpeed(speed); }

  // Instrumentation (DGAD_CLOCK_LOG=1) proving ticks are paced by renders:
  // a single render-sized advance() must never dispatch more than one tick,
  // and a burst flag records any >1 step (the zero-render burst defect).
  std::uint64_t ticksTotal() const { return ticks_total_; }
  std::uint64_t renderChunks() const { return chunks_total_; }
  int maxTicksPerChunk() const { return max_ticks_chunk_; }
  bool sawZeroRenderBurst() const { return zero_render_burst_; }
  std::uint64_t renderedSamples() const { return clock_.renderedSamples(); }
  void resetStats() {
    ticks_total_ = 0;
    chunks_total_ = 0;
    max_ticks_chunk_ = 0;
    zero_render_burst_ = false;
  }

  // --- SoundDevice forwarding (the mixer renders through this shim) --------
  bool init() override { return inner_ != nullptr ? inner_->init() : true; }
  void shutdown() override {
    if (inner_ != nullptr) inner_->shutdown();
  }
  void reset() override {
    if (inner_ != nullptr) inner_->reset();
  }

  void render(float* buf, std::size_t frames) override {
    std::size_t done = 0;
    while (done < frames) {
      // Dispatch a deadline already reached before rendering; the previous
      // iteration rendered for it, so this is still render-interleaved.
      pump(0);
      std::size_t chunk = frames - done;
      const std::size_t until = clock_.samplesUntilNextTick();
      if (chunk > until) chunk = until;
      if (chunk == 0) chunk = frames - done;  // Defensive: never stall.
      if (inner_ != nullptr) {
        inner_->render(buf + done, chunk);
      } else {
        std::fill(buf + done, buf + done + chunk, 0.0f);
      }
      done += chunk;
      pump(chunk);
    }
    pump(0);
  }

  void renderPerChannel(float** bufs, std::size_t frames) override {
    // Stems/scope path (not the live mixer callback): forward only. The
    // master render() above is what clocks the transport.
    if (inner_ != nullptr) inner_->renderPerChannel(bufs, frames);
  }
  void setMute(int ch, bool muted) override {
    if (inner_ != nullptr) inner_->setMute(ch, muted);
  }
  void setSolo(int ch, bool soloed) override {
    if (inner_ != nullptr) inner_->setSolo(ch, soloed);
  }
  void tick(std::uint32_t frame) override {
    if (inner_ != nullptr) inner_->tick(frame);
  }
  void handleCommand(std::uint8_t opcode, const std::uint8_t* payload,
                     std::size_t len) override {
    if (inner_ != nullptr) inner_->handleCommand(opcode, payload, len);
  }
  audio_dbg::DeviceSnapshot snapshot() const override {
    return inner_ != nullptr ? inner_->snapshot() : audio_dbg::DeviceSnapshot();
  }

 private:
  void pump(std::size_t frames) {
    if (engine_ == nullptr) return;
    const int n = clock_.advance(*engine_, frames);
    ticks_total_ += static_cast<std::uint64_t>(n);
    if (frames > 0) {
      ++chunks_total_;
      if (n > max_ticks_chunk_) max_ticks_chunk_ = n;
      if (n > 1) zero_render_burst_ = true;
    }
  }

  audio_dbg::SoundDevice* inner_ = nullptr;
  audio_dbg::SessionEngine* engine_ = nullptr;
  audio_dbg::AudioTickClock clock_;
  std::uint64_t ticks_total_ = 0;
  std::uint64_t chunks_total_ = 0;
  int max_ticks_chunk_ = 0;
  bool zero_render_burst_ = false;
};

// Stage 6.4: SDL key -> transport shortcut. Repeat events are excluded by
// the caller (holding Space must not strobe play/pause).
bool sdlToTransportKey(SDL_Keycode sym, audio_dbg::TransportKey* out) {
  using audio_dbg::TransportKey;
  if (out == nullptr) return false;
  switch (sym) {
    case SDLK_SPACE:
      *out = TransportKey::PlayPause;
      return true;
    case SDLK_HOME:
      *out = TransportKey::Rewind;
      return true;
    case SDLK_LEFT:
      *out = TransportKey::StepBack;
      return true;
    case SDLK_RIGHT:
      *out = TransportKey::StepForward;
      return true;
    case SDLK_e:
      *out = TransportKey::ToggleEnhancement;
      return true;
    case SDLK_LEFTBRACKET:
      *out = TransportKey::RevisionPrev;
      return true;
    case SDLK_RIGHTBRACKET:
      *out = TransportKey::RevisionNext;
      return true;
    case SDLK_TAB:
      *out = TransportKey::ToggleSlot;
      return true;
    default:
      return false;
  }
}

}  // namespace

int main(int argc, char** argv) {
  // --- Stage 8.1/8.2: headless CLI (no SDL / OpenGL / ImGui) ----------------
  // Flag parsing happens before any windowing: --help and --headless /
  // --replay never create a window or an audio device.
  audio_dbg::CliOptions cli_opt;
  std::string cli_err;
  if (!audio_dbg::parseCliArgs(argc, argv, &cli_opt, &cli_err)) {
    std::fprintf(stderr, "pkmn-audio-dbg: %s\n%s", cli_err.c_str(),
                 audio_dbg::cliUsage(argv != nullptr ? argv[0] : nullptr)
                     .c_str());
    return 1;
  }
  if (cli_opt.show_help) {
    std::printf(
        "%s", audio_dbg::cliUsage(argv != nullptr ? argv[0] : nullptr)
                  .c_str());
    return 0;
  }
  if (cli_opt.headless) {
    audio_dbg::HeadlessResult cli_res;
    const int rc = audio_dbg::runHeadless(cli_opt, &cli_err, &cli_res);
    if (rc != 0) {
      std::fprintf(stderr, "pkmn-audio-dbg: %s\n", cli_err.c_str());
    }
    return rc;
  }

  // --- SDL2: Video + Audio --------------------------------------------------
  SDL_setenv("SDL_AUDIODRIVER", "pipewire", 1);
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
    SDL_setenv("SDL_AUDIODRIVER", "", 1);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
      std::fprintf(stderr, "pkmn-audio-dbg: SDL_Init failed: %s\n", SDL_GetError());
      return 1;
    }
  }

  // Request an OpenGL 3.0 core context for the ImGui OpenGL3 backend.
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

  SDL_Window* window = SDL_CreateWindow(
      kWindowTitle, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, kWindowWidth,
      kWindowHeight, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
  if (window == nullptr) {
    std::fprintf(stderr, "pkmn-audio-dbg: SDL_CreateWindow failed: %s\n",
                 SDL_GetError());
    SDL_Quit();
    return 1;
  }

  SDL_GLContext gl_context = SDL_GL_CreateContext(window);
  if (gl_context == nullptr) {
    std::fprintf(stderr, "pkmn-audio-dbg: SDL_GL_CreateContext failed: %s\n",
                 SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  SDL_GL_MakeCurrent(window, gl_context);
  SDL_GL_SetSwapInterval(1);  // Enable vsync.

  // --- ImGui + ImPlot --------------------------------------------------------
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImPlot::CreateContext();
  ImGui::StyleColorsDark();

  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  // No persistent layout yet: Stage 5 (session engine) owns window/dock
  // persistence. Until then, do not write imgui.ini to the caller's CWD.
  io.IniFilename = nullptr;

  ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
  ImGui_ImplOpenGL3_Init("#version 130");

  // --- Session state (Stages 5+6) -----------------------------------------
  // The transport bar owns speed/loop-toggle UI state; the engine owns
  // frames, loop range, slots and the enhancement overlay.
  audio_dbg::SessionEngine engine;
  audio_dbg::TransportBar transport;
  AudioClockShim clock_shim;  // Audio-clock tick source for the transport.
  audio_dbg::SongCatalog catalog;
  audio_dbg::EnhancementManager enh_mgr;
  int last_loaded_track = -1;

  // --- Stage 7.7: concrete devices + tabs + mixer + tracker ----------------
  // Best-effort init: a backend that fails to start still gets its tab
  // (dormant strips) so one missing ROM/SoundFont never kills the UI.
  audio_dbg::Opl3Device opl3_dev;
  audio_dbg::GbApuDevice gbapu_dev;
  audio_dbg::Mt32Device mt32_dev;
  audio_dbg::GmDevice gm_dev;
  opl3_dev.init();
  gbapu_dev.init();
  mt32_dev.init();
  gm_dev.init();

  audio_dbg::Opl3Tab opl3_tab;
  audio_dbg::GbApuTab gbapu_tab;
  audio_dbg::Mt32Tab mt32_tab;
  audio_dbg::GmTab gm_tab;
  opl3_tab.setOpl3Device(&opl3_dev);
  gbapu_tab.setGbApuDevice(&gbapu_dev);
  mt32_tab.setMt32Device(&mt32_dev);
  gm_tab.setGmDevice(&gm_dev);

  audio_dbg::TrackerView tracker;

  audio_dbg::SoundDevice* devices[4] = {&opl3_dev, &gbapu_dev, &mt32_dev,
                                        &gm_dev};
  audio_dbg::DeviceTab* tabs[4] = {&opl3_tab, &gbapu_tab, &mt32_tab, &gm_tab};
  const int device_rates[4] = {
      static_cast<int>(opl3_dev.sampleRate()),
      static_cast<int>(gbapu_dev.sampleRate()),
      static_cast<int>(mt32_dev.sampleRate()),
      static_cast<int>(gm_dev.sampleRate()),
  };

  audio_dbg::AudioMixer mixer;
  if (!mixer.init()) {
    // Headless/CI boxes without an SDL audio device still run the UI;
    // rendering paths stay testable, just silent.
    mixer.init(audio_dbg::AudioMixer::kDefaultOutputRate,
               audio_dbg::AudioMixer::kDefaultBufferFrames,
               /*disabled=*/true);
  }

  // Backend selection: defaults to MT-32 or respects --device.
  int initial_tab = 2;
  if (cli_opt.device == "opl3") initial_tab = 0;
  else if (cli_opt.device == "gbapu") initial_tab = 1;
  else if (cli_opt.device == "gm") initial_tab = 3;

  int device_tab = initial_tab;
  int request_tab_switch = initial_tab;
  engine.setActiveDevice(devices[device_tab]);
  // The engine dispatches to the real device; the mixer renders through the
  // shim, whose render() is the audio clock the ticks are paced from.
  clock_shim.setEngine(&engine);
  clock_shim.setInner(devices[device_tab], device_rates[device_tab]);
  mixer.setDevice(&clock_shim, device_rates[device_tab]);

  // --- Multi-project configuration & track catalog -------------------------
  audio_dbg::ConfigManager config_mgr;
  config_mgr.load();
  if (!cli_opt.project.empty()) {
    config_mgr.setCurrentProject(cli_opt.project);
  }
  if (!cli_opt.project_dir.empty()) {
    config_mgr.setActiveProjectRoot(cli_opt.project_dir);
  }

  std::vector<std::string> track_names;
  int track_index = 0;

  auto reloadActiveProject = [&](const std::string& proj_name) {
    if (!proj_name.empty()) {
      config_mgr.setCurrentProject(proj_name);
    }
    const std::string root = config_mgr.activeRoot();
    const std::string consts = config_mgr.resolveConstantsPath(cli_opt.constants_path);
    const std::string headers = config_mgr.resolveHeadersDir();
    const std::string overrides = config_mgr.resolveOverridesDir(cli_opt.overrides_dir);
    const std::string enhancements = config_mgr.resolveEnhancementsDir(cli_opt.enhancements_dir);

    catalog.load(root, consts, headers);
    enh_mgr.setRepoRoot(root);
    enh_mgr.setEnhanceDir(enhancements);
    enh_mgr.setRevisionsDir(root.empty() ? "" : root + "/dos_port/tools/audio/.revisions");

    track_names.clear();
    if (!catalog.empty()) {
      for (const auto& t : catalog.tracks()) {
        track_names.push_back(t.constant_name);
      }
    } else {
      track_names.push_back("MUSIC_PALLET_TOWN");
    }
    track_index = 0;
    for (std::size_t i = 0; i < track_names.size(); ++i) {
      if (track_names[i] == "MUSIC_PALLET_TOWN") {
        track_index = static_cast<int>(i);
        break;
      }
    }
    last_loaded_track = -1;  // Force reload of baseline
  };

  reloadActiveProject(config_mgr.currentProjectName());

  std::map<std::string, audio_dbg::SongCatalog> other_catalogs;
  auto getProjectCatalog = [&](const std::string& proj_name) -> audio_dbg::SongCatalog& {
    auto it = other_catalogs.find(proj_name);
    if (it != other_catalogs.end()) {
      return it->second;
    }
    const audio_dbg::ProjectConfig* cfg = config_mgr.project(proj_name);
    audio_dbg::SongCatalog cat;
    if (cfg != nullptr && !cfg->root.empty()) {
      std::string c_path = cfg->constants.empty() ? "" :
          (cfg->constants.front() == '/' ? cfg->constants : cfg->root + "/" + cfg->constants);
      std::string h_dir = cfg->headers.empty() ? "" :
          (cfg->headers.front() == '/' ? cfg->headers : cfg->root + "/" + cfg->headers);
      cat.load(cfg->root, c_path, h_dir);
    }
    auto res = other_catalogs.emplace(proj_name, std::move(cat));
    return res.first->second;
  };

  bool enhance_gb = true;
  char search_buf[128] = "";
  bool cross_project_search = false;
  bool focus_search_input = false;

  // --- Main loop, 60 Hz paced -----------------------------------------------
  // Optional transport-clock instrumentation (DGAD_CLOCK_LOG=1) proves the
  // audio clock never dispatches a back-to-back zero-render tick burst:
  // max_ticks_per_chunk must stay 1 and zero_render_burst must stay "no".
  const bool clock_log = std::getenv("DGAD_CLOCK_LOG") != nullptr;
  int clock_log_frames = 0;
  bool running = true;
  while (running) {
    const Uint32 frame_start = SDL_GetTicks();

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL2_ProcessEvent(&event);
      if (event.type == SDL_QUIT) {
        running = false;
      } else if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
        if (event.key.keysym.sym == SDLK_ESCAPE) {
          running = false;
        } else if ((event.key.keysym.mod & KMOD_CTRL) && (event.key.keysym.sym == SDLK_f)) {
          focus_search_input = true;
        } else if (!(event.key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_GUI)) &&
                   event.key.keysym.sym == SDLK_SLASH && !io.WantTextInput) {
          focus_search_input = true;
        } else if (event.key.keysym.sym >= SDLK_F1 && event.key.keysym.sym <= SDLK_F4) {
          request_tab_switch = event.key.keysym.sym - SDLK_F1;
        } else if ((event.key.keysym.mod & KMOD_ALT) &&
                   event.key.keysym.sym >= SDLK_1 && event.key.keysym.sym <= SDLK_4) {
          request_tab_switch = event.key.keysym.sym - SDLK_1;
        } else if (!io.WantTextInput) {
          // Stage 6.4: transport shortcuts handled only when text input is not active
          audio_dbg::TransportKey tkey;
          if (sdlToTransportKey(event.key.keysym.sym, &tkey)) {
            mixer.lock();
            transport.handleKey(engine, &enh_mgr, tkey);
            mixer.unlock();
          }
        }
      }
    }

    // Stage 5.3: hot-reload the enhancement overlay when the watched YAML
    // changes. The 60 Hz engine ticks are NOT advanced from here any more:
    // AudioClockShim::render() paces them off the audio callback's
    // rendered-sample count (see its comment), so the UI frame rate no longer
    // owns the clock. Only with no audio device to clock against (mixer
    // disabled) do we fall back to the UI-frame pacer.
    mixer.lock();
    if (enh_mgr.pollForChanges()) {
      const std::string song = enh_mgr.watchedSong();
      if (!song.empty()) {
        const auto live = enh_mgr.loadYamlFile(song);
        if (!live.first) {
          engine.setEnhancementEvents({});
        } else {
          std::vector<audio_dbg::SimNoteEvent> compiled =
              enh_mgr.compileEnhancement(song, "mt32");
          engine.setEnhancementEvents(std::move(compiled));
        }
      }
    }
    if (mixer.isDisabled()) {
      transport.advance(engine);  // No audio clock: UI-frame fallback.
    } else {
      clock_shim.setSpeed(transport.speed());
    }

    mixer.unlock();

    if (clock_log && ++clock_log_frames >= 120) {
      std::fprintf(stderr,
                   "[clock] ticks=%llu render_chunks=%llu "
                   "max_ticks_per_chunk=%d zero_render_burst=%s rendered=%llu\n",
                   static_cast<unsigned long long>(clock_shim.ticksTotal()),
                   static_cast<unsigned long long>(clock_shim.renderChunks()),
                   clock_shim.maxTicksPerChunk(),
                   clock_shim.sawZeroRenderBurst() ? "YES" : "no",
                   static_cast<unsigned long long>(clock_shim.renderedSamples()));
      clock_shim.resetStats();
      clock_log_frames = 0;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    // Top bar: project switcher, track search bar, track selector + enhancement toggles.
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y));
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, 56));
    ImGui::Begin("Top Bar", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

    // 1. Project Switcher combo
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Project:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f);
    std::string current_proj = config_mgr.currentProjectName();
    if (ImGui::BeginCombo("##ProjectSelect", current_proj.c_str())) {
      for (const auto& p : config_mgr.projects()) {
        const bool is_selected = (p == current_proj);
        if (ImGui::Selectable(p.c_str(), is_selected)) {
          if (p != current_proj) {
            mixer.lock();
            reloadActiveProject(p);
            mixer.unlock();
          }
        }
        if (is_selected) {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }

    // 2. Track Search Bar with shortcut (/ or Ctrl+F)
    ImGui::SameLine();
    ImGui::Text("Search:");
    ImGui::SameLine();
    if (focus_search_input) {
      ImGui::SetKeyboardFocusHere();
      focus_search_input = false;
    }
    ImGui::SetNextItemWidth(150.0f);
    const bool search_submitted = ImGui::InputTextWithHint(
        "##TrackSearch", "/ or Ctrl+F...", search_buf, sizeof(search_buf),
        ImGuiInputTextFlags_EnterReturnsTrue);

    // 3. Search Scope Toggle Button
    ImGui::SameLine();
    const char* scope_label = cross_project_search ? "[Scope: All]" : "[Scope: Project]";
    if (ImGui::Button(scope_label)) {
      cross_project_search = !cross_project_search;
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Toggle track search scope:\n- Project: search within '%s'\n- All: search across all configured projects",
                        current_proj.c_str());
    }

    // 4. Track Selection Combo (filtered by search)
    ImGui::SameLine();
    ImGui::Text("Track:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(260.0f);

    struct TrackOption {
      std::string project;
      std::string track;
      int track_index_in_project;
    };
    std::vector<TrackOption> options;

    if (!cross_project_search) {
      const std::vector<int> matched_indices = catalog.searchTracks(search_buf);
      options.reserve(matched_indices.size());
      for (int idx : matched_indices) {
        if (idx >= 0 && idx < static_cast<int>(track_names.size())) {
          options.push_back({current_proj, track_names[idx], idx});
        }
      }
    } else {
      for (const auto& p : config_mgr.projects()) {
        if (p == current_proj) {
          const std::vector<int> matched_indices = catalog.searchTracks(search_buf);
          for (int idx : matched_indices) {
            if (idx >= 0 && idx < static_cast<int>(track_names.size())) {
              options.push_back({p, track_names[idx], idx});
            }
          }
        } else {
          auto& cat = getProjectCatalog(p);
          const std::vector<int> matched_indices = cat.searchTracks(search_buf);
          for (int idx : matched_indices) {
            if (idx >= 0 && idx < static_cast<int>(cat.tracks().size())) {
              options.push_back({p, cat.tracks()[idx].constant_name, idx});
            }
          }
        }
      }
    }

    // Handle Enter pressed in search bar: pick first match
    if (search_submitted && !options.empty()) {
      const auto& opt = options[0];
      if (opt.project != current_proj) {
        mixer.lock();
        reloadActiveProject(opt.project);
        mixer.unlock();
      }
      for (std::size_t i = 0; i < track_names.size(); ++i) {
        if (track_names[i] == opt.track) {
          track_index = static_cast<int>(i);
          break;
        }
      }
    }

    std::string preview_text;
    if (track_index >= 0 && track_index < static_cast<int>(track_names.size())) {
      preview_text = track_names[track_index];
    } else {
      preview_text = "(No Track Selected)";
    }
    if (search_buf[0] != '\0') {
      preview_text += " (" + std::to_string(options.size()) + " matches)";
    }

    if (ImGui::BeginCombo("##TrackCombo", preview_text.c_str())) {
      if (options.empty()) {
        ImGui::Selectable("No matching tracks found", false, ImGuiSelectableFlags_Disabled);
      } else {
        for (const auto& opt : options) {
          std::string label;
          if (cross_project_search) {
            label = "[" + opt.project + "] " + opt.track;
          } else {
            label = opt.track;
          }
          const bool is_sel = (opt.project == current_proj &&
                               track_index >= 0 &&
                               track_index < static_cast<int>(track_names.size()) &&
                               opt.track == track_names[track_index]);
          if (ImGui::Selectable(label.c_str(), is_sel)) {
            if (opt.project != current_proj) {
              mixer.lock();
              reloadActiveProject(opt.project);
              mixer.unlock();
            }
            for (std::size_t i = 0; i < track_names.size(); ++i) {
              if (track_names[i] == opt.track) {
                track_index = static_cast<int>(i);
                break;
              }
            }
          }
          if (is_sel) {
            ImGui::SetItemDefaultFocus();
          }
        }
      }
      ImGui::EndCombo();
    }

    // 5. Enhancement Toggles
    ImGui::SameLine();
    {
      // Stage 6.3: the checkbox mirrors the engine overlay flag, so the
      // transport bar's [E]/[G] button and this box can never disagree.
      bool enh_on = engine.isEnhancementEnabled();
      if (ImGui::Checkbox("[E] Enhancements", &enh_on)) {
        mixer.lock();
        engine.setEnhancementEnabled(enh_on);
        mixer.unlock();
      }
    }
    ImGui::SameLine();
    ImGui::Checkbox("[G] GB", &enhance_gb);
    ImGui::End();

    // Stage 6.5: (re)load the GB baseline when the track selection changes
    // (also runs once on the first frame for the default track).
    if (track_index != last_loaded_track && track_index >= 0 &&
        track_index < static_cast<int>(track_names.size())) {
      mixer.lock();
      const char* target = (device_tab == 3) ? "gm" : "mt32";
      loadTrackBaseline(engine, enh_mgr, catalog,
                        track_names[static_cast<std::size_t>(track_index)].c_str(),
                        target, &mt32_dev);
      last_loaded_track = track_index;
      mixer.unlock();
    }

    // Device tab bar: one tab per sound device. Selecting a tab routes
    // both the engine event stream and the mixer PCM pull to that device
    // (position-locked: the frame counter is untouched by the switch).
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + 56));
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, viewport->Size.y - 56 - 104));
    ImGui::Begin("Devices", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
    if (ImGui::BeginTabBar("DeviceTabBar")) {
      for (int i = 0; i < 4; ++i) {
        ImGuiTabItemFlags flags = 0;
        if (request_tab_switch == i) {
          flags |= ImGuiTabItemFlags_SetSelected;
        }
        if (ImGui::BeginTabItem(kDeviceTabs[i], nullptr, flags)) {
          if (device_tab != i) {
            mixer.lock();
            device_tab = i;
            engine.setActiveDevice(devices[i]);
            clock_shim.setInner(devices[i], device_rates[i]);
            mixer.setDevice(&clock_shim, device_rates[i]);
            const char* target = (device_tab == 3) ? "gm" : "mt32";
            if (track_index >= 0 && track_index < static_cast<int>(track_names.size())) {
              loadTrackBaseline(engine, enh_mgr, catalog,
                                track_names[static_cast<std::size_t>(track_index)].c_str(),
                                target, &mt32_dev);
            }
            engine.syncDeviceState();
            mixer.unlock();
          }
          const audio_dbg::DeviceSnapshot snap = devices[i]->snapshot();
          tabs[i]->drawChannelStrips(snap);
          tabs[i]->drawDetail(snap);
          // Ticks now run on the audio callback thread; snapshot the engine
          // frame under the same lock to avoid a torn read.
          audio_dbg::SimState sim;
          mixer.lock();
          sim = buildSimState(engine);
          mixer.unlock();
          tabs[i]->drawTracker(snap, &sim);
          tracker.draw(&sim, snap, devices[i]->channelCount());
          ImGui::EndTabItem();
        }
      }
      request_tab_switch = -1;
      ImGui::EndTabBar();
    }
    ImGui::TextDisabled("Active device tab: %s", kDeviceTabs[device_tab]);
    ImGui::End();

    // Stage 6.1-6.3: transport bar (play/pause/stop, readout, seek
    // scrubber, loop/speed rows, enhancement + slot toggles).
    mixer.lock();
    transport.render(engine, &enh_mgr);
    mixer.unlock();

    ImGui::Render();
    glViewport(0, 0, static_cast<int>(io.DisplaySize.x),
               static_cast<int>(io.DisplaySize.y));
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(window);

    // 60 Hz pacing: sleep the remainder of the frame budget.
    const Uint32 frame_ms = SDL_GetTicks() - frame_start;
    if (frame_ms < kFrameBudgetMs) {
      SDL_Delay(kFrameBudgetMs - frame_ms);
    }
  }

  // --- Clean shutdown ---------------------------------------------------------
  mixer.shutdown();
  opl3_dev.shutdown();
  gbapu_dev.shutdown();
  mt32_dev.shutdown();
  gm_dev.shutdown();
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImPlot::DestroyContext();
  ImGui::DestroyContext();

  SDL_GL_DeleteContext(gl_context);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
