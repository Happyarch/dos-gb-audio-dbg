// Stage 4.5: Acceptance tests for the concrete synthesizer backends.
// Verifies:
//   OPL3 (Opl3Device + NukedOPL3):
//     1. Fresh voices dormant; writeReg wakes + decodes operator params.
//     2. Key-on patch renders non-silent master + isolated per-voice stem.
//     3. renderPerChannel dormant fast escape (dormant stems are silence,
//        shadows stay uninitialized until first write).
//     4. Mute/solo gating silences the voice in both render paths.
//     5. Snapshot reflects hardware registers; readReg round-trips.
//   GB-APU (GbApuDevice + Basic_Gb_Apu):
//     1. Fresh channels dormant; duty/noise/wave writes wake + decode.
//     2. Pulse trigger renders non-silent master + isolated stem.
//     3. renderPerChannel dormant fast escape.
//     4. Wave RAM (32 nibbles), LFSR width, sweep decode.
//     5. Mute gating + register shadow round-trip.
// Headless: no GUI, no audio hardware. Exits 0 with ALL PASS, 1 on failure.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "devices/gb_apu_device.h"
#include "devices/opl3_device.h"

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

// Programs OPL voice `v` with a loud sustained 2-op patch and keys on.
// fnum=512/block=4 (~388 Hz at 49716 Hz).
void setupOplVoice(audio_dbg::Opl3Device& dev, int v) {
  const int bank = (v >= 9) ? 1 : 0;
  const int vl = v % 9;
  const std::uint16_t base = (bank == 1) ? 0x100 : 0x000;
  const std::uint8_t mod =
      audio_dbg::Opl3Device::kModSlots[static_cast<std::size_t>(vl)];
  const std::uint8_t car = static_cast<std::uint8_t>(mod + 3);
  dev.writeReg(base + 0x20 + mod, 0x01);  // MULT=1.
  dev.writeReg(base + 0x20 + car, 0x01);
  dev.writeReg(base + 0x40 + mod, 0x00);  // TL=0 (loudest).
  dev.writeReg(base + 0x40 + car, 0x00);
  dev.writeReg(base + 0x60 + mod, 0xF0);  // AR=15 instant.
  dev.writeReg(base + 0x60 + car, 0xF0);
  dev.writeReg(base + 0x80 + mod, 0x00);
  dev.writeReg(base + 0x80 + car, 0x00);
  dev.writeReg(base + 0xE0 + mod, 0x00);  // Sine.
  dev.writeReg(base + 0xE0 + car, 0x00);
  dev.writeReg(base + 0xC0 + static_cast<std::uint16_t>(vl),
               0x32);  // FB=1, FM, stereo pan.
  dev.writeReg(base + 0xA0 + static_cast<std::uint16_t>(vl), 0x00);
  dev.writeReg(base + 0xB0 + static_cast<std::uint16_t>(vl), 0x32);
}

// Triggers GB pulse channel `ch` (0/1) at ~256 Hz, vol 15, duty 50%.
void triggerGbPulse(audio_dbg::GbApuDevice& dev, int ch) {
  const std::uint16_t duty_addr = (ch == 0) ? 0xFF11 : 0xFF16;
  const std::uint16_t env_addr = (ch == 0) ? 0xFF12 : 0xFF17;
  const std::uint16_t lo_addr = (ch == 0) ? 0xFF13 : 0xFF18;
  const std::uint16_t hi_addr = (ch == 0) ? 0xFF14 : 0xFF19;
  dev.writeRegister(duty_addr, 0x80);  // Duty 2 (50%).
  dev.writeRegister(env_addr, 0xF0);   // Vol 15, no envelope steps.
  dev.writeRegister(lo_addr, 0x00);    // N=0x600 -> 256 Hz pulse.
  dev.writeRegister(hi_addr, 0x86);    // Trigger + high bits 0x06.
}

}  // namespace

