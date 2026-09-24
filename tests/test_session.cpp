// Stage 5.5 (Part 5A): Acceptance tests for SongCatalog + SessionEngine.
// Verifies:
//   SongCatalog (real repo data via constants/music_constants.asm +
//     audio/headers/*.asm):
//     1. More than 50 tracks parsed (music + SFX constants).
//     2. Fuzzy resolution: 'pallet' -> MUSIC_PALLET_TOWN,
//        'gym' -> MUSIC_GYM, 'route1' -> MUSIC_ROUTES1,
//        'title' -> MUSIC_TITLE_SCREEN.
//     3. Exact constant, exact header, and case-insensitive lookups.
//     4. Header metadata bound (bank 1..4, channel_count > 0, file path).
//   SessionEngine:
//     1. Frame stepping: N ticks advance current_frame by N and step the
//        active device with the same frame numbers.
//     2. play/pause/resume/stop/seek semantics.
//     3. Loop wrap-around (loop_end -> loop_start).
//     4. Event dispatch to a mock SoundDevice (opcode + payload).
//     5. Note-on/off translation to a mock MidiDevice.
//     6. End-of-track auto-stop; device swap preserves the frame counter.
//   Stage 5.5 (Part 5B):
//     7. Real Yellow note loading: Music_PalletTown mt32 baseline (notes
//        populated, channels in {1,2,3,9}, exact-frame determinism, full
//        play-through dispatch parity) + Music_Routes1 gm spot check.
//     8. Compiled Music_PalletTown enhancement (mt32 + gm targets).
//     9. EnhancementManager revisions (snapshot dedup/list/content/revert)
//        and mtime polling, isolated in a temp dir.
//     10. Position-locked A/B switching: slot swap, enhancement toggle,
//         and device swap preserve current_frame, silence the outgoing
//         MidiDevice, and continue the incoming stream at current_frame.
// Headless: no GUI, no audio hardware. Exits 0 with ALL PASS, 1 on failure.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "devices/midi_device.h"
#include "devices/sound_device.h"
#include "enhancement_manager.h"
#include "session_engine.h"
#include "song_catalog.h"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    ++g_checks;                                                         \
    if (!(cond)) {                                                      \
      ++g_failures;                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
    }                                                                   \
  } while (0)

struct Command {
  std::uint8_t opcode = 0;
  std::vector<std::uint8_t> payload;
};

// Minimal SoundDevice mock: records handleCommand packets + tick frames.
class MockDevice : public audio_dbg::SoundDevice {
 public:
  MockDevice() : SoundDevice(42, "Mock", 4) {}

  bool init() override { return true; }
  void shutdown() override {}
  void reset() override {}
  void render(float* buf, std::size_t frames) override {
    for (std::size_t i = 0; i < frames; ++i) buf[i] = 0.0f;
  }
  void renderPerChannel(float** bufs, std::size_t frames) override {
    for (int c = 0; c < channelCount(); ++c) {
      for (std::size_t i = 0; i < frames; ++i) bufs[c][i] = 0.0f;
    }
  }
  void setMute(int ch, bool muted) override { setMutedState(ch, muted); }
  void setSolo(int ch, bool soloed) override { setSoloedState(ch, soloed); }
  void tick(std::uint32_t frame) override {
    setFrame(frame);
    tick_frames.push_back(frame);
  }
  void handleCommand(std::uint8_t opcode, const std::uint8_t* payload,
                     std::size_t len) override {
    Command cmd;
    cmd.opcode = opcode;
    if (payload != nullptr) {
      for (std::size_t i = 0; i < len; ++i) cmd.payload.push_back(payload[i]);
    }
    // Wake the addressed channel like the real backends do.
    if (!cmd.payload.empty()) wakeChannel(cmd.payload[0] % channelCount());
    commands.push_back(cmd);
  }
  audio_dbg::DeviceSnapshot snapshot() const override {
    return makeSnapshot();
  }

  std::vector<Command> commands;
  std::vector<std::uint32_t> tick_frames;
};

// Minimal MidiDevice mock: silent synth, records nothing beyond MidiDevice's
// own note matrix/history.
class MockMidi : public audio_dbg::MidiDevice {
 public:
  MockMidi() : MidiDevice(43, "MockMidi", 16) {}

