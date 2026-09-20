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
  // MUNT-QT style strips: one row per MIDI channel with mute toggle,
  // patch name, and active-note count. Dormant channels take the fast
  // escape (label only, no bar work).
  for (std::size_t i = 0; i < s.channels.size(); ++i) {
    const int ch = static_cast<int>(i);
    const ChannelState& c = s.channels[i];
    if (c.dormant) {
      ImGui::TextDisabled("CH%02d dormant", ch);
      continue;
    }
    const StripState st = stripState(s, ch);
    int prog = (device_ != nullptr) ? device_->program(ch) : 0;
    ImGui::Text("CH%02d %s %s", ch, programName(prog),
                st.audible ? "on" : "muted");
    ImGui::SameLine();
    if (ImGui::SmallButton(muteLabel(c.muted))) onMuteClick(ch);
    ImGui::SameLine();
    ImGui::Text("notes=%lu peak=%.2f", static_cast<unsigned long>(st.active_notes),
                st.peak);
  }
}

void MidiTab::drawDetail(const DeviceSnapshot& s) {
  if (ImGui::GetCurrentContext() == nullptr) return;
  if (device_ == nullptr) {
    ImGui::TextDisabled("No MIDI device attached.");
    return;
  }
  // Persistent piano-roll bars from the device note history.
  const std::vector<MidiNoteEvent>& hist = device_->noteHistory();
  ImGui::Text("Note history: %lu events", static_cast<unsigned long>(hist.size()));
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
  (void)s;
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
