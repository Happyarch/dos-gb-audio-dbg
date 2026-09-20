// Stage 7.1/7.2: Mt32Tab implementation.
// Draw methods early-return without an ImGui context. See mt32_tab.h.

#include "tabs/mt32_tab.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

#include "imgui.h"

namespace audio_dbg {

const char* Mt32Tab::programName(int program) const {
  return Mt32Device::mt32TimbreName(program);
}

void Mt32Tab::setMt32Device(Mt32Device* dev) {
  mt32_ = dev;
  MidiTab::setDevice(dev);
}

int Mt32Tab::partChannel(int part) {
  if (part < 0 || part >= kParts) return -1;
  if (part == 8) return Mt32Device::kRhythmChannel;
  return part + 1;
}

std::string Mt32Tab::partLabel(int part) {
  if (part < 0 || part >= kParts) return "Invalid";
  if (part == 8) return "Rhythm [Ch 10]";
  return "Part " + std::to_string(part + 1) + " [Ch " +
         std::to_string(part + 2) + "]";
}

Mt32Tab::PartStrip Mt32Tab::partStrip(const DeviceSnapshot& s,
                                      int part) const {
  PartStrip ps;
  ps.part = part;
  ps.channel = partChannel(part);
  if (ps.channel < 0) return ps;
  if (ps.channel >= static_cast<int>(s.channels.size())) return ps;
  const ChannelState& c =
      s.channels[static_cast<std::size_t>(ps.channel)];
  ps.muted = c.muted;
  ps.dormant = c.dormant;
  ps.peak = c.peak;
  ps.audible = DeviceTab::channelAudible(s, ps.channel);
  if (mt32_ != nullptr && ps.channel < mt32_->channelCount()) {
    ps.sounding = mt32_->activeNoteCount(ps.channel);
    ps.active = mt32_->partActive(part);
  } else {
    ps.active = !ps.dormant && ps.sounding > 0;
  }
  return ps;
}

ImVec4 Mt32Tab::partialColor(int state) {
  switch (state) {
    case 1:  // ATTACK: bright orange/red.
      return ImVec4(1.0f, 0.35f, 0.0f, 1.0f);
    case 2:  // SUSTAIN: vibrant green.
      return ImVec4(0.0f, 1.0f, 0.2f, 1.0f);
    case 3:  // RELEASE: amber/yellow.
      return ImVec4(1.0f, 0.85f, 0.0f, 1.0f);
    case 0:  // INACTIVE: dim gray (also the out-of-range fallback).
    default:
      return ImVec4(0.15f, 0.15f, 0.15f, 1.0f);
  }
}

const char* Mt32Tab::partialStateName(int state) {
  switch (state) {
    case 0:
      return "INACTIVE";
    case 1:
      return "ATTACK";
    case 2:
      return "SUSTAIN";
    case 3:
      return "RELEASE";
    default:
      return "UNKNOWN";
  }
}

void Mt32Tab::drawPartialCell(ImDrawList* dl, ImVec2 origin, float size,
                              int state) {
  if (dl == nullptr || size <= 0.0f) return;
  const ImU32 col =
      ImGui::ColorConvertFloat4ToU32(partialColor(state));
  dl->AddRectFilled(origin, ImVec2(origin.x + size, origin.y + size), col);
}

int Mt32Tab::activePartialCount() const {
  if (mt32_ == nullptr) return -1;
  return mt32_->activePartialCount();
}

std::string Mt32Tab::lcdText() const {
  if (mt32_ == nullptr) return "";
  return mt32_->lcdText();
}

void Mt32Tab::onMuteClick(int ch) {
  if (mt32_ == nullptr) return;
  if (ch < 0 || ch >= mt32_->channelCount()) return;
  DeviceSnapshot s = mt32_->snapshot();
  const bool cur = s.channels[static_cast<std::size_t>(ch)].muted;
  mt32_->setMute(ch, !cur);
}

void Mt32Tab::onPartMuteClick(int part) {
  const int ch = partChannel(part);
  if (ch < 0) return;
  onMuteClick(ch);
}

void Mt32Tab::drawChannelStrips(const DeviceSnapshot& s) {
  if (ImGui::GetCurrentContext() == nullptr) return;
  if (mt32_ == nullptr) {
    ImGui::TextDisabled("No MT-32 device attached.");
    return;
  }
  // MUNT-QT parity: 9 rows (Parts 1-8 + Rhythm). Channel 0 is part-less
  // on factory hardware and gets no strip. Dormant parts take the fast
  // escape (label only, no bar work).
  for (int part = 0; part < kParts; ++part) {
    ImGui::PushID(part);
    const PartStrip ps = partStrip(s, part);
    const std::string label = partLabel(part);
    if (ps.dormant) {
      ImGui::TextDisabled("%s dormant", label.c_str());
      ImGui::PopID();
      continue;
    }
    // Activity LED: green when the part is active, dim otherwise.
    if (ps.active) {
      ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.2f, 1.0f), "[o]");
    } else {
      ImGui::TextDisabled("[.]");
    }
    ImGui::SameLine();
    ImGui::Text("%s", label.c_str());
    ImGui::SameLine();
    // Mute toggle, highlighted while muted.
    const bool muted = ps.muted;
    if (muted) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.2f, 0.1f, 1.0f));
    if (ImGui::SmallButton("[M]")) onMuteClick(ps.channel);
    if (muted) ImGui::PopStyleColor();
    ImGui::SameLine();
    const int prog = mt32_->program(ps.channel);
    // Shadow-snapshot read (never the live synth): safe on the UI thread.
    const std::string cur_patch = mt32_->patchName(part);
    // Timbre selector combo (128 factory timbres).
    char combo_id[32];
    std::snprintf(combo_id, sizeof(combo_id), "##mt32prog%d", part);
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::BeginCombo(combo_id, cur_patch.c_str())) {
      for (int p = 0; p < 128; ++p) {
        const bool selected = (p == prog);
        if (ImGui::Selectable(programName(p), selected)) {
          mt32_->dispatchProgramChange(ps.channel, p);
        }
        if (selected) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    // Musical keyboard pitch scale bar showing active sounding notes (MUNT-QT parity).
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    constexpr float kBarW = 240.0f;
    constexpr float kBarH = 16.0f;
    drawPartPitchScale(dl, origin, ImVec2(kBarW, kBarH), part);
    ImGui::Dummy(ImVec2(kBarW, kBarH));
    ImGui::SameLine();
    // Peak level meter.
    ImGui::ProgressBar(ps.peak, ImVec2(70.0f, 0.0f));
    ImGui::PopID();
  }
}

