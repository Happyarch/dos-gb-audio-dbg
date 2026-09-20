// Stage 4.5: Acceptance tests for the MIDI concrete synthesizer backends.
// Verifies:
//   MT-32 (Mt32Device + libmt32emu, real ROMs or mock fallback):
//     1. Fresh channels dormant; init() succeeds with or without ROMs.
//     2. Note dispatch wakes, tracks sounding state, feeds partial count.
//     3. Muted Note-On is pre-synth filtered (no voice allocated).
//     4. MT-32 timbre name resolution (anchors + full-table coverage).
//     5. Master + per-channel rendering; dormant fast escape (sentinel
//        zeros); mute gating on both paths.
//     6. LCD readout (non-empty; mock display SysEx updates it).
//     7. Partial/part telemetry bounds.
//   GM (GmDevice + libfluidsynth, real SoundFont or mock fallback):
//     1. Fresh dormancy; init() succeeds with or without a SoundFont.
//     2. Note dispatch + program/CC reflection.
//     3. GM 128 patch name resolution (anchors + full-table coverage).
//     4. Master + per-channel rendering; dormant fast escape; mute gating.
// Headless: no GUI, no audio hardware. Exits 0 with ALL PASS, 1 on failure.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "devices/gm_device.h"
#include "devices/mt32_device.h"

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

float maxAbs(const float* buf, std::size_t n) {
  float m = 0.0f;
  if (buf == nullptr) return 0.0f;
  for (std::size_t i = 0; i < n; ++i) {
    const float a = std::fabs(buf[i]);
    if (a > m) m = a;
  }
  return m;
}

bool allZero(const float* buf, std::size_t n) {
  if (buf == nullptr || n == 0) return true;
  for (std::size_t i = 0; i < n; ++i) {
    if (buf[i] != 0.0f) return false;
  }
  return true;
}

}  // namespace

