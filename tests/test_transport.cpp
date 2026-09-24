// Stage 6.5: Acceptance tests for TransportBar (docs/current_plan_debug_frontend.md §6).
// Verifies, headless (no GUI, no audio hardware, render() never called):
//   1. Time formatting: MM:SS.f at 60 Hz (and a custom fps), readout string.
//   2. Seek scrubber: clampFrame/fraction helpers, seek clamping, MIDI
//      silencing on seek, resync (playback continues dispatching after seek).
//   3. Speed multipliers: index/value selection + rejection, advance() pacing
//      at 0.25x/0.5x/1.0x/2.0x/4.0x, no debt while paused/stopped, stop reset.
//   4. Loop markers: Set [A]/[B] staging, wrap-around, clear, enable/disable
//      stash round-trip, no-stash fallback.
//   5. Shortcuts: Space/Home/Left/Right/E/Tab handling, [/] revision stepping
//      (null manager -> unhandled; temp-dir manager -> content-verified walk).
//   6. Baseline loader: synthetic .mid with GB + baked "enh ..." tracks —
//      whole-file parse keeps both layers, baseline parse drops the baked
//      enhancement note but keeps the base note and loop markers.
// Exits 0 with ALL PASS on success, 1 on any failure.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "devices/midi_device.h"
#include "devices/sound_device.h"
#include "enhancement_manager.h"
#include "session_engine.h"
#include "transport_bar.h"

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

// Minimal non-MIDI SoundDevice mock (mirrors tests/test_session.cpp).
class MockDevice : public audio_dbg::SoundDevice {
 public:
  MockDevice() : SoundDevice(42, "Mock", 4) {}

  bool init() override { return true; }
  void shutdown() override {}
  void reset() override { ++resets; }
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
    (void)opcode;
    if (len > 0 && payload != nullptr) {
      wakeChannel(payload[0] % channelCount());
    }
    ++commands;
  }
  audio_dbg::DeviceSnapshot snapshot() const override {
    return makeSnapshot();
  }

  std::vector<std::uint32_t> tick_frames;
  int commands = 0;
  int resets = 0;
};

