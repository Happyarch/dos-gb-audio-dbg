// Stage 7.5: GmTab implementation.
// Draw methods early-return without an ImGui context. See gm_tab.h.

#include "tabs/gm_tab.h"

#include <cstdio>
#include <utility>

#include "imgui.h"

namespace audio_dbg {

const char* GmTab::programName(int program) const {
  return GmDevice::gmProgramName(program);
}

void GmTab::setGmDevice(GmDevice* dev) {
  gm_ = dev;
  MidiTab::setDevice(dev);
}

std::string GmTab::channelLabel(int ch) {
  if (ch < 0 || ch >= kChannels) return "Invalid";
  std::string label = "Ch " + std::to_string(ch + 1);
  if (ch == kDrumChannel) label += " [Drums]";
  return label;
}

GmTab::ChannelStrip GmTab::channelStrip(const DeviceSnapshot& s,
                                        int ch) const {
  ChannelStrip cs;
  cs.channel = ch;
  if (ch < 0 || ch >= static_cast<int>(s.channels.size())) return cs;
  const ChannelState& c = s.channels[static_cast<std::size_t>(ch)];
  cs.muted = c.muted;
  cs.soloed = c.soloed;
  cs.dormant = c.dormant;
  cs.peak = c.peak;
  cs.audible = DeviceTab::channelAudible(s, ch);
  if (gm_ != nullptr && ch < gm_->channelCount()) {
    cs.sounding = gm_->activeNoteCount(ch);
    cs.program = gm_->program(ch);
    cs.volume = static_cast<float>(gm_->controlChange(ch, 7)) / 127.0f;
    cs.pan = (static_cast<float>(gm_->controlChange(ch, 10)) - 64.0f) / 64.0f;
  }
  return cs;
}

void GmTab::onMuteClick(int ch) {
  if (gm_ == nullptr) return;
  if (ch < 0 || ch >= gm_->channelCount()) return;
  DeviceSnapshot s = gm_->snapshot();
  const bool cur = s.channels[static_cast<std::size_t>(ch)].muted;
  gm_->setMute(ch, !cur);
}

void GmTab::onSoloClick(int ch) {
  if (gm_ == nullptr) return;
  if (ch < 0 || ch >= gm_->channelCount()) return;
  DeviceSnapshot s = gm_->snapshot();
  const bool cur = s.channels[static_cast<std::size_t>(ch)].soloed;
  gm_->setSolo(ch, !cur);
}

void GmTab::drawChannelStrips(const DeviceSnapshot& s) {
  if (ImGui::GetCurrentContext() == nullptr) return;
  if (gm_ == nullptr) {
    ImGui::TextDisabled("No GM device attached.");
    return;
  }
  // 16 rows, one per MIDI channel. Dormant channels take the fast escape.
  for (int ch = 0; ch < kChannels; ++ch) {
    const ChannelStrip cs = channelStrip(s, ch);
    const std::string label = channelLabel(ch);
    if (cs.dormant) {
      ImGui::TextDisabled("%s dormant", label.c_str());
      continue;
    }
    // Activity LED: lit while notes sound on the channel.
    if (cs.sounding > 0) {
      ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.2f, 1.0f), "[o]");
    } else {
      ImGui::TextDisabled("[.]");
    }
    ImGui::SameLine();
    ImGui::Text("%s", label.c_str());
    ImGui::SameLine();
    if (cs.muted) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.2f, 0.1f, 1.0f));
    if (ImGui::SmallButton("[M]")) onMuteClick(ch);
    if (cs.muted) ImGui::PopStyleColor();
    ImGui::SameLine();
    if (cs.soloed) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.5f, 0.8f, 1.0f));
    if (ImGui::SmallButton("[S]")) onSoloClick(ch);
    if (cs.soloed) ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::Text("%s", programName(cs.program));
    ImGui::SameLine();
    // Program selector combo (128 GM programs).
    char combo_id[32];
    std::snprintf(combo_id, sizeof(combo_id), "##gmprog%d", ch);
    if (ImGui::BeginCombo(combo_id, programName(cs.program))) {
      for (int p = 0; p < 128; ++p) {
        const bool selected = (p == cs.program);
        if (ImGui::Selectable(programName(p), selected)) {
          gm_->dispatchProgramChange(ch, p);
        }
        if (selected) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    // Volume & pan readouts (CC7 / CC10).
    ImGui::Text("vol=%.2f pan=%+.2f", cs.volume, cs.pan);
    ImGui::SameLine();
    // Persistent piano-roll bar: most recent note on this channel.
    const std::vector<MidiNoteEvent>& hist = gm_->noteHistory();
    int last_vel = 0;
    bool last_sounding = false;
    for (std::size_t i = hist.size(); i-- > 0;) {
      if (hist[i].channel == ch) {
        last_vel = hist[i].velocity;
        last_sounding = hist[i].sounding;
        break;
      }
    }
    if (last_vel > 0) {
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImVec2 origin = ImGui::GetCursorScreenPos();
      drawNoteBar(dl, origin, 4.0f + static_cast<float>(last_vel), 8.0f,
                  last_vel, last_sounding);
      ImGui::Dummy(ImVec2(4.0f + static_cast<float>(last_vel), 8.0f));
      ImGui::SameLine();
    }
    ImGui::ProgressBar(cs.peak, ImVec2(80.0f, 0.0f));
  }
}