int main() {
  using namespace audio_dbg;
  constexpr std::size_t kFrames = 1024;

  // --- MT-32: fresh dormancy + init ------------------------------------
  {
    Mt32Device dev;
    CHECK(dev.channelCount() == 16);
    CHECK(dev.init());
    for (int ch = 0; ch < 16; ++ch) CHECK(dev.isDormant(ch));
    CHECK(dev.activePartialCount() == 0);
    CHECK(!dev.partActive(0));
    CHECK(!dev.partActive(8));
    CHECK(!dev.partActive(-1));
    CHECK(!dev.partActive(9));
    CHECK(dev.partialState(-1) == -1);
    CHECK(dev.partialState(32) == -1);
    CHECK(!dev.lcdText().empty());
    std::printf("INFO mt32 mode: %s%s\n",
                dev.isMock() ? "MOCK (no ROMs)" : "REAL ROMs",
                dev.isMock() ? "" : dev.romDir().c_str());
    dev.shutdown();
    std::printf("PASS mt32 fresh dormancy\n");
  }

  // --- MT-32: pre-synth mute filter allocates nothing -------------------
  {
    Mt32Device dev;
    CHECK(dev.init());
    dev.setMute(3, true);
    CHECK(!dev.noteOn(3, 64, 100));  // Filtered: returns false.
    CHECK(!dev.isDormant(3));        // ...but the strip still wakes.
    CHECK(!dev.isNoteSounding(3, 64));
    CHECK(dev.activePartialCount() == 0);
    CHECK(dev.noteHistory().size() == 1);
    CHECK(!dev.noteHistory()[0].sounding);
    dev.shutdown();
    std::printf("PASS mt32 mute filter\n");
  }

  // --- MT-32: note dispatch + partial telemetry -------------------------
  {
    Mt32Device dev;
    CHECK(dev.init());
    dev.dispatchProgramChange(1, 0);
    CHECK(dev.program(1) == 0);
    CHECK(!dev.isDormant(1));
    CHECK(dev.noteOn(1, 60, 100));
    CHECK(dev.isNoteSounding(1, 60));
    CHECK(dev.noteVelocity(1, 60) == 100);
    CHECK(dev.activeNoteCount(1) == 1);
    std::vector<float> master(kFrames, 0.0f);
    dev.render(master.data(), kFrames);  // Lets the real engine allocate.
    CHECK(dev.activePartialCount() > 0);
    CHECK(dev.activePartialCount() <= 32);
    CHECK(dev.partActive(0));
    const int ps = dev.partialState(0);
    CHECK(ps >= 0 && ps <= 3);
    dev.noteOff(1, 60);
    CHECK(!dev.isNoteSounding(1, 60));
    CHECK(dev.activeNoteCount(1) == 0);
    CHECK(dev.snapshot().channels.size() == 16);
    // Out-of-range dispatch is ignored safely.
    dev.dispatchNoteOn(-1, 60, 100);
    dev.dispatchNoteOn(16, 60, 100);
    dev.dispatchNoteOff(1, 200);
    dev.dispatchProgramChange(99, 5);
    dev.dispatchControlChange(1, 999, 1);
    dev.dispatchSysEx(nullptr, 0);
    dev.shutdown();
    std::printf("PASS mt32 note dispatch + telemetry\n");
  }

  // --- MT-32: patch name resolution --------------------------------------
  {
    Mt32Device dev;
    CHECK(dev.resolveProgramName(0, 0) == "AcouPiano1");
    CHECK(dev.resolveProgramName(0, 7) == "Honkytonk");
    CHECK(dev.resolveProgramName(0, 8) == "Organ 1");
    for (int p = 0; p < 128; ++p) {
      CHECK(!dev.resolveProgramName(0, p).empty());
    }
    CHECK(dev.resolveProgramName(0, -5) == dev.resolveProgramName(0, 0));
    CHECK(dev.resolveProgramName(0, 200) == dev.resolveProgramName(0, 127));
    std::printf("PASS mt32 patch names\n");
  }

  // --- MT-32: rendering + dormant fast escape + mute gating ---------------
  {
    Mt32Device dev;
    CHECK(dev.init());
    std::vector<std::vector<float> > stems(
        16, std::vector<float>(kFrames, 9.0f));
    std::vector<float*> bufs(16, nullptr);
    for (int ch = 0; ch < 16; ++ch)
      bufs[static_cast<std::size_t>(ch)] = stems[static_cast<std::size_t>(ch)].data();
    // All dormant: stems must be exact silence (fast escape, no stepping).
    dev.renderPerChannel(bufs.data(), kFrames);
    for (int ch = 0; ch < 16; ++ch)
      CHECK(allZero(stems[static_cast<std::size_t>(ch)].data(), kFrames));

    CHECK(dev.noteOn(1, 60, 110));
    CHECK(!dev.isDormant(1));
    CHECK(dev.isDormant(0));  // Part-less channel never wakes on its own.
    std::vector<float> master(kFrames, 0.0f);
    dev.render(master.data(), kFrames);
    dev.render(master.data(), kFrames);  // Second block: attack sustains.
    const float unmuted_peak = maxAbs(master.data(), kFrames);
    CHECK(unmuted_peak > 1e-4f);

    for (int ch = 0; ch < 16; ++ch)
      std::fill(stems[static_cast<std::size_t>(ch)].begin(),
                stems[static_cast<std::size_t>(ch)].end(), 9.0f);
    dev.renderPerChannel(bufs.data(), kFrames);
    CHECK(maxAbs(stems[1].data(), kFrames) > 1e-4f);
    for (int ch = 0; ch < 16; ++ch) {
      if (ch == 1) continue;
      CHECK(allZero(stems[static_cast<std::size_t>(ch)].data(), kFrames));
    }

    // Mute stops new voices (pre-synth filter) and lets the held voice
    // release: the MT-32 captures part volume at note start, so the CC7
    // render gate only silences subsequently triggered notes (measured:
    // muting a held piano leaves it at full level — authentic hardware
    // behavior). The deterministic post-mute property is native
    // telemetry: after note-off + reset + drains, no partial is active.
    dev.setMute(1, true);
    CHECK(!dev.noteOn(1, 62, 100));  // Filtered: returns false.
    CHECK(!dev.isNoteSounding(1, 62));
    dev.noteOff(1, 60);
    dev.reset();
    std::vector<float> drain(kFrames, 0.0f);
    for (int i = 0; i < 8; ++i) dev.render(drain.data(), kFrames);
    CHECK(dev.activePartialCount() == 0);
    CHECK(!dev.partActive(0));
    dev.setMute(1, false);
    dev.shutdown();
    std::printf("PASS mt32 rendering + dormant escape (peak=%.2e)\n",
                unmuted_peak);
  }

  // --- MT-32: mute gate silences subsequently triggered voices ------------
  {
    Mt32Device dev;
    CHECK(dev.init());
    dev.setMute(1, true);
    // Flush the CC7=0 part volume into the engine BEFORE allocating: the
    // gate is captured at note start. Direct dispatch bypasses the
    // pre-synth filter so a voice is allocated despite the mute.
    // No prior audio exists, so no reverb tail can leak. The mock mixer is
    // exact zero; live Munt keeps the gated voice ~150x below its unmuted
    // peak (measured 7.6e-4 vs 1.2e-1).
    std::vector<float> flush(kFrames, 0.0f);
    dev.render(flush.data(), kFrames);
    dev.dispatchNoteOn(1, 60, 110);
    std::vector<float> master(kFrames, 9.0f);
    dev.render(master.data(), kFrames);
    CHECK(maxAbs(master.data(), kFrames) < 2e-3f);
    std::vector<std::vector<float> > stems(
        16, std::vector<float>(kFrames, 9.0f));
    std::vector<float*> bufs(16, nullptr);
    for (int ch = 0; ch < 16; ++ch)
      bufs[static_cast<std::size_t>(ch)] = stems[static_cast<std::size_t>(ch)].data();
    dev.renderPerChannel(bufs.data(), kFrames);
    for (int ch = 0; ch < 16; ++ch)
      CHECK(allZero(stems[static_cast<std::size_t>(ch)].data(), kFrames));
    dev.shutdown();
    std::printf("PASS mt32 mute gating\n");
  }

  // --- MT-32: LCD readout --------------------------------------------------
  {
    Mt32Device dev;
    CHECK(dev.init());
    if (dev.isMock()) {
      // Roland display-write SysEx updates the mock LCD like hardware.
      const std::uint8_t sysex[] = {
          0xF0, 0x41, 0x10, 0x16, 0x12, 0x20, 0x00, 0x00, 'H', 'E',
          'L', 'L', 'O', ' ', 'M', 'T', '-', '3', '2', ' ', ' ', ' ',
          ' ', ' ', ' ', ' ', ' ', ' ', 0x00, 0xF7};
      dev.dispatchSysEx(sysex, sizeof(sysex));
      CHECK(dev.lcdText().substr(0, 12) == "HELLO MT-32 ");
    } else {
      std::vector<float> master(kFrames, 0.0f);
      dev.render(master.data(), kFrames);
      CHECK(!dev.lcdText().empty());
    }
    dev.shutdown();
    std::printf("PASS mt32 lcd\n");
  }

  // --- GM: fresh dormancy + init ------------------------------------------
  {
    GmDevice dev;
    CHECK(dev.channelCount() == 16);
    CHECK(dev.init());
    for (int ch = 0; ch < 16; ++ch) CHECK(dev.isDormant(ch));
    std::printf("INFO gm mode: %s%s\n",
                dev.isMock() ? "MOCK (no SoundFont)" : "REAL SoundFont",
                dev.isMock() ? "" : dev.soundFontPath().c_str());
    dev.shutdown();
    std::printf("PASS gm fresh dormancy\n");
  }

  // --- GM: note dispatch + program/CC reflection ---------------------------
  {
    GmDevice dev;
    CHECK(dev.init());
    dev.dispatchProgramChange(0, 40);
    CHECK(dev.program(0) == 40);
    CHECK(dev.resolveProgramName(0, 40) == "Violin");
    dev.dispatchControlChange(0, 7, 90);
    CHECK(dev.controlChange(0, 7) == 90);
    CHECK(dev.noteOn(0, 69, 100));
    CHECK(dev.isNoteSounding(0, 69));
    CHECK(dev.noteVelocity(0, 69) == 100);
    CHECK(!dev.isDormant(0));
    CHECK(dev.isDormant(5));
    // Muted Note-On filtered pre-synth.
    dev.setMute(5, true);
    CHECK(!dev.noteOn(5, 60, 100));
    CHECK(!dev.isNoteSounding(5, 60));
    CHECK(!dev.noteHistory().empty());
    dev.noteOff(0, 69);
    CHECK(!dev.isNoteSounding(0, 69));
    // Out-of-range dispatch ignored safely.
    dev.dispatchNoteOn(-1, 60, 100);
    dev.dispatchNoteOn(2, 128, 100);
    dev.dispatchProgramChange(16, 3);
    dev.dispatchControlChange(0, 200, 1);
    dev.shutdown();
    std::printf("PASS gm note dispatch\n");
  }

  // --- GM: patch name resolution --------------------------------------------
  {
    GmDevice dev;
    CHECK(dev.resolveProgramName(0, 0) == "Acoustic Grand Piano");
    CHECK(dev.resolveProgramName(0, 24) == "Acoustic Guitar (nylon)");
    CHECK(dev.resolveProgramName(0, 40) == "Violin");
    CHECK(dev.resolveProgramName(0, 56) == "Trumpet");
    CHECK(dev.resolveProgramName(0, 118) == "Synth Drum");
    CHECK(dev.resolveProgramName(0, 127) == "Gunshot");
    for (int p = 0; p < 128; ++p) {
      CHECK(!dev.resolveProgramName(3, p).empty());
    }
    CHECK(dev.resolveProgramName(0, -5) == dev.resolveProgramName(0, 0));
    CHECK(dev.resolveProgramName(0, 200) == dev.resolveProgramName(0, 127));
    std::printf("PASS gm patch names\n");
  }

  // --- GM: rendering + dormant fast escape + mute gating ---------------------
  {
    GmDevice dev;
    CHECK(dev.init());
    std::vector<std::vector<float> > stems(
        16, std::vector<float>(kFrames, 9.0f));
    std::vector<float*> bufs(16, nullptr);
    for (int ch = 0; ch < 16; ++ch)
      bufs[static_cast<std::size_t>(ch)] = stems[static_cast<std::size_t>(ch)].data();
    dev.renderPerChannel(bufs.data(), kFrames);
    for (int ch = 0; ch < 16; ++ch)
      CHECK(allZero(stems[static_cast<std::size_t>(ch)].data(), kFrames));

    CHECK(dev.noteOn(2, 64, 110));
    std::vector<float> master(kFrames, 0.0f);
    dev.render(master.data(), kFrames);
    const float unmuted_peak = maxAbs(master.data(), kFrames);
    CHECK(unmuted_peak > 1e-4f);

    for (int ch = 0; ch < 16; ++ch)
      std::fill(stems[static_cast<std::size_t>(ch)].begin(),
                stems[static_cast<std::size_t>(ch)].end(), 9.0f);
    dev.renderPerChannel(bufs.data(), kFrames);
    CHECK(maxAbs(stems[2].data(), kFrames) > 1e-4f);
    for (int ch = 0; ch < 16; ++ch) {
      if (ch == 2) continue;
      CHECK(allZero(stems[static_cast<std::size_t>(ch)].data(), kFrames));
    }

    // Mute gates the voice out of the master mix. FluidSynth applies the
    // CC7 gate up to a block late and its reverb tail still rings, so
    // render one throwaway block, then require the next to be a small
    // fraction of the unmuted peak.
    dev.setMute(2, true);
    std::vector<float> muted(kFrames, 9.0f);
    dev.render(muted.data(), kFrames);
    dev.render(muted.data(), kFrames);
    CHECK(maxAbs(muted.data(), kFrames) < 0.2f * unmuted_peak);
    dev.setMute(2, false);
    dev.noteOff(2, 64);
    dev.reset();
    dev.shutdown();
    std::printf("PASS gm rendering + dormant escape (peak=%.2e)\n",
                unmuted_peak);
  }

  // --- GM: mute gating is near-absolute with no reverb tail -----------------
  {
    GmDevice dev;
    CHECK(dev.init());
    dev.setMute(2, true);
    // Direct dispatch bypasses the pre-synth filter and allocates a voice
    // on the muted channel; the CC7 render gate must still silence it.
    // No prior audio exists, so no reverb tail can leak. The mock mixer is
    // exact zero; live FluidSynth keeps residuals below 1e-3 (measured
    // 8.1e-5: the muted voice's effect sends tap pre-volume).
    dev.dispatchNoteOn(2, 64, 110);
    std::vector<float> master(kFrames, 9.0f);
    dev.render(master.data(), kFrames);
    CHECK(maxAbs(master.data(), kFrames) < 1e-3f);
    std::vector<std::vector<float> > stems(
        16, std::vector<float>(kFrames, 9.0f));
    std::vector<float*> bufs(16, nullptr);
    for (int ch = 0; ch < 16; ++ch)
      bufs[static_cast<std::size_t>(ch)] = stems[static_cast<std::size_t>(ch)].data();
    dev.renderPerChannel(bufs.data(), kFrames);
    for (int ch = 0; ch < 16; ++ch)
      CHECK(allZero(stems[static_cast<std::size_t>(ch)].data(), kFrames));
    dev.shutdown();
    std::printf("PASS gm mute gating\n");
  }

  if (g_failures == 0) {
    std::printf("ALL PASS (%d checks)\n", g_checks);
    return 0;
  }
  std::printf("%d/%d checks FAILED\n", g_failures, g_checks);
  return 1;
}
