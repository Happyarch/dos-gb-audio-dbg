// Stage 4.6: ImfcTab implementation.
// Draw methods early-return without an ImGui context. See imfc_tab.h.

#include "tabs/imfc_tab.h"

#include <algorithm>
#include <cstdio>
#include <utility>

#include "imgui.h"

namespace audio_dbg {

ImfcTab::ImfcTab(int device_id, std::string device_name)
    : MidiTab(device_id, std::move(device_name)) {}

void ImfcTab::setImfcDevice(ImfcDevice* dev) {
  imfc_ = dev;
  MidiTab::setDevice(dev);
}

std::string ImfcTab::instrumentLabel(int inst) {
  if (inst < 0 || inst >= kInstruments) return "Invalid";
  return "Inst " + std::to_string(inst + 1) + " [Ch " +
         std::to_string(inst + 1) + "]";
}

const char* ImfcTab::programName(int program) const {
  if (imfc_ != nullptr && imfc_->synthCore() != nullptr) {
    return imfc_->synthCore()->presetName(2, program % 48);
  }
  return "Unknown";
}

ImfcTab::InstrumentStrip ImfcTab::instrumentStrip(const DeviceSnapshot& s,
                                                 int inst) const {
  InstrumentStrip is;
  is.inst = inst;
  is.channel = inst;
  if (is.channel < 0 || is.channel >= static_cast<int>(s.channels.size())) {
    return is;
  }
  const ChannelState& c = s.channels[static_cast<std::size_t>(is.channel)];
  is.muted = c.muted;
  is.soloed = c.soloed;
  is.dormant = c.dormant;
  is.peak = c.peak;
  is.audible = DeviceTab::channelAudible(s, is.channel);

  if (imfc_ != nullptr && imfc_->synthCore() != nullptr) {
    const auto* core = imfc_->synthCore();
    is.active = core->isInstrumentSounding(inst);
    is.sounding = static_cast<std::size_t>(core->activeNotes(inst));
    const auto& cfg = core->instrumentConfig(inst);
    is.bank = cfg.voiceBankNumber;
    is.program = cfg.voiceNumber;
    is.voice_name = imfc_->imfcPresetName(cfg.voiceBankNumber, cfg.voiceNumber);
  } else {
    is.active = !is.dormant;
  }
  return is;
}

void ImfcTab::onMuteClick(int ch) {
  if (imfc_ == nullptr) return;
  if (ch < 0 || ch >= imfc_->channelCount()) return;
  DeviceSnapshot s = imfc_->snapshot();
  const bool cur = s.channels[static_cast<std::size_t>(ch)].muted;
  imfc_->setMute(ch, !cur);
}

void ImfcTab::pushWaveformData(int ch, const float* samples, std::size_t n) {
  if (imfc_ != nullptr && imfc_->synthCore() != nullptr) {
    // Escape early if channel is inactive and silent
    if (!imfc_->synthCore()->isInstrumentSounding(ch)) {
      bool all_zero = true;
      for (std::size_t i = 0; i < n; ++i) {
        if (samples[i] != 0.0f) {
          all_zero = false;
          break;
        }
      }
      if (all_zero) return;
    }
  }
  MidiTab::pushWaveformData(ch, samples, n);
}

void ImfcTab::drawChannelStrips(const DeviceSnapshot& s) {
  if (ImGui::GetCurrentContext() == nullptr) return;
  if (imfc_ == nullptr) {
    ImGui::TextDisabled("No IBM Music Feature device attached.");
    return;
  }

  for (int inst = 0; inst < kInstruments; ++inst) {
    ImGui::PushID(inst);
    const InstrumentStrip is = instrumentStrip(s, inst);
    const std::string label = instrumentLabel(inst);

    // Activity LED
    if (is.active) {
      ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.2f, 1.0f), "[o]");
    } else {
      ImGui::TextDisabled("[.]");
    }
    ImGui::SameLine();

