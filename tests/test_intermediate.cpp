// Stage 2.6: Acceptance test for the Tier-2 intermediate bases.
// Mocks MidiDevice/MidiTab, FmDevice/FmTab, PsgDevice/PsgTab and verifies:
//   1. polymorphic dispatch through SoundDevice*/DeviceTab*,
//   2. pre-synth MIDI muting (muted Note-On allocates no voice),
//   3. FM voice configuration + dormancy,
//   4. PSG duty/wave/LFSR state + wake-up,
//   5. lazy channel dormant skipping (push stores nothing until wake;
//      empty handleCommand wakes nothing).
// Exits 0 with ALL PASS on success, 1 on any failure. No GUI needed.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "devices/fm_device.h"
#include "devices/midi_device.h"
#include "devices/psg_device.h"
#include "tabs/device_tab.h"
#include "tabs/fm_tab.h"
#include "tabs/midi_tab.h"
#include "tabs/psg_tab.h"

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

// --- Mocks ---------------------------------------------------------------

class MockMidiDevice : public audio_dbg::MidiDevice {
 public:
  MockMidiDevice() : MidiDevice(1, "MockMidi", 16) {}
  bool init() override { return true; }
  void shutdown() override {}
  void reset() override {}
  void render(float* buf, std::size_t frames) override {
    ++render_calls;
    if (buf != nullptr) {
      for (std::size_t i = 0; i < frames; ++i) buf[i] = 0.0f;
    }
  }
  void renderPerChannel(float** bufs, std::size_t frames) override {
    ++render_ch_calls;
    (void)bufs;
    (void)frames;
  }
  void dispatchNoteOn(int ch, int note, int velocity) override {
    ++dispatch_on;
    last_ch = ch;
    last_note = note;
    last_vel = velocity;
  }
  void dispatchNoteOff(int ch, int note) override {
    ++dispatch_off;
    last_off_ch = ch;
    last_off_note = note;
  }
  int render_calls = 0;
  int render_ch_calls = 0;
  int dispatch_on = 0;
  int dispatch_off = 0;
  int last_ch = -1;
  int last_note = -1;
  int last_vel = -1;
  int last_off_ch = -1;
  int last_off_note = -1;
};

class MockMidiTab : public audio_dbg::MidiTab {
 public:
  MockMidiTab() : MidiTab(1, "MockMidiTab") {}
  const char* programName(int program) const override {
    (void)program;
    return "MockProg";
  }
  void drawChannelStrips(const audio_dbg::DeviceSnapshot& s) override {
    ++strips_calls;
    MidiTab::drawChannelStrips(s);  // Safe headless (context guard).
  }
  std::size_t testWaveSize(int ch) const { return waveSize(ch); }
  int strips_calls = 0;
};

class MockFmDevice : public audio_dbg::FmDevice {
 public:
  explicit MockFmDevice(int voices = 8) : FmDevice(2, "MockFm", voices) {}
  bool init() override { return true; }
  void shutdown() override {}
  void reset() override {}
  void render(float* buf, std::size_t frames) override {
    ++render_calls;
    if (buf != nullptr) {
      for (std::size_t i = 0; i < frames; ++i) buf[i] = 0.0f;
    }
  }
  void renderPerChannel(float** bufs, std::size_t frames) override {
    ++render_ch_calls;
    (void)bufs;
    (void)frames;
  }
  void configureOperator(int voice, bool carrier,
                         const audio_dbg::FmOperatorParams& p) override {
    ++op_calls;
    last_voice = voice;
    last_carrier = carrier;
    last_mult = p.mult;
  }
  void applyFrequency(int voice, int fnum, int block) override {
    ++freq_calls;
    last_fvoice = voice;
    last_fnum = fnum;
    last_block = block;
  }
  void applyKeyOn(int voice) override {
    ++keyon_calls;
    last_keyon = voice;
  }
  void applyKeyOff(int voice) override {
    ++keyoff_calls;
    last_keyoff = voice;
  }
  int render_calls = 0;
  int render_ch_calls = 0;
  int op_calls = 0;
  int freq_calls = 0;
  int keyon_calls = 0;
  int keyoff_calls = 0;
  int last_voice = -1;
  bool last_carrier = false;
  int last_mult = 0;
  int last_fvoice = -1;
  int last_fnum = -1;
  int last_block = -1;
  int last_keyon = -1;
  int last_keyoff = -1;
};