// Minimal MidiDevice mock: silent synth beyond MidiDevice's own matrix.
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

  // --- 1. Time formatting -------------------------------------------------
  {
    CHECK(TransportBar::formatTime(0) == "00:00.0");
    CHECK(TransportBar::formatTime(6) == "00:00.1");
    CHECK(TransportBar::formatTime(30) == "00:00.5");
    CHECK(TransportBar::formatTime(60) == "00:01.0");
    CHECK(TransportBar::formatTime(90) == "00:01.5");
    CHECK(TransportBar::formatTime(3599) == "00:59.9");
    CHECK(TransportBar::formatTime(3600) == "01:00.0");
    CHECK(TransportBar::formatTime(5040) == "01:24.0");
    CHECK(TransportBar::formatTime(6030) == "01:40.5");
    CHECK(TransportBar::formatTime(9900) == "02:45.0");
    // Custom frame rate: 30 ticks at 30 fps is exactly one second.
    CHECK(TransportBar::formatTime(30, 30.0) == "00:01.0");
    CHECK(TransportBar::formatTime(15, 30.0) == "00:00.5");
    CHECK(TransportBar::formatReadout(5040, 9900) ==
          "01:24.0 / 02:45.0 (f: 5040/9900)");
    CHECK(TransportBar::formatReadout(0, 0) == "00:00.0 / 00:00.0 (f: 0/0)");
    std::printf("PASS time formatting\n");
  }

  // --- 2a. Clamp + fraction helpers ----------------------------------------
  {
    CHECK(TransportBar::clampFrame(-5, 100) == 0);
    CHECK(TransportBar::clampFrame(0, 100) == 0);
    CHECK(TransportBar::clampFrame(30, 100) == 30);
    CHECK(TransportBar::clampFrame(100, 100) == 100);
    CHECK(TransportBar::clampFrame(500, 100) == 100);
    CHECK(TransportBar::clampFrame(5, 0) == 0);
    CHECK(TransportBar::frameToFraction(50, 100) == 0.5f);
    CHECK(TransportBar::frameToFraction(0, 0) == 0.0f);
    CHECK(TransportBar::frameToFraction(7, 0) == 0.0f);
    CHECK(TransportBar::fractionToFrame(0.5f, 100) == 50);
    CHECK(TransportBar::fractionToFrame(0.0f, 100) == 0);
    CHECK(TransportBar::fractionToFrame(1.0f, 100) == 100);
    CHECK(TransportBar::fractionToFrame(2.0f, 100) == 100);
    CHECK(TransportBar::fractionToFrame(-1.0f, 100) == 0);
    CHECK(TransportBar::fractionToFrame(0.5f, 0) == 0);
    std::printf("PASS clamp + fractions\n");
  }

  // --- 2b. Seek scrubber: clamping, MIDI silence, playing resync ------------
  {
    SessionEngine eng;
    MockMidi midi;
    CHECK(midi.init());
    eng.setActiveDevice(&midi);
    eng.setTotalFrames(100);
    eng.addNote(0, 0, 64, 100, 50);  // On @0, off @50.
    TransportBar bar;
    eng.play();
    eng.tick();  // Frame 0: note-on.
    CHECK(midi.isNoteSounding(0, 64));
    bar.seekToFrame(eng, 20);
    CHECK(eng.currentFrame() == 20);
    CHECK(!midi.isNoteSounding(0, 64));  // Seek silenced the voice.
    CHECK(eng.isPlaying());              // Still playing after the scrub.
    eng.addNote(25, 0, 65, 100, 5);      // Later note to prove the resync.
    for (int i = 0; i < 6; ++i) eng.tick();  // Frames 20..25.
    CHECK(eng.currentFrame() == 26);
    CHECK(midi.isNoteSounding(0, 65));  // Re-dispatched at the new position.
    bar.seekToFrame(eng, 500);          // Clamped to total_frames.
    CHECK(eng.currentFrame() == 100);
    bar.seekToFrame(eng, 0);
    CHECK(eng.currentFrame() == 0);
    midi.shutdown();
    std::printf("PASS seek scrubber\n");
  }

  // --- 2c. Seek on a non-MIDI device resets instead of allNotesOff ---------
  {
    SessionEngine eng;
    MockDevice dev;
    eng.setActiveDevice(&dev);
    eng.setTotalFrames(50);
    TransportBar bar;
    eng.play();
    eng.tick();
    const int resets_before = dev.resets;
    bar.seekToFrame(eng, 10);
    CHECK(eng.currentFrame() == 10);
    CHECK(dev.resets == resets_before + 1);
    CHECK(eng.isPlaying());
    std::printf("PASS seek non-midi reset\n");
  }

  // --- 3. Speed multipliers -------------------------------------------------
  {
    CHECK(TransportBar::numSpeeds() == 5);
    CHECK(TransportBar::speedAt(0) == 0.25);
    CHECK(TransportBar::speedAt(1) == 0.5);
    CHECK(TransportBar::speedAt(2) == 1.0);
    CHECK(TransportBar::speedAt(3) == 2.0);
    CHECK(TransportBar::speedAt(4) == 4.0);
    CHECK(TransportBar::speedAt(-1) == 1.0);
    CHECK(TransportBar::speedAt(5) == 1.0);
    TransportBar bar;
    CHECK(bar.speedIndex() == 2);
    CHECK(bar.speed() == 1.0);
    CHECK(!bar.setSpeedIndex(-1));
    CHECK(!bar.setSpeedIndex(5));
    CHECK(bar.speedIndex() == 2);
    CHECK(!bar.setSpeed(3.0));
    CHECK(bar.speedIndex() == 2);
    CHECK(bar.setSpeed(2.0));
    CHECK(bar.speedIndex() == 3);

    // 1.0x: one UI frame advances exactly one engine frame.
    {
      SessionEngine eng;
      MockDevice dev;
      eng.setActiveDevice(&dev);
      eng.setTotalFrames(100);
      TransportBar b;
      eng.play();
      CHECK(b.advance(eng) == 1);
      CHECK(b.advance(eng) == 1);
      CHECK(b.advance(eng) == 1);
      CHECK(eng.currentFrame() == 3);
    }
    // 4.0x / 2.0x: multi-tick bursts per UI frame.
    {
      SessionEngine eng;
      MockDevice dev;
      eng.setActiveDevice(&dev);
      eng.setTotalFrames(100);
      TransportBar b;
      CHECK(b.setSpeedIndex(4));
      eng.play();
      CHECK(b.advance(eng) == 4);
      CHECK(eng.currentFrame() == 4);
      CHECK(b.setSpeedIndex(3));
      CHECK(b.advance(eng) == 2);
      CHECK(eng.currentFrame() == 6);
    }
    // 0.25x: one engine tick per four UI frames; 0.5x: one per two.
    {
      SessionEngine eng;
      MockDevice dev;
      eng.setActiveDevice(&dev);
      eng.setTotalFrames(100);
      TransportBar b;
      CHECK(b.setSpeedIndex(0));
      eng.play();
      CHECK(b.advance(eng) == 0);
      CHECK(b.advance(eng) == 0);
      CHECK(b.advance(eng) == 0);
      CHECK(eng.currentFrame() == 0);
      CHECK(b.advance(eng) == 1);
      CHECK(eng.currentFrame() == 1);
      CHECK(b.accumulator() == 0.0);
      CHECK(b.setSpeedIndex(1));
      CHECK(b.advance(eng) == 0);
      CHECK(eng.currentFrame() == 1);
      CHECK(b.advance(eng) == 1);
      CHECK(eng.currentFrame() == 2);
    }
    // Paused/stopped UI frames accrue no debt; stop clears the accumulator.
    {
      SessionEngine eng;
      MockDevice dev;
      eng.setActiveDevice(&dev);
      eng.setTotalFrames(100);
      TransportBar b;
      CHECK(b.setSpeedIndex(0));
      eng.play();
      CHECK(b.advance(eng) == 0);  // Accumulator now 0.25.
      eng.pause();
      for (int i = 0; i < 5; ++i) CHECK(b.advance(eng) == 0);
      CHECK(eng.currentFrame() == 0);
      eng.play();  // Resume: only the pre-pause 0.25 carries over.
      CHECK(b.advance(eng) == 0);
      CHECK(b.advance(eng) == 0);
      CHECK(b.advance(eng) == 1);
      CHECK(eng.currentFrame() == 1);
      CHECK(b.setSpeedIndex(0));
      CHECK(b.advance(eng) == 0);
      b.stop(eng);
      CHECK(eng.isStopped());
      CHECK(eng.currentFrame() == 0);
      CHECK(b.accumulator() == 0.0);
      CHECK(b.advance(eng) == 0);  // Stopped: no-op.
    }
    std::printf("PASS speed multipliers\n");
  }

  // --- 4. Loop markers -------------------------------------------------------
  {
    // Set [A] stages, Set [B] enables; ticks wrap loop_end -> loop_start.
    SessionEngine eng;
    MockDevice dev;
    eng.setActiveDevice(&dev);
    eng.setTotalFrames(100);
    TransportBar bar;
    bar.seekToFrame(eng, 10);
    bar.setLoopStartToCurrent(eng);
    CHECK(!eng.loopEnabled());  // Staged only: [10, 0].
    CHECK(eng.loopStart() == 10);
    bar.seekToFrame(eng, 20);
    bar.setLoopEndToCurrent(eng);
    CHECK(eng.loopEnabled());
    CHECK(eng.loopStart() == 10);
    CHECK(eng.loopEnd() == 20);
    bar.seekToFrame(eng, 19);
    eng.play();
    eng.tick();  // Frame 19 -> 20 -> wrap to 10.
    CHECK(eng.currentFrame() == 10);
    // advance() honours the same wrap (8,10) window.
    eng.setLoop(8, 10);
    bar.seekToFrame(eng, 8);
    bar.setSpeedIndex(2);
    CHECK(bar.advance(eng) == 1);
    CHECK(eng.currentFrame() == 9);
    CHECK(bar.advance(eng) == 1);
    CHECK(eng.currentFrame() == 8);
    // Disable stashes; re-enable restores.
    eng.setLoop(10, 20);
    bar.setLoopEnabled(eng, false);
    CHECK(!eng.loopEnabled());
    bar.setLoopEnabled(eng, true);
    CHECK(eng.loopEnabled());
    CHECK(eng.loopStart() == 10);
    CHECK(eng.loopEnd() == 20);
    bar.toggleLoop(eng);
    CHECK(!eng.loopEnabled());
    bar.toggleLoop(eng);
    CHECK(eng.loopEnabled());
    CHECK(eng.loopStart() == 10);
    // Clear drops the stash: re-enable falls back to [0, total].
    bar.clearLoop(eng);
    CHECK(!eng.loopEnabled());
    bar.setLoopEnabled(eng, true);
    CHECK(eng.loopEnabled());
    CHECK(eng.loopStart() == 0);
    CHECK(eng.loopEnd() == 100);
    // No-op when already in the requested state.
    bar.setLoopEnabled(eng, true);
    CHECK(eng.loopStart() == 0);
    CHECK(eng.loopEnd() == 100);
    std::printf("PASS loop markers\n");
  }

  // --- 5a. Shortcuts: Space / Home / Left / Right -----------------------------
  {
    SessionEngine eng;
    MockDevice dev;
    eng.setActiveDevice(&dev);
    eng.setTotalFrames(100);
    TransportBar bar;
    CHECK(bar.handleKey(eng, nullptr, TransportKey::PlayPause));
    CHECK(eng.isPlaying());
    CHECK(bar.handleKey(eng, nullptr, TransportKey::PlayPause));
    CHECK(eng.isPaused());
    CHECK(bar.handleKey(eng, nullptr, TransportKey::PlayPause));
    CHECK(eng.isPlaying());
    CHECK(!eng.isPaused());
    bar.seekToFrame(eng, 40);
    CHECK(bar.handleKey(eng, nullptr, TransportKey::StepForward));
    CHECK(eng.currentFrame() == 41);
    CHECK(bar.handleKey(eng, nullptr, TransportKey::StepBack));
    CHECK(eng.currentFrame() == 40);
    CHECK(bar.handleKey(eng, nullptr, TransportKey::Rewind));
    CHECK(eng.currentFrame() == 0);
    CHECK(bar.handleKey(eng, nullptr, TransportKey::StepBack));  // Clamped.
    CHECK(eng.currentFrame() == 0);
    bar.seekToFrame(eng, 100);
    CHECK(bar.handleKey(eng, nullptr, TransportKey::StepForward));  // Clamped.
    CHECK(eng.currentFrame() == 100);
    // Single step works while paused (frame moves, engine stays paused).
    bar.seekToFrame(eng, 10);
    eng.pause();
    CHECK(bar.handleKey(eng, nullptr, TransportKey::StepForward));
    CHECK(eng.currentFrame() == 11);
    CHECK(eng.isPaused());
    std::printf("PASS shortcuts transport\n");
  }

  // --- 5b. Shortcut: E toggles the enhancement overlay -----------------------
  {
    SessionEngine eng;
    MockMidi midi;
    CHECK(midi.init());
    eng.setActiveDevice(&midi);
    eng.setTotalFrames(50);
    eng.addNote(0, 0, 50, 100, 5);
    audio_dbg::SimNoteEvent eon;
    eon.frame = 0;
    eon.channel = 5;
    eon.note = 80;
    eon.velocity = 90;
    eon.duration_frames = 5;
    eon.is_note_on = true;
    audio_dbg::SimNoteEvent eoff;
    eoff.frame = 5;
    eoff.channel = 5;
    eoff.note = 80;
    eoff.velocity = 0;
    eoff.duration_frames = 0;
    eoff.is_note_on = false;
    eng.setEnhancementEvents({eon, eoff});
    TransportBar bar;
    CHECK(eng.eventCount() == 4);
    CHECK(bar.handleKey(eng, nullptr, TransportKey::ToggleEnhancement));
    CHECK(!eng.isEnhancementEnabled());
    CHECK(eng.eventCount() == 2);
    CHECK(bar.handleKey(eng, nullptr, TransportKey::ToggleEnhancement));
    CHECK(eng.isEnhancementEnabled());
    CHECK(eng.eventCount() == 4);
    midi.shutdown();
    std::printf("PASS shortcut enhancement toggle\n");
  }

  // --- 5c. Shortcut: Tab toggles slot A/B with position locking --------------
  {
    SessionEngine eng;
    MockMidi midi;
    CHECK(midi.init());
    eng.setActiveDevice(&midi);
    eng.setTotalFrames(100);
    eng.addNote(2, 0, 60, 100, 10);
    eng.setComparisonSlot(ComparisonSlot::B);
    eng.addNote(3, 1, 72, 100, 10);
    eng.setComparisonSlot(ComparisonSlot::A);
    TransportBar bar;
    eng.play();
    eng.tick();
    eng.tick();
    eng.tick();  // Frame 2: A note-on.
    CHECK(eng.currentFrame() == 3);
    CHECK(midi.isNoteSounding(0, 60));
    CHECK(bar.handleKey(eng, nullptr, TransportKey::ToggleSlot));
    CHECK(eng.activeSlot() == ComparisonSlot::B);
    CHECK(eng.currentFrame() == 3);
    CHECK(!midi.isNoteSounding(0, 60));
    CHECK(bar.handleKey(eng, nullptr, TransportKey::ToggleSlot));
    CHECK(eng.activeSlot() == ComparisonSlot::A);
    CHECK(eng.currentFrame() == 3);
    midi.shutdown();
    std::printf("PASS shortcut slot toggle\n");
  }

  // --- 5d. Revisions: null manager unhandled; temp-dir walk verified ---------
  {
    SessionEngine eng;
    TransportBar bar;
    CHECK(!bar.handleKey(eng, nullptr, TransportKey::RevisionPrev));
    CHECK(!bar.handleKey(eng, nullptr, TransportKey::RevisionNext));
    EnhancementManager bare("");
    CHECK(!bar.stepRevision(&bare, 1));

    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / "pkmn-transport-rev-test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp / "enh", ec);
    EnhancementManager mgr((tmp / "enh").string(), (tmp / "rev").string(),
                           tmp.string());
    mgr.watchSong("Music_TestSong");
    auto s1 = mgr.saveSnapshot("Music_TestSong", "rev-one", "first");
    CHECK(s1.first == 1);
    auto s2 = mgr.saveSnapshot("Music_TestSong", "rev-two", "second");
    CHECK(s2.first == 2);
    CHECK(mgr.listRevisions("Music_TestSong").size() == 2);
    // Untracked forward step enters at the oldest revision.
    CHECK(bar.handleKey(eng, &mgr, TransportKey::RevisionNext));
    CHECK(bar.revisionIndex() == 0);
    CHECK(mgr.loadYamlFile("Music_TestSong").second == "rev-one");
    CHECK(bar.handleKey(eng, &mgr, TransportKey::RevisionNext));
    CHECK(bar.revisionIndex() == 1);
    CHECK(mgr.loadYamlFile("Music_TestSong").second == "rev-two");
    // Past the newest: unhandled, cursor stays.
    CHECK(!bar.handleKey(eng, &mgr, TransportKey::RevisionNext));
    CHECK(bar.revisionIndex() == 1);
    CHECK(bar.handleKey(eng, &mgr, TransportKey::RevisionPrev));
    CHECK(bar.revisionIndex() == 0);
    CHECK(mgr.loadYamlFile("Music_TestSong").second == "rev-one");
    CHECK(!bar.handleKey(eng, &mgr, TransportKey::RevisionPrev));
    CHECK(bar.revisionIndex() == 0);
    // Untracked backward step enters at the newest revision.
    bar.setRevisionIndex(-1);
    CHECK(bar.handleKey(eng, &mgr, TransportKey::RevisionPrev));
    CHECK(bar.revisionIndex() == 1);
    CHECK(mgr.loadYamlFile("Music_TestSong").second == "rev-two");
    fs::remove_all(tmp, ec);
    std::printf("PASS revision stepping\n");
  }

  // --- 5e. Baseline loader drops baked enhancement tracks ------------------
  // A type-1 .mid with a conductor track (loop markers), a "GB ch1" base
  // track and an "enh ... tier2" track: the whole-file parse keeps both
  // note layers (soundtrack export stays complete), while the baseline
  // parse (drop_enhancement_tracks) keeps the base note and the loop
  // markers but drops the baked enhancement note. Runtime enhancement is
  // the live bridge's job alone (per-device tier target); without this
  // every enhancement note sounds twice, and tier-2/3 leak onto OPL3.
  {
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / "pkmn-baseline-drop-test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    const std::string path = (tmp / "t.mid").string();
    // Minimal SMF type-1, division 60 (1 tick = 1 frame at tempo 1e6).
    std::vector<std::uint8_t> bytes;
    auto push = [&](std::initializer_list<std::uint8_t> xs) {
      bytes.insert(bytes.end(), xs);
    };
    auto track = [&](const std::vector<std::uint8_t>& ev) {
      push({'M', 'T', 'r', 'k'});
      const auto n = ev.size();
      push({0, 0, (std::uint8_t)(n >> 8), (std::uint8_t)(n & 0xFF)});
      bytes.insert(bytes.end(), ev.begin(), ev.end());
    };
    auto name = [](const char* s) {
      std::vector<std::uint8_t> ev = {0x00, 0xFF, 0x03};
      std::string t(s);
      ev.push_back((std::uint8_t)t.size());
      for (char c : t) ev.push_back((std::uint8_t)c);
      return ev;
    };
    push({'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 3, 0, 60});
    // Conductor: tempo 1e6, name, loopStart at tick 0, loopEnd at tick 120.
    {
      std::vector<std::uint8_t> ev = {0x00, 0xFF, 0x51, 0x03,
                                      0x0F, 0x42, 0x40};
      auto n = name("T");
      ev.insert(ev.end(), n.begin(), n.end());
      const char* s = "loopStart";
      ev.insert(ev.end(), {0x00, 0xFF, 0x06, 0x09});
      for (int i = 0; s[i]; ++i) ev.push_back((std::uint8_t)s[i]);
      const char* m = "loopEnd";
      ev.push_back(120);  // delta 120 (< 128: single-byte VLQ).
      ev.insert(ev.end(), {0xFF, 0x06, 0x07});
      for (int i = 0; m[i]; ++i) ev.push_back((std::uint8_t)m[i]);
      ev.insert(ev.end(), {0x00, 0xFF, 0x2F, 0x00});
      track(ev);
    }
    // Base: "GB ch1", note 60 on ch1 tick 0, off tick 60.
    {
      auto ev = name("GB ch1");
      ev.insert(ev.end(), {0x00, 0x90 | 1, 60, 100});
      ev.insert(ev.end(), {60, 0x80 | 1, 60, 64});
      ev.insert(ev.end(), {0x00, 0xFF, 0x2F, 0x00});
      track(ev);
    }
    // Baked enhancement: "enh foo tier2", note 40 on ch6 tick 0, off tick 30.
    {
      auto ev = name("enh foo tier2");
      ev.insert(ev.end(), {0x00, 0x90 | 6, 40, 90});
      ev.insert(ev.end(), {30, 0x80 | 6, 40, 64});
      ev.insert(ev.end(), {0x00, 0xFF, 0x2F, 0x00});
      track(ev);
    }
    {
      std::ofstream f(path, std::ios::binary);
      f.write(reinterpret_cast<const char*>(bytes.data()),
              (std::streamsize)bytes.size());
    }
    EnhancementManager mgr("");
    auto whole = mgr.parseMidiFile(path);
    auto base = mgr.parseMidiFile(path, true);
    auto hasNote = [](const std::vector<audio_dbg::SimNoteEvent>& evs, int ch,
                      int note) {
      for (const auto& e : evs) {
        if (e.is_note_on && e.type == audio_dbg::SimEventType::Note &&
            e.channel == ch && e.note == note)
          return true;
      }
      return false;
    };
    CHECK(hasNote(whole.notes, 1, 60));
    CHECK(hasNote(whole.notes, 6, 40));
    CHECK(whole.loop_end_frame == 120);
    CHECK(hasNote(base.notes, 1, 60));
    CHECK(!hasNote(base.notes, 6, 40));
    CHECK(base.loop_end_frame == 120);
    fs::remove_all(tmp, ec);
    std::printf("PASS baseline drops baked enhancement\n");
  }

  if (g_failures == 0) {
    std::printf("ALL PASS (%d checks)\n", g_checks);
    return 0;
  }
  std::printf("%d/%d checks FAILED\n", g_failures, g_checks);
  return 1;
}