    // Mute button
    if (is.muted) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
    }
    if (ImGui::Button("M", ImVec2(22, 0))) {
      onMuteClick(inst);
    }
    if (is.muted) {
      ImGui::PopStyleColor();
    }
    ImGui::SameLine();

    // Solo button
    if (is.soloed) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.9f, 0.8f, 0.2f, 1.0f));
    }
    if (ImGui::Button("S", ImVec2(22, 0))) {
      imfc_->setSolo(inst, !is.soloed);
    }
    if (is.soloed) {
      ImGui::PopStyleColor();
    }
    ImGui::SameLine();

    // Select button for detailed inspection
    const bool is_selected = (selected_inst_ == inst);
    if (is_selected) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 1.0f, 1.0f));
    }
    if (ImGui::Button(label.c_str(), ImVec2(110, 0))) {
      selected_inst_ = inst;
    }
    if (is_selected) {
      ImGui::PopStyleColor();
    }
    ImGui::SameLine();

    // Bank and Voice readout
    const char* bank_name = "ROM";
    if (is.bank == 0) bank_name = "RAM-A";
    else if (is.bank == 1) bank_name = "RAM-B";
    else if (is.bank >= 2 && is.bank <= 6) {
      static const char* kRomBanks[] = {"ROM-1", "ROM-2", "ROM-3", "ROM-4", "ROM-5"};
      bank_name = kRomBanks[is.bank - 2];
    }
    ImGui::Text("[%s %02d] %-14s", bank_name, is.program + 1, is.voice_name.c_str());
    ImGui::SameLine();

    // Level / Pan
    if (imfc_->synthCore() != nullptr) {
      const auto& cfg = imfc_->synthCore()->instrumentConfig(inst);
      ImGui::Text("Vol:%3d Pan:%2d", cfg.outputLevel, cfg.pan);
    }
    ImGui::SameLine();

    // Peak meter
    const float peak_clamped = std::clamp(is.peak, 0.0f, 1.0f);
    ImGui::ProgressBar(peak_clamped, ImVec2(60, 0), "");

    ImGui::PopID();
  }
}