void GmTab::drawDetail(const DeviceSnapshot& s) {
  if (ImGui::GetCurrentContext() == nullptr) return;
  if (gm_ == nullptr) {
    ImGui::TextDisabled("No GM device attached.");
    return;
  }
  // Note history summary plus per-channel volume/pan table.
  const std::vector<MidiNoteEvent>& hist = gm_->noteHistory();
  ImGui::Text("Note history: %lu events",
              static_cast<unsigned long>(hist.size()));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  constexpr float kRowH = 4.0f;
  for (std::size_t i = 0; i < hist.size() && i < 64; ++i) {
    const MidiNoteEvent& ev = hist[i];
    const float y = origin.y + static_cast<float>(i) * (kRowH + 1.0f);
    const float w = 2.0f + static_cast<float>(ev.velocity);
    drawNoteBar(dl, ImVec2(origin.x, y), w, kRowH, ev.velocity, ev.sounding);
  }
  ImGui::Dummy(ImVec2(200.0f, 64.0f * (kRowH + 1.0f)));
  for (int ch = 0; ch < kChannels; ++ch) {
    const ChannelStrip cs = channelStrip(s, ch);
    if (cs.dormant) continue;
    ImGui::Text("%s prog=%d vol=%.2f pan=%+.2f notes=%lu", channelLabel(ch).c_str(),
                cs.program, cs.volume, cs.pan,
                static_cast<unsigned long>(cs.sounding));
  }
}

void GmTab::drawTracker(const DeviceSnapshot& s, const SimState* sim) {
  if (ImGui::GetCurrentContext() == nullptr) return;
  ImGui::Text("GM tracker: frame %u", s.frame);
  if (sim != nullptr) {
    ImGui::Text("Driver frame %u, %lu notes", sim->driver_frame,
                static_cast<unsigned long>(sim->notes.size()));
  } else {
    ImGui::TextDisabled("No sim state.");
  }
}

void GmTab::pushWaveformData(int ch, const float* samples, std::size_t n) {
  // Fast escape on dormant channels (or bad input).
  if (samples == nullptr || n == 0) return;
  if (gm_ != nullptr) {
    if (gm_->isDormant(ch)) return;
    ensureWaveStorage(static_cast<std::size_t>(gm_->channelCount()));
  } else {
    ensureWaveStorage(16);
  }
  storeWaveform(ch, samples, n);
}

}  // namespace audio_dbg