class MockFmTab : public audio_dbg::FmTab {
 public:
  MockFmTab() : FmTab(2, "MockFmTab") {}
  const char* voiceLabel(int voice) const override {
    (void)voice;
    return "MockVoice";
  }
  void drawChannelStrips(const audio_dbg::DeviceSnapshot& s) override {
    ++strips_calls;
    FmTab::drawChannelStrips(s);
  }
  std::size_t testWaveSize(int ch) const { return waveSize(ch); }
  int strips_calls = 0;
};

class MockPsgDevice : public audio_dbg::PsgDevice {
 public:
  MockPsgDevice() : PsgDevice(3, "MockPsg", 4) {}
  bool init() override { return true; }
  void shutdown() override {}
  void reset() override {}
  void render(float* buf, std::size_t frames) override {
    ++render_calls;
    if (buf != nullptr) {
      for (std::size_t i = 0; i < frames; ++i) buf[i] = 0.0f;
    }
  }
  void renderPerChannel(float** bufs, std::size_t frames) override {
    ++render_ch_calls;
    (void)bufs;
    (void)frames;
  }
  void applyRegister(int ch) override {
    ++apply_calls;
    last_ch = ch;
  }
  int render_calls = 0;
  int render_ch_calls = 0;
  int apply_calls = 0;
  int last_ch = -1;
};

class MockPsgTab : public audio_dbg::PsgTab {
 public:
  MockPsgTab() : PsgTab(3, "MockPsgTab") {}
  const char* envelopeName(int envelope) const override {
    (void)envelope;
    return "MockEnv";
  }
  void drawChannelStrips(const audio_dbg::DeviceSnapshot& s) override {
    ++strips_calls;
    PsgTab::drawChannelStrips(s);
  }
  std::size_t testWaveSize(int ch) const { return waveSize(ch); }
  int strips_calls = 0;
};

}  // namespace