void ImfcTab::drawDetail(const DeviceSnapshot& /*s*/) {
  if (ImGui::GetCurrentContext() == nullptr) return;
  if (imfc_ == nullptr || imfc_->synthCore() == nullptr) {
    ImGui::TextDisabled("No IMFC device / synth core attached.");
    return;
  }

  auto* core = imfc_->synthCore();

  // --- Master Section ---
  ImGui::SeparatorText("IMFC Master Controls");
  int mvol = static_cast<int>(core->masterVolume());
  if (ImGui::SliderInt("Master Output Level", &mvol, 0, 127)) {
    core->setMasterVolume(static_cast<std::uint8_t>(mvol));
  }
  ImGui::SameLine();
  bool mprot = core->memoryProtect();
  if (ImGui::Checkbox("Memory Protect", &mprot)) {
    core->setMemoryProtect(mprot);
  }

  // --- Instrument Detail Section ---
  ImGui::SeparatorText("Instrument Inspector");
  ImGui::Text("Select Instrument:");
  ImGui::SameLine();
  for (int i = 0; i < kInstruments; ++i) {
    ImGui::PushID(100 + i);
    char btn_txt[16];
    std::snprintf(btn_txt, sizeof(btn_txt), "%d%s", i + 1,
                  core->isInstrumentSounding(i) ? "*" : "");
    if (selected_inst_ == i) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 1.0f, 1.0f));
    }
    if (ImGui::Button(btn_txt, ImVec2(30, 0))) {
      selected_inst_ = i;
    }
    if (selected_inst_ == i) {
      ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    ImGui::PopID();
  }
  ImGui::NewLine();

  const auto& cfg = core->instrumentConfig(selected_inst_);
  const auto& voice = core->activeVoice(selected_inst_);

  ImGui::Text("Instrument %d Configuration: Polyphony=%d notes | MIDI Ch=%d | Output Level=%d | Pan=%d",
              selected_inst_ + 1, cfg.numberOfNotes, cfg.midiChannel + 1, cfg.outputLevel, cfg.pan);

  std::string vname = voice.getName();
  while (!vname.empty() && vname.back() == ' ') vname.pop_back();
  ImGui::Text("Active Voice: \"%s\" (Bank %d, Prog %d) | Algorithm: %d | Feedback: %d",
              vname.c_str(), cfg.voiceBankNumber, cfg.voiceNumber + 1, voice.getAlgorithm(), voice.getFeedback());

  // 4-Operator Parameters Table
  if (ImGui::BeginTable("ImfcOpTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
    ImGui::TableSetupColumn("Parameter");
    ImGui::TableSetupColumn("Op 4 (Car)");
    ImGui::TableSetupColumn("Op 3");
    ImGui::TableSetupColumn("Op 2");
    ImGui::TableSetupColumn("Op 1 (Mod)");
    ImGui::TableHeadersRow();

    auto row_int = [&](const char* label, int v4, int v3, int v2, int v1) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::Text("%s", label);
      ImGui::TableSetColumnIndex(1);
      ImGui::Text("%d", v4);
      ImGui::TableSetColumnIndex(2);
      ImGui::Text("%d", v3);
      ImGui::TableSetColumnIndex(3);
      ImGui::Text("%d", v2);
      ImGui::TableSetColumnIndex(4);
      ImGui::Text("%d", v1);
    };

    const auto& op4 = voice.operators[3];
    const auto& op3 = voice.operators[2];
    const auto& op2 = voice.operators[1];
    const auto& op1 = voice.operators[0];

    row_int("Total Level (TL)", op4.getTotalLevel(), op3.getTotalLevel(), op2.getTotalLevel(), op1.getTotalLevel());
    row_int("Freq Multiplier (MULT)", op4.getMultiple(), op3.getMultiple(), op2.getMultiple(), op1.getMultiple());
    row_int("Detune (DT)", static_cast<int>(op4.getDetune()) - 3, static_cast<int>(op3.getDetune()) - 3,
            static_cast<int>(op2.getDetune()) - 3, static_cast<int>(op1.getDetune()) - 3);
    row_int("Attack Rate (AR)", op4.getAttackRate(), op3.getAttackRate(), op2.getAttackRate(), op1.getAttackRate());
    row_int("Decay 1 Rate (D1R)", op4.getDecay1Rate(), op3.getDecay1Rate(), op2.getDecay1Rate(), op1.getDecay1Rate());
    row_int("Decay 2 Rate (D2R)", op4.getDecay2Rate(), op3.getDecay2Rate(), op2.getDecay2Rate(), op1.getDecay2Rate());
    row_int("Sustain Level (SL)", op4.getSustainLevel(), op3.getSustainLevel(), op2.getSustainLevel(), op1.getSustainLevel());
    row_int("Release Rate (RR)", op4.getReleaseRate(), op3.getReleaseRate(), op2.getReleaseRate(), op1.getReleaseRate());

    ImGui::EndTable();
  }

  // --- RAM Custom Voice Bank Inspector ---
  if (ImGui::CollapsingHeader("RAM Custom Voice Bank (FB-01 User Voices)")) {
    ImGui::Text("RAM Bank 0 (User Bank A):");
    for (int slot = 0; slot < 48; ++slot) {
      const auto& cv = core->customVoice(0, slot);
      std::string name = cv.getName();
      while (!name.empty() && name.back() == ' ') name.pop_back();
      if (!name.empty() && name[0] != 0) {
        ImGui::BulletText("Slot %02d: %s (Alg %d, FB %d)", slot + 1, name.c_str(), cv.getAlgorithm(), cv.getFeedback());
      }
    }
  }
}

void ImfcTab::drawTracker(const DeviceSnapshot& /*s*/, const SimState* /*sim*/) {
  if (ImGui::GetCurrentContext() == nullptr) return;
  ImGui::TextDisabled("IMFC Tracker view not active.");
}

}  // namespace audio_dbg
