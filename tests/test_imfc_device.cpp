// Stage 4.6: Acceptance tests for the IMFC (Yamaha FB-01) backend and tab.
// Verifies:
//   1. Fresh channels dormant; init() succeeds; sample rate matches 55930 Hz.
//   2. ROM preset names resolve accurately across ROM 1..5 banks.
//   3. Note dispatch produces real 4-op FM synthesis samples (non-zero audio).
//   4. Per-channel rendering, dormant fast escape, and mute gating.
//   5. Yamaha FB-01 SysEx voice upload and RAM bank storage.
//   6. ImfcTab ImGui-free helper methods and headless null-safety.
// Headless: no GUI, no audio hardware. Exits 0 with ALL PASS, 1 on failure.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "devices/imfc_device.h"
#include "tabs/imfc_tab.h"

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

void testImfcLifecycle() {
  audio_dbg::ImfcDevice dev;
  CHECK(dev.channelCount() == audio_dbg::ImfcDevice::kChannels);
  CHECK(dev.channelCount() == 8);

  const auto s0 = dev.snapshot();
  CHECK(s0.channels.size() == 8);
  for (int i = 0; i < 8; ++i) {
    CHECK(s0.channels[i].dormant);
    CHECK(!s0.channels[i].muted);
  }

  CHECK(dev.init());
  CHECK(dev.sampleRate() == audio_dbg::ImfcDevice::kDefaultRate);
}

void testImfcPresets() {
  audio_dbg::ImfcDevice dev;
  CHECK(dev.init());

  // ROM 1 (Bank 2) checks
  CHECK(std::string(dev.imfcPresetName(2, 0)) == "Brass");
  CHECK(std::string(dev.imfcPresetName(2, 2)) == "Trumpet");
  CHECK(std::string(dev.imfcPresetName(2, 5)) == "Piano");
  CHECK(std::string(dev.imfcPresetName(2, 15)) == "Flute");

  // ROM 2 (Bank 3) checks
  CHECK(std::string(dev.imfcPresetName(3, 0)) == "UpPiano");
  CHECK(std::string(dev.imfcPresetName(3, 7)) == "Grand");

  // ROM 3 (Bank 4) checks
  CHECK(std::string(dev.imfcPresetName(4, 0)) == "Horn2");
  CHECK(std::string(dev.imfcPresetName(4, 19)) == "SoloVio");

  // ROM 4 (Bank 5) checks
  CHECK(std::string(dev.imfcPresetName(5, 0)) == "FnkSyn2");
  CHECK(std::string(dev.imfcPresetName(5, 30)) == "Marimb2");

  // ROM 5 (Bank 6) checks
  CHECK(std::string(dev.imfcPresetName(6, 0)) == "JOrgan1");
  CHECK(std::string(dev.imfcPresetName(6, 47)) == "SineWav");
}

void testImfcSynthesisAndMute() {
  audio_dbg::ImfcDevice dev;
  CHECK(dev.init());

  // Render before any note: output must be silent
  constexpr std::size_t kFrames = 512;
  std::vector<float> buf(kFrames * 2, 0.0f);
  dev.render(buf.data(), kFrames);
  CHECK(allZero(buf.data(), buf.size()));

  // Dispatch note on channel 0 (program defaults to Brass)
  dev.noteOn(0, 60, 100);
  CHECK(dev.synthCore()->isInstrumentSounding(0));
  CHECK(dev.synthCore()->activeNotes(0) > 0);

  // Render audio: FM synthesis should generate non-zero PCM
  dev.render(buf.data(), kFrames);
  const float peak = maxAbs(buf.data(), buf.size());
  CHECK(peak > 0.001f);

  // Note off
  dev.noteOff(0, 60);

  // Mute test: when muted, new notes should not sound
  dev.setMute(0, true);
  std::vector<float> mute_buf(kFrames * 2, 0.0f);
  dev.noteOn(0, 64, 100);
  dev.render(mute_buf.data(), kFrames);
  CHECK(allZero(mute_buf.data(), mute_buf.size()));

  // Unmute
  dev.setMute(0, false);
  dev.noteOn(0, 64, 100);
  dev.render(buf.data(), kFrames);
  CHECK(maxAbs(buf.data(), buf.size()) > 0.001f);
  dev.noteOff(0, 64);
}

