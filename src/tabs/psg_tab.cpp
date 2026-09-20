// Stage 2.4: PsgTab implementation.
// Draw methods early-return without an ImGui context. See psg_tab.h.

#include "tabs/psg_tab.h"

#include "imgui.h"

namespace audio_dbg {

const char* PsgTab::channelTypeName(PsgChannelType type) {
  switch (type) {
    case PsgChannelType::PULSE1:
      return "Pulse1";
    case PsgChannelType::PULSE2:
      return "Pulse2";
    case PsgChannelType::WAVE:
      return "Wave";
    case PsgChannelType::NOISE:
      return "Noise";
  }
  return "Unknown";
}

float PsgTab::dutyFraction(int duty) {
  switch (duty & 3) {
    case 0:
      return 0.125f;
    case 1:
      return 0.25f;
    case 2:
      return 0.5f;
    default:
      return 0.75f;
  }
}

void PsgTab::drawDutyBar(ImDrawList* dl, ImVec2 origin, float width,
                         float height, int duty) {
  if (dl == nullptr || width <= 0.0f || height <= 0.0f) return;
  const float frac = dutyFraction(duty);
  const ImU32 hi =
      ImGui::ColorConvertFloat4ToU32(ImVec4(0.2f, 0.9f, 0.3f, 1.0f));
  const ImU32 lo =
      ImGui::ColorConvertFloat4ToU32(ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
  dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), lo);
  dl->AddRectFilled(origin, ImVec2(origin.x + width * frac, origin.y + height),
                    hi);
}

void PsgTab::drawWaveRam(ImDrawList* dl, ImVec2 origin, float width,
                         float height,
                         const std::array<std::uint8_t, 32>& ram) {
  if (dl == nullptr || width <= 0.0f || height <= 0.0f) return;
  const ImU32 bar =
      ImGui::ColorConvertFloat4ToU32(ImVec4(0.3f, 0.7f, 1.0f, 1.0f));
  const float bw = width / 32.0f;
  for (int i = 0; i < 32; ++i) {
    const float frac = static_cast<float>(ram[static_cast<std::size_t>(i)] & 0x0F) / 15.0f;
    const float bh = height * frac;
    const float x = origin.x + static_cast<float>(i) * bw;
    dl->AddRectFilled(ImVec2(x, origin.y + height - bh),
                      ImVec2(x + bw - 1.0f, origin.y + height), bar);
  }
}

void PsgTab::drawChannelStrips(const DeviceSnapshot& s) {
  if (ImGui::GetCurrentContext() == nullptr) return;
  // Channel cards: type name + frequency/volume, dormant fast escape.
  for (std::size_t i = 0; i < s.channels.size(); ++i) {
    const int ch = static_cast<int>(i);
    if (s.channels[i].dormant) {
      ImGui::TextDisabled("CH%d dormant", ch);
      continue;
    }
    const char* type = "—";
    double freq = 0.0;
    int vol = 0;
    if (device_ != nullptr && ch < device_->channelCount()) {
      const PsgChannel& p = device_->psgChannel(ch);
      type = channelTypeName(p.type);
      freq = p.frequency;
      vol = p.volume;
    }
    ImGui::Text("CH%d %s %.1f Hz vol=%d", ch, type, freq, vol);
    ImGui::SameLine();
    if (ImGui::SmallButton(muteLabel(s.channels[i].muted))) onMuteClick(ch);
  }
}

void PsgTab::drawDetail(const DeviceSnapshot& s) {
  if (ImGui::GetCurrentContext() == nullptr) return;
  if (device_ == nullptr) {
    ImGui::TextDisabled("No PSG device attached.");
    return;
  }
  for (int ch = 0; ch < device_->channelCount(); ++ch) {
    const PsgChannel& p = device_->psgChannel(ch);
    if (p.is_dormant) continue;
    ImGui::Text("CH%d %s env=%s lfsr=%d", ch, channelTypeName(p.type),
                envelopeName(p.envelope), p.lfsr_width);
    ImVec2 origin = ImGui::GetCursorScreenPos();
    if (p.type == PsgChannelType::PULSE1 ||
        p.type == PsgChannelType::PULSE2) {
      drawDutyBar(ImGui::GetWindowDrawList(), origin, 128.0f, 12.0f, p.duty);
      ImGui::Dummy(ImVec2(128.0f, 14.0f));
    } else if (p.type == PsgChannelType::WAVE) {
      drawWaveRam(ImGui::GetWindowDrawList(), origin, 160.0f, 32.0f,
                  p.wave_ram);
      ImGui::Dummy(ImVec2(160.0f, 34.0f));
    }
  }
  (void)s;
}

void PsgTab::drawTracker(const DeviceSnapshot& s, const SimState* sim) {
  if (ImGui::GetCurrentContext() == nullptr) return;
  ImGui::Text("PSG tracker: frame %u", s.frame);
  if (sim != nullptr) {
    ImGui::Text("Driver frame %u, %lu notes", sim->driver_frame,
                static_cast<unsigned long>(sim->notes.size()));
  } else {
    ImGui::TextDisabled("No sim state.");
  }
}

void PsgTab::onMuteClick(int ch) {
  if (device_ == nullptr) return;
  if (ch < 0 || ch >= device_->channelCount()) return;
  DeviceSnapshot s = device_->snapshot();
  const bool cur = s.channels[static_cast<std::size_t>(ch)].muted;
  device_->setMute(ch, !cur);
}

void PsgTab::pushWaveformData(int ch, const float* samples, std::size_t n) {
  if (samples == nullptr || n == 0) return;
  if (device_ != nullptr) {
    if (device_->isDormant(ch)) return;
    ensureWaveStorage(static_cast<std::size_t>(device_->channelCount()));
  } else {
    ensureWaveStorage(4);
  }
  storeWaveform(ch, samples, n);
}

}  // namespace audio_dbg