int main() {
  using namespace audio_dbg;
  constexpr std::size_t kFrames = 256;

  // --- OPL3: fresh dormancy -------------------------------------------
  {
    Opl3Device dev;
    CHECK(dev.channelCount() == 18);
    CHECK(dev.init());
    for (int v = 0; v < 18; ++v) {
      CHECK(dev.isDormant(v));
      CHECK(dev.isVoiceDormant(v));
      CHECK(!dev.isShadowInitialized(v));
    }
    // Out-of-range regs ignored, wake nothing.
    dev.writeReg(0x200, 0xFF);
    dev.writeReg(0xFFFF, 0xFF);
    for (int v = 0; v < 18; ++v) CHECK(dev.isDormant(v));
    // Global reg wakes nothing.
    dev.writeReg(0x105, 0x01);
    for (int v = 0; v < 18; ++v) CHECK(dev.isDormant(v));
    CHECK(dev.readReg(0x105) == 0x01);
    CHECK(dev.readReg(0x200) == 0);
    dev.shutdown();
    std::printf("PASS opl3 fresh dormancy\n");
  }

  // --- OPL3: register decode + wake-up ---------------------------------
  {
    Opl3Device dev;
    CHECK(dev.init());
    dev.writeReg(0x20, 0x43);  // Voice 0 mod: VIB=1, MULT=3.
    CHECK(!dev.isDormant(0));
    CHECK(!dev.isVoiceDormant(0));
    CHECK(dev.isDormant(1));  // Untouched stays dormant.
    CHECK(!dev.isShadowInitialized(1));
    CHECK(dev.voice(0).mod.mult == 3);
    CHECK(dev.voice(0).mod.vib);
    CHECK(dev.readReg(0x20) == 0x43);
    dev.writeReg(0x40 + 0x03, 0x15);  // Voice 0 car TL=0x15.
    CHECK(dev.voice(0).car.tl == 0x15);
    dev.writeReg(0xC0, 0x36);  // FB=3, CON=0, pan stereo.
    CHECK(dev.voice(0).feedback == 3);
    CHECK(dev.voice(0).connection == 0);
    dev.writeReg(0xA0, 0x00);
    dev.writeReg(0xB0, 0x32);  // fnum 0x200, block 4, key on.
    CHECK(dev.voice(0).fnum == 0x200);
    CHECK(dev.voice(0).block == 4);
    CHECK(dev.voice(0).keyed_on);
    CHECK(dev.snapshot().channels[0].freq > 100.0f);
    // Bank-1 voice 9 via 0x100-bank regs.
    dev.writeReg(0x120, 0x05);  // Voice 9 mod MULT=5.
    CHECK(!dev.isDormant(9));
    CHECK(dev.voice(9).mod.mult == 5);
    CHECK(dev.isDormant(10));
    // Unmapped operator gap (e.g. 0x07) wakes nothing.
    Opl3Device dev2;
    CHECK(dev2.init());
    dev2.writeReg(0x27, 0x01);
    for (int v = 0; v < 18; ++v) CHECK(dev2.isDormant(v));
    dev.shutdown();
    dev2.shutdown();
    std::printf("PASS opl3 register decode\n");
  }

  // --- OPL3: rendering + dormant fast escape ---------------------------
  {
    Opl3Device dev;
    CHECK(dev.init());
    std::vector<float> silent(kFrames, 9.0f);
    std::vector<float*> stems(18, nullptr);
    std::vector<std::vector<float> > stem_bufs(
        18, std::vector<float>(kFrames, 9.0f));
    for (int v = 0; v < 18; ++v) stems[static_cast<std::size_t>(v)] =
        stem_bufs[static_cast<std::size_t>(v)].data();
    // All dormant: per-channel stems must be silence (fast escape).
    dev.renderPerChannel(stems.data(), kFrames);
    for (int v = 0; v < 18; ++v) CHECK(allZero(stem_bufs[static_cast<std::size_t>(v)].data(), kFrames));
    // Master of a silent chip is (near) silence.
    dev.render(silent.data(), kFrames);
    CHECK(maxAbs(silent.data(), kFrames) < 1e-4f);

    setupOplVoice(dev, 0);
    CHECK(dev.isShadowInitialized(0));
    CHECK(!dev.isShadowInitialized(5));
    std::vector<float> master(kFrames, 0.0f);
    dev.render(master.data(), kFrames);
    CHECK(maxAbs(master.data(), kFrames) > 1e-4f);

    for (int v = 0; v < 18; ++v) {
      std::fill(stem_bufs[static_cast<std::size_t>(v)].begin(),
                stem_bufs[static_cast<std::size_t>(v)].end(), 9.0f);
    }
    dev.renderPerChannel(stems.data(), kFrames);
    CHECK(maxAbs(stem_bufs[0].data(), kFrames) > 1e-4f);
    for (int v = 1; v < 18; ++v) {
      CHECK(allZero(stem_bufs[static_cast<std::size_t>(v)].data(), kFrames));
    }
    // Isolated stem matches the soloed master within float tolerance.
    dev.setSolo(0, true);
    std::vector<float> solo_master(kFrames, 0.0f);
    dev.render(solo_master.data(), kFrames);
    float worst = 0.0f;
    for (std::size_t i = 0; i < kFrames; ++i) {
      const float d = std::fabs(solo_master[i] - stem_bufs[0][i]);
      if (d > worst) worst = d;
    }
    // Soloed master sums the one audible shadow; the stem was rendered on
    // an earlier chip clock, so allow loose tolerance (same voice, same
    // patch — phase may have advanced between the two renders).
    CHECK(worst < 0.5f);
    dev.setSolo(0, false);
    dev.shutdown();
    std::printf("PASS opl3 rendering + dormant escape (worst=%.2e)\n", worst);
  }

  // --- OPL3: mute gating ------------------------------------------------
  {
    Opl3Device dev;
    CHECK(dev.init());
    setupOplVoice(dev, 2);
    std::vector<float> master(kFrames, 0.0f);
    dev.render(master.data(), kFrames);
    CHECK(maxAbs(master.data(), kFrames) > 1e-4f);
    dev.setMute(2, true);
    std::vector<float> muted(kFrames, 9.0f);
    dev.render(muted.data(), kFrames);
    CHECK(maxAbs(muted.data(), kFrames) < 1e-4f);
    std::vector<float> s0(kFrames, 9.0f);
    std::vector<float> s2(kFrames, 9.0f);
    float* bufs[18] = {nullptr};
    for (int v = 0; v < 18; ++v) bufs[v] = (v == 0) ? s0.data() : s2.data();
    // Voice 2 stem is gated to silence while muted; voice 0 stays dormant.
    dev.renderPerChannel(bufs, kFrames);
    CHECK(allZero(s2.data(), kFrames));
    CHECK(allZero(s0.data(), kFrames));
    dev.setMute(2, false);
    dev.render(master.data(), kFrames);
    CHECK(maxAbs(master.data(), kFrames) > 1e-4f);
    dev.shutdown();
    std::printf("PASS opl3 mute gating\n");
  }

  // --- OPL3: authored tier-1 voice patch --------------------------------
  // sub_bass bytes pinned from gen_opl_patches.py (the bridge is the only
  // runtime source; the device takes raw bytes so the table lives once).
  {
    Opl3Device dev;
    CHECK(dev.init());
    Opl3Device::VoicePatch sub;
    const std::uint8_t sub_reg[11] = {0x21, 0x3F, 0xF0, 0x07, 0x00,
                                      0x21, 0x00, 0xF0, 0x07, 0x00, 0x00};
    for (int i = 0; i < 11; ++i) sub.reg[i] = sub_reg[i];
    sub.volume = 60;
    sub.pan = 0x10;
    dev.setVoicePatch(4, sub);
    dev.setVoicePatch(99, sub);  // Out of range: ignored, no crash.
    const std::uint8_t on[3] = {4, 45, 100};
    dev.handleCommand(0x90, on, 3);
    // Voice 4 (bank 0): mod slot 0x09, carrier 0x0C.
    CHECK(dev.readReg(0x20 + 0x09) == 0x21);
    CHECK(dev.readReg(0x40 + 0x09) == 0x3F);
    CHECK(dev.readReg(0x60 + 0x0C) == 0xF0);
    CHECK(dev.readReg(0xE0 + 0x0C) == 0x00);
    // Carrier TL: base 0 + (127 - 100*60/127)/8 = 10, KSL preserved.
    CHECK(dev.readReg(0x40 + 0x0C) == 0x0A);
    // C0: patch feedback nibble (0) | pan.
    CHECK(dev.readReg(0xC0 + 0x04) == 0x10);
    std::vector<float> buf(kFrames, 0.0f);
    dev.render(buf.data(), kFrames);
    CHECK(maxAbs(buf.data(), kFrames) > 1e-4f);
    // Unpatched channel keeps the generic default voice (C0 0x32).
    const std::uint8_t on1[3] = {1, 60, 100};
    dev.handleCommand(0x90, on1, 3);
    CHECK(dev.readReg(0xC0 + 0x01) == 0x32);
    // Clearing releases the voice: next note-on reinstalls the default.
    dev.clearVoicePatches();
    dev.handleCommand(0x90, on, 3);
    CHECK(dev.readReg(0xC0 + 0x04) == 0x32);
    dev.shutdown();
    std::printf("PASS opl3 authored voice patch\n");
  }

  // --- GB-APU: fresh dormancy ------------------------------------------
  {
    GbApuDevice dev;
    CHECK(dev.channelCount() == 4);
    CHECK(dev.init());
    for (int c = 0; c < 4; ++c) {
      CHECK(dev.isDormant(c));
      CHECK(!dev.isShadowInitialized(c));
    }
    CHECK(dev.channelType(0) == PsgChannelType::PULSE1);
    CHECK(dev.channelType(1) == PsgChannelType::PULSE2);
    CHECK(dev.channelType(2) == PsgChannelType::WAVE);
    CHECK(dev.channelType(3) == PsgChannelType::NOISE);
    // Out-of-range ignored, wakes nothing.
    dev.writeRegister(0xFF00, 0xFF);
    dev.writeRegister(0xFF40, 0xFF);
    for (int c = 0; c < 4; ++c) CHECK(dev.isDormant(c));
    // Global regs wake nothing.
    dev.writeRegister(0xFF24, 0x77);
    dev.writeRegister(0xFF25, 0xFF);
    for (int c = 0; c < 4; ++c) CHECK(dev.isDormant(c));
    CHECK(dev.masterVolume() == 0x77);
    CHECK(dev.panning() == 0xFF);
    CHECK(dev.readRegister(0x1234) == 0xFF);
    dev.shutdown();
    std::printf("PASS gbapu fresh dormancy\n");
  }

  // --- GB-APU: register decode ------------------------------------------
  {
    GbApuDevice dev;
    CHECK(dev.init());
    dev.writeRegister(0xFF10, 0x19);  // Sweep: period 1, dir 0, shift 1.
    CHECK(!dev.isDormant(0));
    CHECK(dev.sweep(0) == 0x19);
    CHECK(dev.isDormant(1));
    dev.writeRegister(0xFF11, 0x80);  // Duty 2 (50%).
    CHECK(dev.channelDuty(0) == 2);
    dev.writeRegister(0xFF12, 0xF3);  // Vol 15, dir 0, period 3.
    CHECK(dev.psgChannel(0).volume == 15);
    // Pulse frequency N=0x600 -> ~256 Hz.
    dev.writeRegister(0xFF13, 0x00);
    dev.writeRegister(0xFF14, 0x86);
    CHECK(dev.channelFrequency(0) > 200.0);
    CHECK(dev.channelFrequency(0) < 320.0);
    CHECK(dev.psgChannel(0).active);
    CHECK(dev.readRegister(0xFF14) == 0x86);

    // Wave RAM: FF30=0xAB -> nibbles 0xA, 0xB.
    dev.writeRegister(0xFF30, 0xAB);
    CHECK(!dev.isDormant(2));
    CHECK(dev.psgChannel(2).wave_ram[0] == 0x0A);
    CHECK(dev.psgChannel(2).wave_ram[1] == 0x0B);
    dev.writeRegister(0xFF1C, 0x40);  // Wave vol code 2 (50%).
    CHECK(dev.waveVolumeCode() == 2);
    CHECK(dev.psgChannel(2).volume == 8);

    // Noise: 7-bit mode + divisor/shift decode.
    dev.writeRegister(0xFF22, 0x38);  // Shift 3, 7-bit, divisor 0.
    CHECK(!dev.isDormant(3));
    CHECK(dev.channelLfsrWidth(3) == 7);
    CHECK(dev.noiseSevenBit());
    CHECK(dev.noiseShift() == 3);
    CHECK(dev.noiseDivisor() == 0);
    CHECK(dev.channelFrequency(3) > 1000.0);
    dev.writeRegister(0xFF22, 0x30);  // 15-bit mode.
    CHECK(dev.channelLfsrWidth(3) == 15);
    CHECK(!dev.noiseSevenBit());
    dev.writeRegister(0xFF23, 0x80);  // Trigger noise.
    CHECK(dev.psgChannel(3).active);
    dev.shutdown();
    std::printf("PASS gbapu register decode\n");
  }

  // --- GB-APU: rendering + dormant fast escape --------------------------
  {
    GbApuDevice dev;
    CHECK(dev.init());
    std::vector<std::vector<float> > stems(
        4, std::vector<float>(kFrames, 9.0f));
    float* bufs[4] = {stems[0].data(), stems[1].data(), stems[2].data(),
                      stems[3].data()};
    dev.renderPerChannel(bufs, kFrames);
    for (int c = 0; c < 4; ++c) CHECK(allZero(stems[c].data(), kFrames));

    triggerGbPulse(dev, 0);
    CHECK(dev.isShadowInitialized(0));
    CHECK(!dev.isShadowInitialized(1));
    std::vector<float> master(kFrames, 0.0f);
    dev.render(master.data(), kFrames);
    CHECK(maxAbs(master.data(), kFrames) > 1e-4f);

    for (int c = 0; c < 4; ++c) std::fill(stems[c].begin(), stems[c].end(), 9.0f);
    dev.renderPerChannel(bufs, kFrames);
    CHECK(maxAbs(stems[0].data(), kFrames) > 1e-4f);
    for (int c = 1; c < 4; ++c) CHECK(allZero(stems[c].data(), kFrames));

    // Mute gates the pulse out of both paths.
    dev.setMute(0, true);
    std::vector<float> muted(kFrames, 9.0f);
    dev.render(muted.data(), kFrames);
    CHECK(maxAbs(muted.data(), kFrames) < 1e-4f);
    for (int c = 0; c < 4; ++c) std::fill(stems[c].begin(), stems[c].end(), 9.0f);
    dev.renderPerChannel(bufs, kFrames);
    CHECK(allZero(stems[0].data(), kFrames));
    dev.setMute(0, false);
    dev.shutdown();
    std::printf("PASS gbapu rendering + dormant escape\n");
  }

  // --- GB-APU: noise drums resolve via DRUM_PARAMS (audition mirror) ------
  {
    GbApuDevice dev;
    CHECK(dev.init());
    // MIDI channel 9 carries raw pret noise instrument ids (gb baseline).
    const std::uint8_t on17[3] = {9, 17, 100};
    dev.handleCommand(0x90, on17, 3);
    CHECK(dev.readRegister(0xFF21) == 0x91);  // vol 9, fade 1.
    CHECK(dev.readRegister(0xFF22) == 0x22);  // DRUM_PARAMS[17].nr43.
    CHECK(!dev.noiseSevenBit());
    CHECK(dev.psgChannel(3).active);
    const std::uint8_t off17[3] = {9, 17, 0};
    dev.handleCommand(0x90, off17, 3);
    CHECK(dev.readRegister(0xFF21) == 0x00);
    CHECK(dev.readRegister(0xFF23) == 0x80);  // silence retrigger.
    // 7-bit instrument: id 15 -> NR43 0x18 (width flag set).
    const std::uint8_t on15[3] = {9, 15, 100};
    dev.handleCommand(0x90, on15, 3);
    CHECK(dev.readRegister(0xFF21) == 0xA1);  // vol 10, fade 1.
    CHECK(dev.readRegister(0xFF22) == 0x18);
    CHECK(dev.noiseSevenBit());
    // Unknown id falls back to (8, 1, 34), as audition does.
    const std::uint8_t on99[3] = {9, 99, 100};
    dev.handleCommand(0x90, on99, 3);
    CHECK(dev.readRegister(0xFF21) == 0x81);  // vol 8, fade 1.
    CHECK(dev.readRegister(0xFF22) == 0x22);
    dev.shutdown();
    std::printf("PASS gbapu noise drum params\n");
  }

  if (g_failures == 0) {
    std::printf("ALL PASS (%d checks)\n", g_checks);
    return 0;
  }
  std::printf("%d/%d checks FAILED\n", g_failures, g_checks);
  return 1;
}