  bool init() override { return true; }
  void shutdown() override {}
  void reset() override {}
  void render(float* buf, std::size_t frames) override {
    for (std::size_t i = 0; i < frames; ++i) buf[i] = 0.0f;
  }
  void renderPerChannel(float** bufs, std::size_t frames) override {
    for (int c = 0; c < channelCount(); ++c) {
      for (std::size_t i = 0; i < frames; ++i) bufs[c][i] = 0.0f;
    }
  }
  void dispatchNoteOn(int ch, int note, int velocity) override {
    (void)ch;
    (void)note;
    (void)velocity;
    ++dispatched_ons;
  }
  void dispatchNoteOff(int ch, int note) override {
    (void)ch;
    (void)note;
    ++dispatched_offs;
  }

  int dispatched_ons = 0;
  int dispatched_offs = 0;
};

}  // namespace

int main() {
  using namespace audio_dbg;

  // --- SongCatalog: parses the real repo --------------------------------
#ifdef PKMN_TEST_REPO_ROOT
  SongCatalog cat(PKMN_TEST_REPO_ROOT);
#else
  SongCatalog cat;
#endif
  CHECK(!cat.empty());
  CHECK(cat.size() > 50);
  std::printf("INFO catalog: %zu tracks from %s\n", cat.size(),
              cat.repoRoot().c_str());

  // --- SongCatalog: fuzzy resolution ------------------------------------
  const SongInfo* pallet = cat.findTrack("pallet");
  CHECK(pallet != nullptr);
  if (pallet != nullptr) {
    CHECK(pallet->constant_name == "MUSIC_PALLET_TOWN");
  }
  const SongInfo* gym = cat.findTrack("gym");
  CHECK(gym != nullptr);
  if (gym != nullptr) CHECK(gym->constant_name == "MUSIC_GYM");
  const SongInfo* route1 = cat.findTrack("route1");
  CHECK(route1 != nullptr);
  if (route1 != nullptr) CHECK(route1->constant_name == "MUSIC_ROUTES1");
  const SongInfo* title = cat.findTrack("title");
  CHECK(title != nullptr);
  if (title != nullptr) CHECK(title->constant_name == "MUSIC_TITLE_SCREEN");
  std::printf("PASS catalog fuzzy resolution\n");

  // --- SongCatalog: exact + case-insensitive -----------------------------
  const SongInfo* exact = cat.findTrack("MUSIC_PALLET_TOWN");
  CHECK(exact != nullptr);
  if (exact != nullptr) CHECK(exact->header_label == "Music_PalletTown");
  const SongInfo* header = cat.findTrack("Music_PalletTown");
  CHECK(header != nullptr);
  if (header != nullptr) CHECK(header->constant_name == "MUSIC_PALLET_TOWN");
  const SongInfo* folded = cat.findTrack("music_pallet_town");
  CHECK(folded != nullptr);
  if (folded != nullptr) CHECK(folded->constant_name == "MUSIC_PALLET_TOWN");
  const SongInfo* stem = cat.findTrack("pallet_town");
  CHECK(stem != nullptr);
  if (stem != nullptr) CHECK(stem->constant_name == "MUSIC_PALLET_TOWN");
  CHECK(cat.findTrack("") == nullptr);
  CHECK(cat.findTrack("zzz_no_such_track_xyz") == nullptr);
  std::printf("PASS catalog exact/case-insensitive\n");

  // --- SongCatalog: header metadata --------------------------------------
  if (pallet != nullptr) {
    CHECK(pallet->bank >= 1 && pallet->bank <= 4);
    CHECK(pallet->channel_count > 0);
    CHECK(!pallet->file_path.empty());
    CHECK(!pallet->channels.empty());
    CHECK(static_cast<int>(pallet->channels.size()) == pallet->channel_count);
  }
  // Every music track header resolves to a bank + channel count.
  int unbound_music = 0;
  for (const SongInfo& s : cat.tracks()) {
    if (s.constant_name.rfind("MUSIC_", 0) == 0 && s.channel_count == 0) {
      ++unbound_music;
      std::printf("INFO unbound music header: %s -> %s\n",
                  s.constant_name.c_str(), s.header_label.c_str());
    }
  }
  CHECK(unbound_music == 0);
  std::printf("PASS catalog header metadata\n");

  // --- SessionEngine: frame stepping -------------------------------------
  {
    SessionEngine eng;
    MockDevice dev;
    eng.setActiveDevice(&dev);
    eng.setTotalFrames(1000);
    CHECK(eng.isStopped());
    eng.play();
    CHECK(eng.isPlaying());
    CHECK(!eng.isPaused());
    CHECK(!eng.isStopped());
    for (int i = 0; i < 10; ++i) eng.tick();
    CHECK(eng.currentFrame() == 10);
    CHECK(dev.tick_frames.size() == 10);
    for (std::size_t i = 0; i < dev.tick_frames.size(); ++i) {
      CHECK(dev.tick_frames[i] == i);
    }
    std::printf("PASS engine frame stepping\n");
  }

  // --- SessionEngine: play/pause/resume/stop/seek -------------------------
  {
    SessionEngine eng;
    MockDevice dev;
    eng.setActiveDevice(&dev);
    eng.setTotalFrames(100);
    eng.play();
    eng.tick();
    eng.tick();
    eng.tick();
    CHECK(eng.currentFrame() == 3);
    eng.pause();
    CHECK(eng.isPaused());
    eng.tick();
    eng.tick();
    CHECK(eng.currentFrame() == 3);  // Frozen while paused.
    CHECK(dev.tick_frames.size() == 3);
    eng.play();  // Resume.
    CHECK(!eng.isPaused());
    eng.tick();
    CHECK(eng.currentFrame() == 4);
    eng.seekToFrame(30);
    CHECK(eng.currentFrame() == 30);
    eng.tick();
    CHECK(eng.currentFrame() == 31);
    eng.seekToFrame(500);  // Clamped to total_frames.
    CHECK(eng.currentFrame() == 100);
    eng.stop();
    CHECK(eng.isStopped());
    CHECK(!eng.isPlaying());
    CHECK(eng.currentFrame() == 0);
    eng.tick();  // No-op while stopped.
    CHECK(eng.currentFrame() == 0);
    std::printf("PASS engine play/pause/seek/stop\n");
  }

  // --- SessionEngine: loop wrap-around ------------------------------------
  {
    SessionEngine eng;
    MockDevice dev;
    eng.setActiveDevice(&dev);
    eng.setTotalFrames(100);
    eng.setLoop(2, 5);
    CHECK(eng.loopEnabled());
    eng.seekToFrame(4);
    eng.play();
    eng.tick();  // Frame 4 -> advance 5 -> wrap to 2.
    CHECK(eng.currentFrame() == 2);
    eng.tick();  // Frame 2 -> 3.
    CHECK(eng.currentFrame() == 3);
    eng.tick();  // -> 4.
    CHECK(eng.currentFrame() == 4);
    eng.tick();  // -> wrap to 2 again.
    CHECK(eng.currentFrame() == 2);
    eng.setLoop(0, 0);
    CHECK(!eng.loopEnabled());
    std::printf("PASS engine loop wrap-around\n");
  }

  // --- SessionEngine: mock event dispatch to SoundDevice -------------------
  {
    SessionEngine eng;
    MockDevice dev;
    eng.setActiveDevice(&dev);
    eng.setTotalFrames(100);
    SimNoteEvent on;
    on.frame = 1;
    on.channel = 0;
    on.note = 60;
    on.velocity = 100;
    on.duration_frames = 10;
    on.is_note_on = true;
    eng.addEvent(on);
    SimNoteEvent off;
    off.frame = 3;
    off.channel = 0;
    off.note = 60;
    off.velocity = 0;
    off.duration_frames = 0;
    off.is_note_on = false;
    eng.addEvent(off);
    CHECK(eng.eventCount() == 2);
    eng.play();
    eng.tick();  // Frame 0: nothing scheduled.
    CHECK(dev.commands.empty());
    eng.tick();  // Frame 1: note-on.
    CHECK(dev.commands.size() == 1);
    CHECK(dev.commands[0].opcode == 0x90);
    CHECK(dev.commands[0].payload.size() == 3);
    CHECK(dev.commands[0].payload[0] == 0);
    CHECK(dev.commands[0].payload[1] == 60);
    CHECK(dev.commands[0].payload[2] == 100);
    eng.tick();  // Frame 2: nothing.
    CHECK(dev.commands.size() == 1);
    eng.tick();  // Frame 3: note-off.
    CHECK(dev.commands.size() == 2);
    CHECK(dev.commands[1].opcode == 0x80);
    CHECK(dev.commands[1].payload.size() == 2);
    CHECK(dev.commands[1].payload[1] == 60);
    std::printf("PASS engine event dispatch\n");
  }

  // --- SessionEngine: note translation to MidiDevice -----------------------
  {
    SessionEngine eng;
    MockMidi midi;
    CHECK(midi.init());
    eng.setActiveDevice(&midi);
    eng.setTotalFrames(10);
    eng.addNote(0, 0, 64, 100, 2);  // On @0, off @2.
    CHECK(eng.eventCount() == 2);
    eng.play();
    eng.tick();  // Frame 0: note-on.
    CHECK(midi.isNoteSounding(0, 64));
    CHECK(midi.dispatched_ons == 1);
    eng.tick();  // Frame 1: held.
    CHECK(midi.isNoteSounding(0, 64));
    eng.tick();  // Frame 2: note-off.
    CHECK(!midi.isNoteSounding(0, 64));
    CHECK(midi.dispatched_offs == 1);
    midi.shutdown();
    std::printf("PASS engine midi translation\n");
  }

  // --- SessionEngine: end-of-track auto-stop + device swap ------------------
  {
    SessionEngine eng;
    MockDevice dev_a, dev_b;
    eng.setActiveDevice(&dev_a);
    eng.setTotalFrames(3);
    eng.play();
    eng.tick();
    eng.tick();
    CHECK(eng.currentFrame() == 2);
    eng.setActiveDevice(&dev_b);  // Swap preserves the frame counter.
    CHECK(eng.currentFrame() == 2);
    CHECK(eng.activeDevice() == &dev_b);
    eng.tick();  // Frame 2 -> 3 == total -> auto-stop.
    CHECK(eng.currentFrame() == 3);
    CHECK(eng.isStopped());
    CHECK(!eng.isPlaying());
    CHECK(dev_b.tick_frames.size() == 1);
    CHECK(dev_b.tick_frames[0] == 2);
    eng.tick();  // No-op once stopped.
    CHECK(eng.currentFrame() == 3);
    eng.play();  // Restarts from 0 after reaching the end.
    CHECK(eng.currentFrame() == 0);
    CHECK(eng.isPlaying());
    std::printf("PASS engine auto-stop + device swap\n");
  }

  // --- SessionEngine: null device still advances ----------------------------
  {
    SessionEngine eng;
    eng.setTotalFrames(50);
    eng.play();
    eng.tick();
    eng.tick();
    CHECK(eng.currentFrame() == 2);
    std::printf("PASS engine null device\n");
  }

  // --- Stage 5.5 (Part 5B): real Yellow note loading -----------------------
  // Music_PalletTown mt32 baseline: populated, GB channels {1,2,3,9},
  // frame-sorted, on/off balanced, bit-deterministic across loads, and a
  // full engine play-through dispatches every note on and off.
  {
#ifdef PKMN_TEST_REPO_ROOT
    EnhancementManager mgr(PKMN_TEST_REPO_ROOT);
#else
    EnhancementManager mgr;
#endif
    CHECK(!mgr.repoRoot().empty());
    std::printf("INFO enh-mgr repo root: %s\n", mgr.repoRoot().c_str());
    std::vector<SimNoteEvent> base =
        mgr.loadSongBaseline("Music_PalletTown", "mt32");
    CHECK(!base.empty());
    std::size_t ons = 0, offs = 0;
    std::set<int> chans;
    std::uint32_t max_frame = 0;
    bool sorted = true;
    std::uint32_t prev = 0;
    bool first = true;
    for (const SimNoteEvent& e : base) {
      if (e.type == SimEventType::ProgramChange ||
          e.type == SimEventType::ControlChange) continue;
      if (e.is_note_on) {
        ++ons;
        CHECK(e.velocity >= 1 && e.velocity <= 127);
      } else {
        ++offs;
      }
      chans.insert(e.channel);
      if (e.frame > max_frame) max_frame = e.frame;
      if (!first && e.frame < prev) sorted = false;
      prev = e.frame;
      first = false;
    }
    CHECK(ons > 0);
    CHECK(ons == offs);
    CHECK(sorted);
    // Baselines carry the GB base only: the baked "enh ..." tracks in the
    // .mid (gb_to_midi merges them for soundtrack export) are dropped at
    // load, and runtime enhancement arrives via compileEnhancement (5B
    // below) under the active device's tier target. PalletTown therefore
    // sounds GB ch1/2/3 on MIDI 1/2/3 (no drums on this track) and no
    // enhancement channels. A strict base-only channel check lives on
    // Music_Gym below (no enhancement file).
    for (int c : chans) {
      CHECK(c >= 1 && c <= 3);
    }
    CHECK(chans.count(1) == 1);  // Lead voice present.
    CHECK(chans.count(2) == 1);
    CHECK(chans.count(3) == 1);
    CHECK(chans.count(4) == 0);  // No baked enhancement in baselines.
    std::printf("INFO pallet baseline: %zu events (%zu notes), %u frames\n",
                base.size(), ons, max_frame);
    // Determinism: a second load is field-identical (exact frame timings).
    std::vector<SimNoteEvent> base2 =
        mgr.loadSongBaseline("Music_PalletTown", "mt32");
    auto same_events = [](const std::vector<SimNoteEvent>& a,
                          const std::vector<SimNoteEvent>& b) {
      if (a.size() != b.size()) return false;
      for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].frame != b[i].frame || a[i].channel != b[i].channel ||
            a[i].note != b[i].note || a[i].velocity != b[i].velocity ||
            a[i].duration_frames != b[i].duration_frames ||
            a[i].is_note_on != b[i].is_note_on) {
          return false;
        }
      }
      return true;
    };
    CHECK(same_events(base, base2));
    // Missing song / missing file -> empty, never a crash.
    CHECK(mgr.loadSongBaseline("Nope_NotASong", "mt32").empty());
    CHECK(mgr.loadMidiFile("/nonexistent/path.mid").empty());
    // Loop markers: the SMF "loopStart"/"loopEnd" marker meta events carry the
    // song's loop point (gb_to_midi.py). PalletTown has an intro (loop_start
    // 1920) so its loop must NOT be the whole-song 0..end; JigglypuffSong is
    // a through-composed one-shot (loop_start None) so it reports no loop.
    {
      audio_dbg::MidiFileData loop = mgr.parseMidiFile(
          mgr.midiPathFor("Music_PalletTown", "mt32"));
      std::printf("INFO pallet loop: start=%u end=%u\n",
                  loop.loop_start_frame, loop.loop_end_frame);
      CHECK(loop.loop_end_frame > loop.loop_start_frame);
      CHECK(loop.loop_start_frame > 0);
      audio_dbg::MidiFileData oneshot = mgr.parseMidiFile(
          mgr.midiPathFor("Music_JigglypuffSong", "mt32"));
      std::printf("INFO jiggly loop: start=%u end=%u\n",
                  oneshot.loop_start_frame, oneshot.loop_end_frame);
      CHECK(oneshot.loop_start_frame == 0 && oneshot.loop_end_frame == 0);
    }
    // GM spot check on a track that ships a gm rendering.
    std::vector<SimNoteEvent> gm_base =
        mgr.loadSongBaseline("Music_Routes1", "gm");
    CHECK(!gm_base.empty());
    // Strict GB-base channel check on a track with NO enhancement file:
    // only pulse/wave ch1-3 and noise/drums ch9 may sound.
    CHECK(!mgr.loadYamlFile("Music_Gym").first);
    std::vector<SimNoteEvent> gym_base =
        mgr.loadSongBaseline("Music_Gym", "mt32");
    CHECK(!gym_base.empty());
    {
      std::set<int> gym_chans;
      for (const SimNoteEvent& e : gym_base) gym_chans.insert(e.channel);
      for (int c : gym_chans) {
        CHECK(c == 1 || c == 2 || c == 3 || c == 9);
      }
      CHECK(!gym_chans.empty());
    }
    // Full play-through: every loaded note-on/off reaches the device.
    SessionEngine eng;
    MockMidi midi;
    CHECK(midi.init());
    eng.setActiveDevice(&midi);
    eng.setEvents(base);
    eng.setTotalFrames(max_frame + 1);
    eng.play();
    while (eng.isPlaying()) eng.tick();
    CHECK(midi.dispatched_ons == static_cast<int>(ons));
    CHECK(midi.dispatched_offs == static_cast<int>(offs));
    CHECK(midi.program(1) == 72);
    CHECK(midi.program(2) == 50);
    CHECK(eng.currentFrame() == max_frame + 1);
    CHECK(eng.isStopped());
    midi.shutdown();
    std::printf("PASS real track note loading\n");
  }

  // --- Stage 5.5 (Part 5B): compiled YAML enhancement -----------------------
  {
#ifdef PKMN_TEST_REPO_ROOT
    EnhancementManager mgr(PKMN_TEST_REPO_ROOT);
#else
    EnhancementManager mgr;
#endif
    std::vector<SimNoteEvent> enh =
        mgr.compileEnhancement("Music_PalletTown", "mt32");
    CHECK(!enh.empty());
    std::size_t ons = 0, offs = 0;
    std::set<int> chans;
    bool sorted = true;
    std::uint32_t prev = 0;
    bool first = true;
    for (const SimNoteEvent& e : enh) {
      if (e.type == SimEventType::ProgramChange) continue;
      if (e.is_note_on) {
        ++ons;
      } else {
        ++offs;
      }
      chans.insert(e.channel);
      if (!first && e.frame < prev) sorted = false;
      prev = e.frame;
      first = false;
    }
    CHECK(ons > 0);
    CHECK(ons == offs);
    CHECK(sorted);
    for (int c : chans) {
      // Free melodic parts 4..8 (+ 9 for rhythm), mirroring the merge.
      CHECK((c >= 4 && c <= 9));
    }
    std::printf("INFO pallet enhancement: %zu events (%zu notes)\n",
                enh.size(), ons);
    std::vector<SimNoteEvent> enh_gm =
        mgr.compileEnhancement("Music_PalletTown", "gm");
    CHECK(!enh_gm.empty());
    CHECK(mgr.compileEnhancement("Nope_NotASong").empty());
    std::printf("PASS enhancement compilation\n");
  }

  // --- renderBaseline(): re-render .mid baselines via gb_to_midi ----------
  {
#ifdef PKMN_TEST_REPO_ROOT
    EnhancementManager mgr(PKMN_TEST_REPO_ROOT);
#else
    EnhancementManager mgr;
#endif
    // False cases first (no side effects): empty song, empty repo root.
    std::string err;
    CHECK(!mgr.renderBaseline("", &err));
    CHECK(!err.empty());
    EnhancementManager noroot("", "", "");
    CHECK(!noroot.renderBaseline("Music_PalletTown", &err));
    // True case: single-song render for both MIDI targets. Rewrites the
    // (gitignored) assets/midi/<target>/Music_PalletTown.mid files.
    CHECK(mgr.renderBaseline("Music_PalletTown", &err));
    namespace fs2 = std::filesystem;
    for (const char* t : {"mt32", "gm"}) {
      CHECK(!mgr.midiPathFor("Music_PalletTown", t).empty());
      CHECK(fs2::is_regular_file(mgr.midiPathFor("Music_PalletTown", t)));
    }
    std::printf("PASS baseline re-render\n");
  }

  // --- Stage 5.5 (Part 5B): revisions + mtime watcher (temp dir) ------------
  {
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / "pkmn-viewer-enh-test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp / "enh", ec);
    EnhancementManager m;
    m.setEnhanceDir((tmp / "enh").string());
    m.setRevisionsDir((tmp / "rev").string());
    const fs::path yaml = tmp / "enh" / "Music_TestSong.yaml";
    {
      std::ofstream f(yaml, std::ios::binary | std::ios::trunc);
      f << "v1";
    }
    m.watchSong("Music_TestSong");
    CHECK(!m.pollForChanges());  // Cached at watch time: no change.
    // Deterministic mtime bump (no sleep: filesystems may tick coarsely).
    fs::last_write_time(yaml, fs::last_write_time(yaml) +
                                   std::chrono::seconds(2),
                        ec);
    CHECK(!ec);
    CHECK(m.pollForChanges());
    CHECK(!m.pollForChanges());  // Second poll: already consumed.
    auto s1 = m.saveSnapshot("Music_TestSong", "v1", "first");
    CHECK(s1.first == 1);
    CHECK(!s1.second.empty());
    // Identical content: dedup, no new file.
    auto s1b = m.saveSnapshot("Music_TestSong", "v1", "again");
    CHECK(s1b.first == 1);
    CHECK(s1b.second == s1.second);
    CHECK(m.listRevisions("Music_TestSong").size() == 1);
    auto s2 = m.saveSnapshot("Music_TestSong", "v2", "second");
    CHECK(s2.first == 2);
    std::vector<RevisionEntry> revs = m.listRevisions("Music_TestSong");
    CHECK(revs.size() == 2);
    CHECK(revs[0].id == 1);
    CHECK(revs[1].id == 2);
    CHECK(!revs[0].timestamp.empty());
    auto c1 = m.getRevisionContent("Music_TestSong", 1);
    CHECK(c1.first);
    CHECK(c1.second == "v1");
    auto c2 = m.getRevisionContent("Music_TestSong", 2);
    CHECK(c2.first);
    CHECK(c2.second == "v2");
    CHECK(!m.getRevisionContent("Music_TestSong", 99).first);
    {
      std::ofstream f(yaml, std::ios::binary | std::ios::trunc);
      f << "v3-live";
    }
    CHECK(m.revertToRevision("Music_TestSong", 1));
    auto live = m.loadYamlFile("Music_TestSong");
    CHECK(live.first);
    CHECK(live.second == "v1");
    CHECK(!m.revertToRevision("Music_TestSong", 99));
    CHECK(!m.pollForChanges());  // Revert refreshed the watch cache.
    fs::remove_all(tmp, ec);
    std::printf("PASS revisions + watcher\n");
  }

  // --- Stage 5.5 (Part 5B): position-locked A/B slot switching --------------
  {
    SessionEngine eng;
    MockMidi midi;
    CHECK(midi.init());
    eng.setActiveDevice(&midi);
    eng.setTotalFrames(100);
    eng.addNote(2, 0, 60, 100, 10);  // Slot A (active): ch0 n60 @2..12.
    CHECK(eng.activeSlot() == ComparisonSlot::A);
    eng.setComparisonSlot(ComparisonSlot::B);
    eng.addNote(3, 1, 72, 100, 10);  // Slot B: ch1 n72 @3..13.
    eng.setComparisonSlot(ComparisonSlot::A);
    CHECK(eng.slotEvents(ComparisonSlot::A).size() == 2);
    CHECK(eng.slotEvents(ComparisonSlot::B).size() == 2);
    eng.play();
    eng.tick();  // Frame 0.
    eng.tick();  // Frame 1.
    eng.tick();  // Frame 2: A note-on.
    CHECK(eng.currentFrame() == 3);
    CHECK(midi.isNoteSounding(0, 60));
    const int offs_before = midi.dispatched_offs;
    eng.setComparisonSlot(ComparisonSlot::B);
    CHECK(eng.activeSlot() == ComparisonSlot::B);
    CHECK(eng.currentFrame() == 3);  // Position locked: no drift.
    CHECK(!midi.isNoteSounding(0, 60));  // Outgoing silenced.
    CHECK(midi.dispatched_offs > offs_before);
    eng.tick();  // Frame 3: B note-on; continues at current_frame.
    CHECK(eng.currentFrame() == 4);
    CHECK(midi.isNoteSounding(1, 72));
    eng.toggleSlotAB();
    CHECK(eng.activeSlot() == ComparisonSlot::A);
    CHECK(eng.currentFrame() == 4);
    CHECK(!midi.isNoteSounding(1, 72));
    // Re-selecting the active slot is a no-op (no extra silencing).
    const int offs_after = midi.dispatched_offs;
    eng.setComparisonSlot(ComparisonSlot::A);
    CHECK(midi.dispatched_offs == offs_after);
    CHECK(eng.currentFrame() == 4);
    midi.shutdown();
    std::printf("PASS position-locked A/B switching\n");
  }

  // --- Stage 5.5 (Part 5B): enhancement toggle ------------------------------
  {
    SessionEngine eng;
    MockMidi midi;
    CHECK(midi.init());
    eng.setActiveDevice(&midi);
    eng.setTotalFrames(50);
    eng.addNote(0, 0, 50, 100, 5);  // Base (slot A).
    SimNoteEvent eon;
    eon.frame = 0;
    eon.channel = 5;
    eon.note = 80;
    eon.velocity = 90;
    eon.duration_frames = 5;
    eon.is_note_on = true;
    SimNoteEvent eoff;
    eoff.frame = 5;
    eoff.channel = 5;
    eoff.note = 80;
    eoff.velocity = 0;
    eoff.duration_frames = 0;
    eoff.is_note_on = false;
    eng.setEnhancementEvents({eon, eoff});  // Overlay: ch5 n80 @0..5.
    CHECK(eng.isEnhancementEnabled());
    CHECK(eng.eventCount() == 4);  // Base pair + overlay pair.
    eng.play();
    eng.tick();  // Frame 0: both note-ons.
    CHECK(eng.currentFrame() == 1);
    CHECK(midi.isNoteSounding(0, 50));
    CHECK(midi.isNoteSounding(5, 80));
    eng.setEnhancementEnabled(false);
    CHECK(!eng.isEnhancementEnabled());
    CHECK(eng.currentFrame() == 1);  // Toggle preserves the frame.
    CHECK(!midi.isNoteSounding(0, 50));  // Toggle silences.
    CHECK(!midi.isNoteSounding(5, 80));
    CHECK(eng.eventCount() == 2);  // Base only.
    eng.seekToFrame(0);
    eng.tick();  // Frame 0, enhancement off: base only.
    CHECK(midi.isNoteSounding(0, 50));
    CHECK(!midi.isNoteSounding(5, 80));
    eng.setEnhancementEnabled(true);
    CHECK(eng.currentFrame() == 1);
    CHECK(!midi.isNoteSounding(0, 50));  // Re-enable silences first.
    CHECK(eng.eventCount() == 4);
    eng.seekToFrame(0);
    eng.tick();  // Frame 0, enhancement on: both again.
    CHECK(midi.isNoteSounding(0, 50));
    CHECK(midi.isNoteSounding(5, 80));
    // Unchanged value: no-op, no extra silencing.
    const int offs_before = midi.dispatched_offs;
    eng.setEnhancementEnabled(true);
    CHECK(midi.dispatched_offs == offs_before);
    midi.shutdown();
    std::printf("PASS enhancement toggle\n");
  }

  // --- Stage 5.5 (Part 5B): device swap silences + preserves frame ----------
  {
    SessionEngine eng;
    MockMidi dev1, dev2;
    CHECK(dev1.init());
    CHECK(dev2.init());
    eng.setActiveDevice(&dev1);
    eng.setTotalFrames(20);
    eng.addNote(0, 0, 64, 100, 10);
    eng.play();
    eng.tick();  // Frame 0: note-on on dev1.
    CHECK(eng.currentFrame() == 1);
    CHECK(dev1.isNoteSounding(0, 64));
    eng.setActiveDevice(&dev2);
    CHECK(eng.activeDevice() == &dev2);
    CHECK(eng.currentFrame() == 1);  // No drift across the swap.
    CHECK(!dev1.isNoteSounding(0, 64));  // Outgoing silenced.
    eng.tick();  // Frame 1 steps the NEW device.
    CHECK(dev2.snapshot().frame == 1);
    dev1.shutdown();
    dev2.shutdown();
    std::printf("PASS device swap silencing\n");
  }

  if (g_failures == 0) {
    std::printf("ALL PASS (%d checks)\n", g_checks);
    return 0;
  }
  std::printf("%d/%d checks FAILED\n", g_failures, g_checks);
  return 1;
}
