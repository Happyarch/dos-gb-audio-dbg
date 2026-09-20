// Stage 2.3: FmTab implementation.
// Draw methods early-return without an ImGui context. See fm_tab.h.

#include "tabs/fm_tab.h"

#include "imgui.h"

namespace audio_dbg {

FmTab::VoiceView FmTab::voiceView(int voice) const {
  VoiceView v;
  if (device_ == nullptr) return v;
  if (voice < 0 || voice >= device_->voiceCount()) return v;
  const FmVoice& vs = device_->voice(voice);
  v.dormant = vs.is_dormant;
  v.keyed_on = vs.keyed_on;
  v.env_vol = vs.env_vol;
  v.connection = vs.connection;
  v.feedback = vs.feedback;
  return v;
}

const char* FmTab::envelopePhaseName(bool keyed_on, float env_vol) {
  if (!keyed_on) return "RELEASE";
  if (env_vol >= 0.99f) return "ATTACK";
  if (env_vol > 0.0f) return "SUSTAIN";
  return "IDLE";
}

void FmTab::drawAlgorithm(ImDrawList* dl, ImVec2 origin, float size,
                          int connection, int feedback) {
  if (dl == nullptr || size <= 0.0f) return;
  // Two operator boxes (MOD left, CAR right); series vs parallel routing
  // selected by the connection bit; feedback shown as loop count text.
  const ImU32 box = ImGui::ColorConvertFloat4ToU32(ImVec4(0.2f, 0.6f, 1.0f, 1.0f));
  const ImU32 line = ImGui::ColorConvertFloat4ToU32(ImVec4(0.8f, 0.8f, 0.8f, 1.0f));
  const float bw = size * 0.35f;
  const float bh = size * 0.4f;
  const ImVec2 mod0(origin.x, origin.y);
  const ImVec2 car0(origin.x + size - bw, origin.y);
  dl->AddRect(mod0, ImVec2(mod0.x + bw, mod0.y + bh), box);
  dl->AddRect(car0, ImVec2(car0.x + bw, car0.y + bh), box);
  const ImVec2 mod_out(mod0.x + bw, mod0.y + bh * 0.5f);
  const ImVec2 car_in(car0.x, car0.y + bh * 0.5f);
  if (connection == 0) {
    // Series: MOD feeds CAR.
    dl->AddLine(mod_out, car_in, line, 1.5f);
    dl->AddLine(ImVec2(car0.x + bw, car0.y + bh * 0.5f),
                ImVec2(car0.x + bw + size * 0.1f, car0.y + bh * 0.5f), line,
                1.5f);
  } else {
    // Parallel: both feed output.
    dl->AddLine(ImVec2(mod0.x + bw * 0.5f, mod0.y + bh),
                ImVec2(mod0.x + bw * 0.5f, mod0.y + bh + size * 0.2f), line,
                1.5f);
    dl->AddLine(ImVec2(car0.x + bw * 0.5f, car0.y + bh),
                ImVec2(car0.x + bw * 0.5f, car0.y + bh + size * 0.2f), line,
                1.5f);
  }
  (void)feedback;
}

void FmTab::drawChannelStrips(const DeviceSnapshot& s) {
  if (ImGui::GetCurrentContext() == nullptr) return;
  // Voice grid: one cell per voice, dormant cells take the fast escape.
  for (std::size_t i = 0; i < s.channels.size(); ++i) {
    const int v = static_cast<int>(i);
    if (s.channels[i].dormant) {
      ImGui::TextDisabled("V%02d dormant", v);
      continue;
    }
    const VoiceView vv = voiceView(v);
    ImGui::Text("V%02d %s %s env=%.2f", v, voiceLabel(v),
                vv.keyed_on ? "KEY" : "off", vv.env_vol);
    ImGui::SameLine();
    if (ImGui::SmallButton(muteLabel(s.channels[i].muted))) onMuteClick(v);
  }
}

void FmTab::drawDetail(const DeviceSnapshot& s) {
  if (ImGui::GetCurrentContext() == nullptr) return;
  if (device_ == nullptr) {
    ImGui::TextDisabled("No FM device attached.");
    return;
  }
  // Operator matrix cards (MOD/CAR parameters) for the first live voices.
  int shown = 0;
  for (int v = 0; v < device_->voiceCount() && shown < 4; ++v) {
    const FmVoice& vs = device_->voice(v);
    if (vs.is_dormant) continue;
    ImGui::Text("V%d MOD mult=%d ar=%d dr=%d sl=%d rr=%d tl=%d", v, vs.mod.mult,
                vs.mod.ar, vs.mod.dr, vs.mod.sl, vs.mod.rr, vs.mod.tl);
    ImGui::Text("V%d CAR mult=%d ar=%d dr=%d sl=%d rr=%d tl=%d ws=%d", v,
                vs.car.mult, vs.car.ar, vs.car.dr, vs.car.sl, vs.car.rr,
                vs.car.tl, vs.car.ws);
    ImVec2 origin = ImGui::GetCursorScreenPos();
    drawAlgorithm(ImGui::GetWindowDrawList(), origin, 64.0f, vs.connection,
                  vs.feedback);
    ImGui::Dummy(ImVec2(80.0f, 70.0f));
    ++shown;
  }
  if (shown == 0) ImGui::TextDisabled("All voices dormant.");
  (void)s;
}

void FmTab::drawTracker(const DeviceSnapshot& s, const SimState* sim) {
  if (ImGui::GetCurrentContext() == nullptr) return;
  ImGui::Text("FM tracker: frame %u", s.frame);
  if (sim != nullptr) {
    ImGui::Text("Driver frame %u, %lu notes", sim->driver_frame,
                static_cast<unsigned long>(sim->notes.size()));
  } else {
    ImGui::TextDisabled("No sim state.");
  }
}

void FmTab::onMuteClick(int ch) {
  if (device_ == nullptr) return;
  if (ch < 0 || ch >= device_->channelCount()) return;
  DeviceSnapshot s = device_->snapshot();
  const bool cur = s.channels[static_cast<std::size_t>(ch)].muted;
  device_->setMute(ch, !cur);
}

void FmTab::pushWaveformData(int ch, const float* samples, std::size_t n) {
  if (samples == nullptr || n == 0) return;
  if (device_ != nullptr) {
    if (device_->isVoiceDormant(ch)) return;
    ensureWaveStorage(static_cast<std::size_t>(device_->voiceCount()));
  } else {
    ensureWaveStorage(32);
  }
  storeWaveform(ch, samples, n);
}

}  // namespace audio_dbg