void testImfcPerChannelRender() {
  audio_dbg::ImfcDevice dev;
  CHECK(dev.init());

  constexpr std::size_t kFrames = 256;
  std::vector<std::vector<float>> ch_bufs(8, std::vector<float>(kFrames, 0.0f));
  std::vector<float*> ptrs(8);
  for (int i = 0; i < 8; ++i) ptrs[i] = ch_bufs[i].data();

  // Play a note on channel 2
  dev.noteOn(2, 67, 110);
  dev.renderPerChannel(ptrs.data(), kFrames);

  // Channel 2 should have audio
  CHECK(maxAbs(ch_bufs[2].data(), kFrames) > 0.001f);

  // Other channels that were never touched should be silent
  CHECK(allZero(ch_bufs[0].data(), kFrames));
  CHECK(allZero(ch_bufs[1].data(), kFrames));
  CHECK(allZero(ch_bufs[3].data(), kFrames));
  dev.noteOff(2, 67);
}

void testImfcSysExVoiceUpload() {
  audio_dbg::ImfcDevice dev;
  CHECK(dev.init());

  // 1. Turn off memory protect
  uint8_t unprotect[] = {0xF0, 0x43, 0x75, 0x00, 0x10, 0x21, 0x00, 0xF7};
  dev.dispatchSysEx(unprotect, sizeof(unprotect));
  CHECK(!dev.synthCore()->memoryProtect());

  // 2. Prepare 64-byte VoiceDefinition for a custom voice named "TestVox"
  audio_dbg::ImfcVoiceDef vdef;
  std::memcpy(vdef.name, "TestVox", 7);
  vdef.byteC = 0xC0 | (3 << 3) | 4; // Feedback 3, Algorithm 4

  // Pack into 128 nibbles (low nibble then high nibble)
  std::vector<uint8_t> upload_sysex = {
      0xF0, 0x43, 0x75, 0x00, 0x08, 0x00, 0x01, 0x00
  };
  const uint8_t* raw = reinterpret_cast<const uint8_t*>(&vdef);
  int sum = 0;
  for (int i = 0; i < 64; ++i) {
    uint8_t lo = raw[i] & 0x0F;
    uint8_t hi = (raw[i] >> 4) & 0x0F;
    upload_sysex.push_back(lo);
    upload_sysex.push_back(hi);
    sum += lo + hi;
  }
  uint8_t chk = static_cast<uint8_t>((-sum) & 0x7F);
  upload_sysex.push_back(chk);
  upload_sysex.push_back(0xF7);

  // Send voice upload to instrument 0 buffer
  dev.dispatchSysEx(upload_sysex.data(), upload_sysex.size());

  // 3. Store to RAM Bank 0 slot 0
  uint8_t store[] = {0xF0, 0x43, 0x75, 0x00, 0x28, 0x40, 0x00, 0xF7};
  dev.dispatchSysEx(store, sizeof(store));

  // Verify custom voice is now stored in RAM Bank 0 slot 0!
  const auto& stored = dev.synthCore()->customVoice(0, 0);
  CHECK(stored.getName() == "TestVox");
  CHECK(stored.getAlgorithm() == 4);
  CHECK(stored.getFeedback() == 3);
}

void testImfcTabHelpers() {
  audio_dbg::ImfcDevice dev;
  CHECK(dev.init());

  audio_dbg::ImfcTab tab;
  tab.setImfcDevice(&dev);

  CHECK(audio_dbg::ImfcTab::instrumentLabel(0) == "Inst 1 [Ch 1]");
  CHECK(audio_dbg::ImfcTab::instrumentLabel(7) == "Inst 8 [Ch 8]");

  CHECK(tab.selectedInstrument() == 0);
  tab.setSelectedInstrument(5);
  CHECK(tab.selectedInstrument() == 5);
  tab.setSelectedInstrument(99); // Out of bounds -> ignored
  CHECK(tab.selectedInstrument() == 5);

  const auto snap = dev.snapshot();
  auto is = tab.instrumentStrip(snap, 0);
  CHECK(is.inst == 0);
  CHECK(is.channel == 0);
  CHECK(is.dormant);

  // Null-safe draw calls when ImGui context is absent (early return without crash)
  tab.drawChannelStrips(snap);
  tab.drawDetail(snap);
  tab.drawTracker(snap, nullptr);
}

} // namespace

int main() {
  testImfcLifecycle();
  testImfcPresets();
  testImfcSynthesisAndMute();
  testImfcPerChannelRender();
  testImfcSysExVoiceUpload();
  testImfcTabHelpers();

  std::printf("IMFC Device & Tab tests: %d checks, %d failures\n", g_checks,
              g_failures);
  return g_failures == 0 ? 0 : 1;
}