void Mt32Tab::drawPartPitchScale(ImDrawList* dl, ImVec2 origin, ImVec2 size,
                                 int part) {
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

  if (mt32_ == nullptr || part < 0 || part >= kParts) return;

  std::uint8_t keys[32];
  std::uint8_t vels[32];
  const int count = mt32_->getPlayingNotes(part, keys, vels);
  constexpr float kMarkerW = 4.0f;
  for (int i = 0; i < count; ++i) {
    int clamped = keys[i];
    if (clamped < kMinNote) clamped = kMinNote;
    if (clamped > kMaxNote) clamped = kMaxNote;
    const float frac = static_cast<float>(clamped - kMinNote) /
                       static_cast<float>(kRange);
    const float mx = origin.x + frac * (size.x - kMarkerW);
    const int vel = vels[i];
    const float r = std::clamp(2.0f * static_cast<float>(vel) / 255.0f, 0.0f, 1.0f);
    const float g =
        std::clamp((255.0f - 2.0f * static_cast<float>(vel)) / 255.0f, 0.0f, 1.0f);
    const ImU32 marker_col = ImGui::ColorConvertFloat4ToU32(
        ImVec4(r, g, 0.0f, 0.95f));
    dl->AddRectFilled(ImVec2(mx, origin.y + 1.0f),
                      ImVec2(mx + kMarkerW, origin.y + size.y - 1.0f),
                      marker_col, 1.0f);
  }
}

void Mt32Tab::drawDetail(const DeviceSnapshot& s) {
  if (ImGui::GetCurrentContext() == nullptr) return;
  if (mt32_ == nullptr) {
    ImGui::TextDisabled("No MT-32 device attached.");
    return;
  }
  // 20-char dot-matrix LCD: green monospace on near-black with a border.
  ImGui::Text("LCD");
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.02f, 0.08f, 0.03f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 1.0f, 0.45f, 1.0f));
  if (ImGui::BeginChild("##mt32lcd", ImVec2(340.0f, 30.0f), true)) {
    const std::string lcd = lcdText();
    ImGui::TextUnformatted(lcd.c_str());
  }
  ImGui::EndChild();
  ImGui::PopStyleColor(2);

  // 32-slot partial-state 4x8 LED matrix + numeric readout.
  const int active = activePartialCount();
  ImGui::Text("Partials: %d / %d", active, kPartialSlots);
  for (int row = 0; row < 4; ++row) {
    for (int col = 0; col < 8; ++col) {
      const int slot = row * 8 + col;
      const int state = mt32_->partialState(slot);
      ImGui::PushID(slot);
      ImGui::ColorButton("##partial", partialColor(state),
                         ImGuiColorEditFlags_NoTooltip, ImVec2(18.0f, 18.0f));
      ImGui::PopID();
      if (col < 7) ImGui::SameLine();
    }
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("0=INACTIVE 1=ATTACK 2=SUSTAIN 3=RELEASE");
  }
  (void)s;
}

void Mt32Tab::drawTracker(const DeviceSnapshot& s, const SimState* sim) {
  if (ImGui::GetCurrentContext() == nullptr) return;
  ImGui::Text("MT-32 tracker: frame %u", s.frame);
  if (sim != nullptr) {
    ImGui::Text("Driver frame %u, %lu notes", sim->driver_frame,
                static_cast<unsigned long>(sim->notes.size()));
  } else {
    ImGui::TextDisabled("No sim state.");
  }
}

void Mt32Tab::pushWaveformData(int ch, const float* samples, std::size_t n) {
  // Fast escape on dormant channels (or bad input).
  if (samples == nullptr || n == 0) return;
  if (mt32_ != nullptr) {
    if (mt32_->isDormant(ch)) return;
    ensureWaveStorage(static_cast<std::size_t>(mt32_->channelCount()));
  } else {
    ensureWaveStorage(16);
  }
  storeWaveform(ch, samples, n);
}

}  // namespace audio_dbg
