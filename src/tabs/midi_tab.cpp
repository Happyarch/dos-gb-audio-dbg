// Stage 2.2: MidiTab implementation.
// Draw methods early-return without an ImGui context so headless checks can
// link and call helpers safely. See midi_tab.h.

#include "tabs/midi_tab.h"

#include "imgui.h"

namespace audio_dbg {

void MidiTab::drawNoteBar(ImDrawList* dl, ImVec2 origin, float width,
                          float row_h, int velocity, bool sounding) {
  if (dl == nullptr || width <= 0.0f || row_h <= 0.0f) return;
  if (velocity < 0) velocity = 0;
  if (velocity > 127) velocity = 127;
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  DeviceTab::velocityColor(velocity, &r, &g, &b);
  const ImU32 col =
      ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, sounding ? 1.0f : 0.35f));
  dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + row_h), col);
}

void MidiTab::drawPitchScale(ImDrawList* dl, ImVec2 origin, ImVec2 size,
                            MidiDevice* dev, int ch) {
  if (dl == nullptr || size.x <= 0.0f || size.y <= 0.0f) return;

  const ImU32 bg_col =
      ImGui::ColorConvertFloat4ToU32(ImVec4(0.18f, 0.18f, 0.20f, 1.0f));
  const ImU32 border_col =
      ImGui::ColorConvertFloat4ToU32(ImVec4(0.28f, 0.28f, 0.30f, 1.0f));
  const ImU32 tick_col =
      ImGui::ColorConvertFloat4ToU32(ImVec4(0.32f, 0.32f, 0.35f, 1.0f));

  dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), bg_col,
                    2.0f);
  dl->AddRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), border_col,
              2.0f);

  // Standard piano range: A0 (21) to C8 (108).
  constexpr int kMinNote = 21;
  constexpr int kMaxNote = 108;
  constexpr int kRange = kMaxNote - kMinNote;

  // Draw octave ticks for C keys (24, 36, 48, 60, 72, 84, 96, 108).
  for (int n = 24; n <= 108; n += 12) {
    const float x = origin.x +
                    (static_cast<float>(n - kMinNote) /
                     static_cast<float>(kRange)) *
                        size.x;
    dl->AddLine(ImVec2(x, origin.y + 1), ImVec2(x, origin.y + size.y - 1),
                tick_col, 1.0f);
  }

  if (dev == nullptr || ch < 0 || ch >= dev->channelCount()) return;

  // Draw active sounding notes as markers on the scale.
  constexpr float kMarkerW = 4.0f;
  for (int note = 0; note < MidiDevice::kNotesPerChannel; ++note) {
    if (dev->isNoteSounding(ch, note)) {
      int clamped = note;
      if (clamped < kMinNote) clamped = kMinNote;
      if (clamped > kMaxNote) clamped = kMaxNote;
      const float frac = static_cast<float>(clamped - kMinNote) /
                         static_cast<float>(kRange);
      const float mx = origin.x + frac * (size.x - kMarkerW);
      const int vel = dev->noteVelocity(ch, note);
      const float alpha = 0.65f + 0.35f * (static_cast<float>(vel) / 127.0f);
      const ImU32 marker_col = ImGui::ColorConvertFloat4ToU32(
          ImVec4(0.98f, 0.45f, 0.12f, alpha));
      dl->AddRectFilled(ImVec2(mx, origin.y + 1.0f),
                        ImVec2(mx + kMarkerW, origin.y + size.y - 1.0f),
                        marker_col, 1.0f);
    }
  }
}

MidiTab::StripState MidiTab::stripState(const DeviceSnapshot& s, int ch) const {
  StripState st;
  if (ch < 0 || static_cast<std::size_t>(ch) >= s.channels.size()) return st;
  const ChannelState& c = s.channels[static_cast<std::size_t>(ch)];
  st.muted = c.muted;
  st.soloed = c.soloed;
  st.dormant = c.dormant;
  st.peak = c.peak;
  st.audible = DeviceTab::channelAudible(s, ch);
  if (device_ != nullptr && ch < device_->channelCount()) {
    st.active_notes = device_->activeNoteCount(ch);
  }
  return st;
}