int main() {
  using namespace audio_dbg;

  // --- 1. Polymorphic dispatch -------------------------------------------
  {
    MockMidiDevice midi;
    MockFmDevice fm(8);
    MockPsgDevice psg;
    SoundDevice* devs[3] = {&midi, &fm, &psg};
    float buf[16] = {0};
    for (SoundDevice* d : devs) {
      CHECK(d->init());
      d->tick(7);
      CHECK(d->snapshot().frame == 7u);
      d->render(buf, 16);
      d->renderPerChannel(nullptr, 0);
      d->reset();
      d->shutdown();
    }
    CHECK(midi.render_calls == 1);
    CHECK(fm.render_calls == 1);
    CHECK(psg.render_calls == 1);

    MockMidiTab mtab;
    MockFmTab ftab;
    MockPsgTab ptab;
    DeviceTab* tabs[3] = {&mtab, &ftab, &ptab};
    DeviceSnapshot s = midi.snapshot();
    for (DeviceTab* t : tabs) {
      t->drawChannelStrips(s);  // Virtual dispatch -> mock counter.
      t->drawDetail(s);
      t->drawTracker(s, nullptr);
      t->onMuteClick(0);  // No device attached: must be a safe no-op.
    }
    CHECK(mtab.strips_calls == 1);
    CHECK(ftab.strips_calls == 1);
    CHECK(ptab.strips_calls == 1);
    std::printf("PASS polymorphic dispatch\n");
  }

  // --- 2. Pre-synth MIDI muting ------------------------------------------
  {
    MockMidiDevice midi;
    CHECK(midi.isDormant(3));
    midi.setMute(3, true);
    const bool suppressed = midi.noteOn(3, 60, 100);
    CHECK(!suppressed);                    // Filtered: no voice allocated.
    CHECK(midi.dispatch_on == 0);
    CHECK(!midi.isNoteSounding(3, 60));
    CHECK(midi.noteVelocity(3, 60) == 0);
    CHECK(!midi.isDormant(3));             // …but the strip still wakes.
    CHECK(!midi.noteHistory().empty());
    CHECK(!midi.noteHistory().back().sounding);

    midi.setMute(3, false);
    CHECK(midi.noteOn(3, 60, 100));
    CHECK(midi.dispatch_on == 1);
    CHECK(midi.last_ch == 3 && midi.last_note == 60 && midi.last_vel == 100);
    CHECK(midi.isNoteSounding(3, 60));
    CHECK(midi.noteVelocity(3, 60) == 100);
    CHECK(midi.activeNoteCount(3) == 1);
    midi.noteOff(3, 60);
    CHECK(!midi.isNoteSounding(3, 60));
    CHECK(midi.dispatch_off == 1);

    // Solo-exclusion also filters.
    midi.setSolo(5, true);
    CHECK(!midi.noteOn(6, 64, 90));
    CHECK(midi.dispatch_on == 1);  // Unchanged.
    CHECK(midi.noteOn(5, 64, 90));
    CHECK(midi.dispatch_on == 2);
    midi.setSolo(5, false);

    // Program / pitch / CC tracking + wake-up.
    midi.setProgram(0, 40);
    CHECK(midi.program(0) == 40);
    midi.setPitchBend(0, 9000);
    CHECK(midi.pitchBend(0) == 9000);
    midi.setControlChange(0, 7, 80);
    CHECK(midi.controlChange(0, 7) == 80);
    CHECK(!midi.isDormant(0));

    // Out-of-range safety.
    CHECK(!midi.noteOn(-1, 60, 100));
    CHECK(!midi.isNoteSounding(-1, 60));
    CHECK(midi.shouldFilterNoteOn(99));

    // Tab mute click toggles the device through the attached pointer.
    MockMidiTab tab;
    tab.setDevice(&midi);
    midi.setMute(1, false);
    tab.onMuteClick(1);
    CHECK(midi.snapshot().channels[1].muted);
    tab.onMuteClick(1);
    CHECK(!midi.snapshot().channels[1].muted);
    // stripState reflects audibility.
    midi.setMute(1, true);
    MidiTab::StripState st = tab.stripState(midi.snapshot(), 1);
    CHECK(st.muted && !st.audible);

    // velocityColor grading: vel=100 -> RGB(200,55,0).
    float r = 0.0f, g = 0.0f, b = 0.0f;
    DeviceTab::velocityColor(100, &r, &g, &b);
    CHECK(r > 0.78f && r < 0.79f);
    CHECK(g > 0.21f && g < 0.22f);
    CHECK(b == 0.0f);
    // Null draw-list helper: must not crash.
    MidiTab::drawNoteBar(nullptr, ImVec2(0, 0), 10.0f, 4.0f, 100, true);

    std::printf("PASS pre-synth MIDI muting\n");
  }

  // --- 3. FM voice configuration -----------------------------------------
  {
    MockFmDevice fm(8);
    CHECK(fm.voiceCount() == 8);
    CHECK(fm.isVoiceDormant(2));
    CHECK(fm.isDormant(2));

    FmOperatorParams p;
    p.mult = 3;
    p.ar = 15;
    p.tl = 20;
    p.ws = 1;
    fm.writeOperator(2, false, p);
    CHECK(!fm.isVoiceDormant(2));  // Key path wakes on config.
    CHECK(!fm.isDormant(2));
    CHECK(fm.op_calls == 1);
    CHECK(fm.last_voice == 2 && !fm.last_carrier && fm.last_mult == 3);
    CHECK(fm.voice(2).mod.mult == 3);
    CHECK(fm.voice(2).mod.tl == 20);

    fm.writeFrequency(2, 512, 4);
    CHECK(fm.freq_calls == 1);
    CHECK(fm.voice(2).fnum == 512 && fm.voice(2).block == 4);
    fm.writeFeedback(2, 5, 1);
    CHECK(fm.voice(2).feedback == 5 && fm.voice(2).connection == 1);

    fm.voiceKeyOn(2);
    CHECK(fm.voice(2).keyed_on);
    CHECK(fm.voice(2).env_vol == 1.0f);
    CHECK(fm.keyon_calls == 1 && fm.last_keyon == 2);
    fm.voiceKeyOff(2);
    CHECK(!fm.voice(2).keyed_on);
    CHECK(fm.keyoff_calls == 1);

    // Untouched voice stays dormant.
    CHECK(fm.isVoiceDormant(3));

    // Voice count clamps to kMaxVoices.
    MockFmDevice big(64);
    CHECK(big.voiceCount() == FmDevice::kMaxVoices);
    // Out-of-range writes are safe no-ops.
    fm.writeOperator(99, true, p);
    fm.voiceKeyOn(-1);

    // Tab voice view + envelope names.
    MockFmTab tab;
    tab.setDevice(&fm);
    FmTab::VoiceView vv = tab.voiceView(2);
    CHECK(!vv.dormant && !vv.keyed_on && vv.connection == 1);
    CHECK(std::strcmp(FmTab::envelopePhaseName(true, 1.0f), "ATTACK") == 0);
    CHECK(std::strcmp(FmTab::envelopePhaseName(true, 0.5f), "SUSTAIN") == 0);
    CHECK(std::strcmp(FmTab::envelopePhaseName(false, 0.0f), "RELEASE") == 0);
    FmTab::drawAlgorithm(nullptr, ImVec2(0, 0), 64.0f, 0, 3);  // No crash.

    std::printf("PASS FM voice configuration\n");
  }

  // --- 4. PSG duty/wave state --------------------------------------------
  {
    MockPsgDevice psg;
    CHECK(psg.isDormant(0));
    CHECK(psg.psgChannel(0).is_dormant);

    psg.setDuty(0, 2);
    CHECK(psg.channelDuty(0) == 2);
    psg.setDuty(1, 7);  // Masked to 0..3.
    CHECK(psg.channelDuty(1) == 3);

    std::array<std::uint8_t, 32> ram{};
    for (int i = 0; i < 32; ++i) ram[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i % 16);
    psg.setWaveRam(2, ram);
    CHECK(psg.psgChannel(2).wave_ram[10] == 10);
    psg.setWaveEntry(2, 5, 0x1A);  // Nibble-masked.
    CHECK(psg.psgChannel(2).wave_ram[5] == 0x0A);

    psg.setLfsrWidth(3, 7);
    CHECK(psg.channelLfsrWidth(3) == 7);
    psg.setLfsrWidth(3, 9);  // Invalid: ignored.
    CHECK(psg.channelLfsrWidth(3) == 7);

    psg.setFrequency(0, 440.0);
    CHECK(psg.channelFrequency(0) == 440.0);
    psg.setVolume(0, 12);
    CHECK(psg.psgChannel(0).volume == 12);

    psg.activateChannel(0);
    CHECK(psg.psgChannel(0).active);
    CHECK(!psg.psgChannel(0).is_dormant);
    CHECK(!psg.isDormant(0));
    psg.deactivateChannel(0);
    CHECK(!psg.psgChannel(0).active);
    CHECK(psg.apply_calls > 0);

    // Default GB-APU type layout.
    CHECK(psg.channelType(0) == PsgChannelType::PULSE1);
    CHECK(psg.channelType(1) == PsgChannelType::PULSE2);
    CHECK(psg.channelType(2) == PsgChannelType::WAVE);
    CHECK(psg.channelType(3) == PsgChannelType::NOISE);

    // Out-of-range setters are safe no-ops.
    psg.setDuty(99, 1);
    psg.setWaveEntry(2, 99, 5);

    // Tab helpers.
    CHECK(std::strcmp(PsgTab::channelTypeName(PsgChannelType::WAVE), "Wave") == 0);
    CHECK(PsgTab::dutyFraction(0) == 0.125f);
    CHECK(PsgTab::dutyFraction(1) == 0.25f);
    CHECK(PsgTab::dutyFraction(2) == 0.5f);
    CHECK(PsgTab::dutyFraction(3) == 0.75f);
    PsgTab::drawDutyBar(nullptr, ImVec2(0, 0), 64.0f, 8.0f, 2);
    PsgTab::drawWaveRam(nullptr, ImVec2(0, 0), 64.0f, 16.0f, ram);

    std::printf("PASS PSG duty/wave state\n");
  }

  // --- 5. Lazy dormant skipping ------------------------------------------
  {
    // Device-level: dormant push stores nothing; post-wake stores all.
    MockFmDevice fm(4);
    const float samples[8] = {0.1f, 0.2f, 0.3f, 0.4f,
                              0.5f, 0.6f, 0.7f, 0.8f};
    CHECK(fm.isDormant(1));
    fm.pushWaveform(1, samples, 8);
    CHECK(fm.waveSize(1) == 0u);
    fm.wakeVoice(1);
    fm.pushWaveform(1, samples, 8);
    CHECK(fm.waveSize(1) == 8u);

    // Empty handleCommand wakes nothing; non-empty wakes payload[0] % n.
    MockMidiDevice midi;
    const std::uint8_t empty_payload[1] = {0};
    midi.handleCommand(0x90, nullptr, 0);
    midi.handleCommand(0x90, empty_payload, 0);
    CHECK(midi.isDormant(0) && midi.isDormant(5));
    const std::uint8_t wake5[1] = {5};
    midi.handleCommand(0x90, wake5, 1);
    CHECK(!midi.isDormant(5));
    CHECK(midi.isDormant(6));

    MockPsgDevice psg;
    psg.handleCommand(0x01, nullptr, 0);
    CHECK(psg.isDormant(2));
    const std::uint8_t wake2[1] = {2};
    psg.handleCommand(0x01, wake2, 1);
    CHECK(!psg.isDormant(2));

    // Tab-level: dormant push stores nothing; post-wake stores.
    MockMidiTab mtab;
    MockMidiDevice mdev;
    mtab.setDevice(&mdev);
    CHECK(mdev.isDormant(4));
    mtab.pushWaveformData(4, samples, 8);
    CHECK(mtab.testWaveSize(4) == 0u);
    mdev.noteOn(4, 60, 100);
    mtab.pushWaveformData(4, samples, 8);
    CHECK(mtab.testWaveSize(4) == 8u);

    MockFmTab ftab;
    ftab.setDevice(&fm);
    CHECK(fm.isVoiceDormant(3));
    ftab.pushWaveformData(3, samples, 8);
    CHECK(ftab.testWaveSize(3) == 0u);
    fm.voiceKeyOn(3);
    ftab.pushWaveformData(3, samples, 8);
    CHECK(ftab.testWaveSize(3) == 8u);

    MockPsgTab ptab;
    ptab.setDevice(&psg);
    // Channel 3 was never woken on this psg instance path: fresh check.
    MockPsgDevice psg2;
    ptab.setDevice(&psg2);
    ptab.pushWaveformData(3, samples, 8);
    CHECK(ptab.testWaveSize(3) == 0u);
    psg2.activateChannel(3);
    ptab.pushWaveformData(3, samples, 8);
    CHECK(ptab.testWaveSize(3) == 8u);

    std::printf("PASS lazy dormant skipping\n");
  }

  if (g_failures == 0) {
    std::printf("ALL PASS (%d checks)\n", g_checks);
    return 0;
  }
  std::printf("%d/%d checks FAILED\n", g_failures, g_checks);
  return 1;
}
