// Stage 7.3: Opl3Tab implementation.
// Draw methods early-return without an ImGui context. See opl3_tab.h.

#include "tabs/opl3_tab.h"

#include <cstdio>

#include "imgui.h"

namespace audio_dbg {

namespace {

constexpr const char* kVoiceLabels[Opl3Tab::kVoices] = {
    "Voice 1 (Primary)",   "Voice 2 (Primary)",   "Voice 3 (Primary)",
    "Voice 4 (Primary)",   "Voice 5 (Primary)",   "Voice 6 (Primary)",
    "Voice 7 (Primary)",   "Voice 8 (Primary)",   "Voice 9 (Primary)",
    "Voice 10 (Secondary)", "Voice 11 (Secondary)", "Voice 12 (Secondary)",
    "Voice 13 (Secondary)", "Voice 14 (Secondary)", "Voice 15 (Secondary)",
    "Voice 16 (Secondary)", "Voice 17 (Secondary)", "Voice 18 (Secondary)",
};

}  // namespace

const char* Opl3Tab::bankName(int voice) {
  if (voice >= 0 && voice < 9) return "Primary";
  if (voice >= 9 && voice < kVoices) return "Secondary";
  return "Invalid";
}

const char* Opl3Tab::voiceLabel(int voice) const {
  if (voice < 0 || voice >= kVoices) return "Invalid";
  return kVoiceLabels[voice];
}

void Opl3Tab::setOpl3Device(Opl3Device* dev) {
  opl3_ = dev;
  FmTab::setDevice(dev);
}

FmOperatorParams Opl3Tab::opParams(int voice, bool carrier) const {
  FmOperatorParams p;
  if (opl3_ == nullptr) return p;
  if (voice < 0 || voice >= opl3_->voiceCount()) return p;
  const FmVoice& v = opl3_->voice(voice);
  return carrier ? v.car : v.mod;
}

std::size_t Opl3Tab::findZeroCrossing(const float* samples, std::size_t n) {
  if (samples == nullptr || n < 2) return 0;
  for (std::size_t i = 0; i + 1 < n; ++i) {
    if (samples[i] <= 0.0f && samples[i + 1] > 0.0f) return i + 1;
  }
  return 0;
}

void Opl3Tab::onMuteClick(int ch) {
  if (opl3_ == nullptr) return;
  if (ch < 0 || ch >= opl3_->channelCount()) return;
  DeviceSnapshot s = opl3_->snapshot();
  const bool cur = s.channels[static_cast<std::size_t>(ch)].muted;
  opl3_->setMute(ch, !cur);
}

void Opl3Tab::pushWaveformData(int ch, const float* samples, std::size_t n) {
  // Fast escape on dormant voices (or bad input).
  if (samples == nullptr || n == 0) return;
  if (opl3_ != nullptr) {
    if (opl3_->isVoiceDormant(ch)) return;
    ensureWaveStorage(static_cast<std::size_t>(opl3_->voiceCount()));
  } else {
    ensureWaveStorage(static_cast<std::size_t>(kVoices));
  }
  storeWaveform(ch, samples, n);
}

void Opl3Tab::drawChannelStrips(const DeviceSnapshot& s) {
  if (ImGui::GetCurrentContext() == nullptr) return;
  if (opl3_ == nullptr) {
    ImGui::TextDisabled("No OPL3 device attached.");
    return;
  }
  // 18-voice grid: dormant voices take the fast escape (label only).
  for (int v = 0; v < kVoices; ++v) {
    if (v >= static_cast<int>(s.channels.size())) break;
    const ChannelState& c = s.channels[static_cast<std::size_t>(v)];
    if (c.dormant) {
      ImGui::TextDisabled("%s dormant", voiceLabel(v));
      continue;
    }
    const VoiceView vv = voiceView(v);
    ImGui::Text("%s %s env=%.2f %s", voiceLabel(v),
                vv.keyed_on ? "KEY" : "off", vv.env_vol,
                vv.connection == 0 ? "SerialFM" : "Parallel");
    ImGui::SameLine();
    if (ImGui::SmallButton(muteLabel(c.muted))) onMuteClick(v);
  }
}

void Opl3Tab::drawDetail(const DeviceSnapshot& s) {
  if (ImGui::GetCurrentContext() == nullptr) return;
  if (opl3_ == nullptr) {
    ImGui::TextDisabled("No OPL3 device attached.");
    return;
  }
  // 18-voice operator matrix: MOD + CAR cards per live voice, algorithm
  // routing via drawAlgorithm(), feedback readout, carrier scope with
  // zero-crossing stabilization.
  int shown = 0;
  for (int v = 0; v < opl3_->voiceCount(); ++v) {
    const FmVoice& vs = opl3_->voice(v);
    if (vs.is_dormant) continue;
    ImGui::Separator();
    ImGui::Text("%s fnum=%d blk=%d %s fb=%d %s", voiceLabel(v), vs.fnum,
                vs.block, envelopePhaseName(vs.keyed_on, vs.env_vol),
                vs.feedback, vs.connection == 0 ? "Serial FM" : "Parallel");
    ImGui::Text("MOD mult=%d tl=%d ar=%d dr=%d sl=%d rr=%d ws=%d",
                vs.mod.mult, vs.mod.tl, vs.mod.ar, vs.mod.dr, vs.mod.sl,
                vs.mod.rr, vs.mod.ws);
    ImGui::Text("CAR mult=%d tl=%d ar=%d dr=%d sl=%d rr=%d ws=%d",
                vs.car.mult, vs.car.tl, vs.car.ar, vs.car.dr, vs.car.sl,
                vs.car.rr, vs.car.ws);
    ImVec2 origin = ImGui::GetCursorScreenPos();
    drawAlgorithm(ImGui::GetWindowDrawList(), origin, 64.0f, vs.connection,
                  vs.feedback);
    ImGui::Dummy(ImVec2(80.0f, 70.0f));
    // Carrier oscilloscope: stabilized on the first positive zero crossing.
    float buf[256];
    const std::size_t got = copyWaveform(v, buf, 256);
    if (got >= 2) {
      const std::size_t start = findZeroCrossing(buf, got);
      const float* base = buf + (start < got ? start : 0);
      const int count =
          static_cast<int>(got - (start < got ? start : 0));
      char scope_id[32];
      std::snprintf(scope_id, sizeof(scope_id), "##opl3scope%d", v);
      ImGui::PlotLines(scope_id, base, count, 0, nullptr, -1.0f, 1.0f,
                       ImVec2(220.0f, 48.0f));
    } else {
      ImGui::TextDisabled("Scope: no data");
    }
    ++shown;
  }
  if (shown == 0) ImGui::TextDisabled("All voices dormant.");
  (void)s;
}

void Opl3Tab::drawTracker(const DeviceSnapshot& s, const SimState* sim) {
  if (ImGui::GetCurrentContext() == nullptr) return;
  ImGui::Text("OPL3 tracker: frame %u", s.frame);
  if (sim != nullptr) {
    ImGui::Text("Driver frame %u, %lu notes", sim->driver_frame,
                static_cast<unsigned long>(sim->notes.size()));
  } else {
    ImGui::TextDisabled("No sim state.");
  }
}

}  // namespace audio_dbg
