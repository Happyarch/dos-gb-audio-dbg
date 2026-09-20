// Stage 7.4: GbApuTab implementation.
// Draw methods early-return without an ImGui context. See gb_apu_tab.h.

#include "tabs/gb_apu_tab.h"

#include <cstdio>

#include "imgui.h"

namespace audio_dbg {

namespace {

constexpr const char* kEnvelopeNames[8] = {
    "Env 0 (pace 0 down)", "Env 1 (pace 1 down)", "Env 2 (pace 2 down)",
    "Env 3 (pace 3 down)", "Env 4 (pace 4 down)", "Env 5 (pace 5 down)",
    "Env 6 (pace 6 down)", "Env 7 (pace 7 down)",
};

}  // namespace

const char* GbApuTab::envelopeName(int envelope) const {
  if (envelope < 0) envelope = 0;
  if (envelope > 7) envelope = 7;
  return kEnvelopeNames[envelope];
}

void GbApuTab::setGbApuDevice(GbApuDevice* dev) {
  gb_ = dev;
  PsgTab::setDevice(dev);
}

std::string GbApuTab::channelLabel(int ch) {
  switch (ch) {
    case kPulse1:
      return "Pulse 1";
    case kPulse2:
      return "Pulse 2";
    case kWave:
      return "Wave";
    case kNoise:
      return "Noise";
    default:
      return "Invalid";
  }
}

const char* GbApuTab::noiseWidthName(bool seven_bit) {
  return seven_bit ? "7-bit" : "15-bit";
}

const char* GbApuTab::waveVolumeName(int code) {
  switch (code & 3) {
    case 0:
      return "Mute";
    case 1:
      return "100%";
    case 2:
      return "50%";
    default:
      return "25%";
  }
}

GbApuTab::ChannelCard GbApuTab::channelCard(int ch) const {
  ChannelCard c;
  c.channel = ch;
  if (ch < 0 || ch >= kChannels) return c;
  if (gb_ == nullptr) return c;
  if (ch >= gb_->channelCount()) return c;
  const PsgChannel& p = gb_->psgChannel(ch);
  c.dormant = p.is_dormant;
  c.frequency = p.frequency;
  c.duty = p.duty;
  c.volume = p.volume;
  c.envelope = p.envelope;
  c.wave_ram = p.wave_ram;
  DeviceSnapshot s = gb_->snapshot();
  if (ch < static_cast<int>(s.channels.size())) {
    c.muted = s.channels[static_cast<std::size_t>(ch)].muted;
    c.active = !s.channels[static_cast<std::size_t>(ch)].dormant;
  }
  if (ch == kPulse1) c.sweep = gb_->sweep(0);
  if (ch == kWave) c.wave_volume_code = gb_->waveVolumeCode();
  if (ch == kNoise) {
    c.noise_seven_bit = gb_->noiseSevenBit();
    c.noise_shift = gb_->noiseShift();
    c.noise_divisor = gb_->noiseDivisor();
  }
  return c;
}

std::size_t GbApuTab::findZeroCrossing(const float* samples, std::size_t n) {
  if (samples == nullptr || n < 2) return 0;
  for (std::size_t i = 0; i + 1 < n; ++i) {
    if (samples[i] <= 0.0f && samples[i + 1] > 0.0f) return i + 1;
  }
  return 0;
}

void GbApuTab::onMuteClick(int ch) {
  if (gb_ == nullptr) return;
  if (ch < 0 || ch >= gb_->channelCount()) return;
  DeviceSnapshot s = gb_->snapshot();
  const bool cur = s.channels[static_cast<std::size_t>(ch)].muted;
  gb_->setMute(ch, !cur);
}

void GbApuTab::pushWaveformData(int ch, const float* samples, std::size_t n) {
  // Fast escape on dormant channels (or bad input).
  if (samples == nullptr || n == 0) return;
  if (gb_ != nullptr) {
    if (gb_->isDormant(ch)) return;
    ensureWaveStorage(static_cast<std::size_t>(gb_->channelCount()));
  } else {
    ensureWaveStorage(static_cast<std::size_t>(kChannels));
  }
  storeWaveform(ch, samples, n);
}

void GbApuTab::drawChannelStrips(const DeviceSnapshot& s) {
  if (ImGui::GetCurrentContext() == nullptr) return;
  if (gb_ == nullptr) {
    ImGui::TextDisabled("No GB-APU device attached.");
    return;
  }
  // 4 channel cards: type name + frequency/volume, dormant fast escape.
  for (int ch = 0; ch < kChannels; ++ch) {
    if (ch >= static_cast<int>(s.channels.size())) break;
    ImGui::PushID(ch);
    if (s.channels[static_cast<std::size_t>(ch)].dormant) {
      ImGui::TextDisabled("%s dormant", channelLabel(ch).c_str());
      ImGui::PopID();
      continue;
    }
    const ChannelCard c = channelCard(ch);
    ImGui::Text("%s %.1f Hz vol=%d %s", channelLabel(ch).c_str(),
                c.frequency, c.volume, envelopeName(c.envelope));
    ImGui::SameLine();
    if (ImGui::SmallButton(muteLabel(c.muted))) onMuteClick(ch);
    ImGui::PopID();
  }
}

void GbApuTab::drawDetail(const DeviceSnapshot& s) {
  if (ImGui::GetCurrentContext() == nullptr) return;
  if (gb_ == nullptr) {
    ImGui::TextDisabled("No GB-APU device attached.");
    return;
  }
  for (int ch = 0; ch < kChannels; ++ch) {
    const PsgChannel& p = gb_->psgChannel(ch);
    if (p.is_dormant) continue;
    const ChannelCard c = channelCard(ch);
    ImGui::Separator();
    ImGui::Text("%s", channelLabel(ch).c_str());
    ImVec2 origin = ImGui::GetCursorScreenPos();
    if (ch == kPulse1) {
      // Pulse 1: duty + period/frequency + envelope volume + sweep.
      drawDutyBar(ImGui::GetWindowDrawList(), origin, 128.0f, 12.0f, p.duty);
      ImGui::Dummy(ImVec2(128.0f, 14.0f));
      ImGui::Text("duty=%d (%.1f%%) freq=%.1f Hz vol=%d %s sweep=0x%02X",
                  p.duty, dutyFraction(p.duty) * 100.0f, c.frequency,
                  c.volume, envelopeName(c.envelope), c.sweep);
    } else if (ch == kPulse2) {
      // Pulse 2: duty + period/frequency + envelope volume (no sweep).
      drawDutyBar(ImGui::GetWindowDrawList(), origin, 128.0f, 12.0f, p.duty);
      ImGui::Dummy(ImVec2(128.0f, 14.0f));
      ImGui::Text("duty=%d (%.1f%%) freq=%.1f Hz vol=%d %s", p.duty,
                  dutyFraction(p.duty) * 100.0f, c.frequency, c.volume,
                  envelopeName(c.envelope));
    } else if (ch == kWave) {
      // Wave: 32-nibble Wave RAM visualizer + hex readouts + freq + level.
      drawWaveRam(ImGui::GetWindowDrawList(), origin, 160.0f, 32.0f,
                  p.wave_ram);
      ImGui::Dummy(ImVec2(160.0f, 34.0f));
      char hex[128];
      std::snprintf(hex, sizeof(hex),
                    "%X %X %X %X %X %X %X %X | %X %X %X %X %X %X %X %X",
                    p.wave_ram[0] & 0x0F, p.wave_ram[1] & 0x0F,
                    p.wave_ram[2] & 0x0F, p.wave_ram[3] & 0x0F,
                    p.wave_ram[4] & 0x0F, p.wave_ram[5] & 0x0F,
                    p.wave_ram[6] & 0x0F, p.wave_ram[7] & 0x0F,
                    p.wave_ram[8] & 0x0F, p.wave_ram[9] & 0x0F,
                    p.wave_ram[10] & 0x0F, p.wave_ram[11] & 0x0F,
                    p.wave_ram[12] & 0x0F, p.wave_ram[13] & 0x0F,
                    p.wave_ram[14] & 0x0F, p.wave_ram[15] & 0x0F);
      ImGui::Text("wave0-15: %s", hex);
      std::snprintf(hex, sizeof(hex),
                    "%X %X %X %X %X %X %X %X | %X %X %X %X %X %X %X %X",
                    p.wave_ram[16] & 0x0F, p.wave_ram[17] & 0x0F,
                    p.wave_ram[18] & 0x0F, p.wave_ram[19] & 0x0F,
                    p.wave_ram[20] & 0x0F, p.wave_ram[21] & 0x0F,
                    p.wave_ram[22] & 0x0F, p.wave_ram[23] & 0x0F,
                    p.wave_ram[24] & 0x0F, p.wave_ram[25] & 0x0F,
                    p.wave_ram[26] & 0x0F, p.wave_ram[27] & 0x0F,
                    p.wave_ram[28] & 0x0F, p.wave_ram[29] & 0x0F,
                    p.wave_ram[30] & 0x0F, p.wave_ram[31] & 0x0F);
      ImGui::Text("wave16-31: %s", hex);
      ImGui::Text("freq=%.1f Hz level=%s vol=%d", c.frequency,
                  waveVolumeName(c.wave_volume_code), c.volume);
    } else {
      // Noise: 7-bit vs 15-bit mode, polynomial step, shift, divisor.
      ImGui::Text("mode=%s width=%d shift=%u divisor=%u freq=%.1f Hz vol=%d",
                  noiseWidthName(c.noise_seven_bit), p.lfsr_width,
                  c.noise_shift, c.noise_divisor, c.frequency, c.volume);
    }
    // Per-channel oscilloscope: read the audio-thread scope ring owned by
    // the device (filled in GbApuDevice::render()). The UI never renders a
    // scope itself. Stabilized on the first positive zero crossing.
    float buf[256];
    const std::size_t got = gb_->copyWaveform(ch, buf, 256);
    if (got >= 2) {
      const std::size_t start = findZeroCrossing(buf, got);
      const float* base = buf + (start < got ? start : 0);
      const int count = static_cast<int>(got - (start < got ? start : 0));
      char scope_id[32];
      std::snprintf(scope_id, sizeof(scope_id), "##gbpuscope%d", ch);
      ImGui::PlotLines(scope_id, base, count, 0, nullptr, -1.0f, 1.0f,
                       ImVec2(220.0f, 48.0f));
    } else {
      ImGui::TextDisabled("Scope: no data");
    }
  }
  (void)s;
}

void GbApuTab::drawTracker(const DeviceSnapshot& s, const SimState* sim) {
  if (ImGui::GetCurrentContext() == nullptr) return;
  ImGui::Text("GB-APU tracker: frame %u", s.frame);
  if (sim != nullptr) {
    ImGui::Text("Driver frame %u, %lu notes", sim->driver_frame,
                static_cast<unsigned long>(sim->notes.size()));
  } else {
    ImGui::TextDisabled("No sim state.");
  }
}

}  // namespace audio_dbg
