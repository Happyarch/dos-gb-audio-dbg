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
    ImGui::PushID(ch);
    const ChannelStrip cs = channelStrip(s, ch);
    const std::string label = channelLabel(ch);
    if (cs.dormant) {
      ImGui::TextDisabled("%s dormant", label.c_str());
      ImGui::PopID();
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
    // Program selector combo (128 GM programs).
    char combo_id[32];
    std::snprintf(combo_id, sizeof(combo_id), "##gmprog%d", ch);
    ImGui::SetNextItemWidth(140.0f);
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
    // Musical keyboard pitch scale bar showing active sounding notes.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    constexpr float kBarW = 240.0f;
    constexpr float kBarH = 16.0f;
    drawPitchScale(dl, origin, ImVec2(kBarW, kBarH), gm_, ch);
    ImGui::Dummy(ImVec2(kBarW, kBarH));
    ImGui::SameLine();
    // Volume & pan readouts (CC7 / CC10).
    ImGui::Text("v:%.2f p:%+.2f", cs.volume, cs.pan);
    ImGui::SameLine();
    ImGui::ProgressBar(cs.peak, ImVec2(60.0f, 0.0f));
    ImGui::PopID();
  }
}

void GmTab::drawDetail(const DeviceSnapshot& s) {
  if (ImGui::GetCurrentContext() == nullptr) return;
  if (gm_ == nullptr) {
    ImGui::TextDisabled("No GM device attached.");
    return;
  }
  ImGui::Text("GM Channel Telemetry & Routing:");
  ImGui::Separator();
  for (int ch = 0; ch < kChannels; ++ch) {
    const ChannelStrip cs = channelStrip(s, ch);
    if (cs.dormant) continue;
    std::string sounding_notes;
    for (int note = 0; note < MidiDevice::kNotesPerChannel; ++note) {
      if (gm_->isNoteSounding(ch, note)) {
        if (!sounding_notes.empty()) sounding_notes += ", ";
        sounding_notes += std::to_string(note);
      }
    }
    ImGui::Text("%-16s | %-20s | vol=%.2f pan=%+.2f | notes=%lu (%s) | peak=%.2f",
                channelLabel(ch).c_str(), programName(cs.program),
                cs.volume, cs.pan, static_cast<unsigned long>(cs.sounding),
                sounding_notes.empty() ? "-" : sounding_notes.c_str(),
                cs.peak);
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