void MidiTab::drawChannelStrips(const DeviceSnapshot& s) {
  if (ImGui::GetCurrentContext() == nullptr) return;
  // MUNT-QT style strips: LED, label, mute toggle, patch name,
  // keyboard pitch scale, and peak meter.
  for (std::size_t i = 0; i < s.channels.size(); ++i) {
    const int ch = static_cast<int>(i);
    ImGui::PushID(ch);
    const ChannelState& c = s.channels[i];
    if (c.dormant) {
      ImGui::TextDisabled("CH%02d dormant", ch);
      ImGui::PopID();
      continue;
    }
    const StripState st = stripState(s, ch);
    const int prog = (device_ != nullptr) ? device_->program(ch) : 0;
    if (st.active_notes > 0) {
      ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.2f, 1.0f), "[o]");
    } else {
      ImGui::TextDisabled("[.]");
    }
    ImGui::SameLine();
    ImGui::Text("CH%02d", ch);
    ImGui::SameLine();
    if (c.muted) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.2f, 0.1f, 1.0f));
    if (ImGui::SmallButton(muteLabel(c.muted))) onMuteClick(ch);
    if (c.muted) ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::Text("%-16s", programName(prog));
    ImGui::SameLine();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    constexpr float kBarW = 240.0f;
    constexpr float kBarH = 16.0f;
    drawPitchScale(dl, origin, ImVec2(kBarW, kBarH), device_, ch);
    ImGui::Dummy(ImVec2(kBarW, kBarH));
    ImGui::SameLine();
    ImGui::ProgressBar(st.peak, ImVec2(70.0f, 0.0f));
    ImGui::PopID();
  }
}

void MidiTab::drawDetail(const DeviceSnapshot& s) {
  if (ImGui::GetCurrentContext() == nullptr) return;
  if (device_ == nullptr) {
    ImGui::TextDisabled("No MIDI device attached.");
    return;
  }
  ImGui::Text("Channel Status Overview (Active Notes & Programs):");
  ImGui::Separator();
  for (int ch = 0; ch < device_->channelCount(); ++ch) {
    if (device_->isDormant(ch)) continue;
    const StripState st = stripState(s, ch);
    const int prog = device_->program(ch);
    std::string sounding_notes;
    for (int note = 0; note < MidiDevice::kNotesPerChannel; ++note) {
      if (device_->isNoteSounding(ch, note)) {
        if (!sounding_notes.empty()) sounding_notes += ", ";
        sounding_notes += std::to_string(note);
      }
    }
    ImGui::Text("CH%02d [%s]: %s | notes=%lu (%s) | peak=%.2f", ch,
                st.audible ? "ON" : "MUTED", programName(prog),
                static_cast<unsigned long>(st.active_notes),
                sounding_notes.empty() ? "-" : sounding_notes.c_str(),
                st.peak);
  }
}

void MidiTab::drawTracker(const DeviceSnapshot& s, const SimState* sim) {
  if (ImGui::GetCurrentContext() == nullptr) return;
  ImGui::Text("MIDI tracker: frame %u", s.frame);
  if (sim != nullptr) {
    ImGui::Text("Driver frame %u, %lu notes",
                sim->driver_frame,
                static_cast<unsigned long>(sim->notes.size()));
  } else {
    ImGui::TextDisabled("No sim state.");
  }
}

void MidiTab::onMuteClick(int ch) {
  if (device_ == nullptr) return;
  if (ch < 0 || ch >= device_->channelCount()) return;
  DeviceSnapshot s = device_->snapshot();
  const bool cur = s.channels[static_cast<std::size_t>(ch)].muted;
  device_->setMute(ch, !cur);
}

void MidiTab::pushWaveformData(int ch, const float* samples, std::size_t n) {
  // Fast escape on dormant channels (or bad input).
  if (samples == nullptr || n == 0) return;
  if (device_ != nullptr) {
    if (device_->isDormant(ch)) return;
    ensureWaveStorage(static_cast<std::size_t>(device_->channelCount()));
  } else {
    ensureWaveStorage(16);
  }
  storeWaveform(ch, samples, n);
}

}  // namespace audio_dbg
