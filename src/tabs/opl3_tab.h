// Stage 7.3: OPL3 concrete tab.
//
// `Opl3Tab` extends `FmTab` for the Yamaha YMF262 (NukedOPL3):
//   - 18 voices (0-8 Primary bank 0, 9-17 Secondary bank 1),
//   - per-voice MOD/CAR operator cards (MULT, TL, AR, DR, SL, RR, WS),
//   - algorithm connection routing via `FmTab::drawAlgorithm()` + feedback,
//   - per-voice carrier oscilloscopes with zero-crossing stabilization.
//
// Draw methods early-return without an ImGui context; the ImGui-free
// helpers (`voiceLabel()`, `bankName()`, `opParams()`,
// `findZeroCrossing()`) are unit-testable.
//
// See docs/current_plan_debug_frontend.md §7.3.

#ifndef PKMN_AUDIO_DBG_TABS_OPL3_TAB_H_
#define PKMN_AUDIO_DBG_TABS_OPL3_TAB_H_

#include <cstddef>
#include <string>

#include "devices/opl3_device.h"
#include "imgui.h"
#include "tabs/fm_tab.h"

namespace audio_dbg {

class Opl3Tab : public FmTab {
 public:
  Opl3Tab(int device_id = 10, std::string device_name = "OPL3")
      : FmTab(device_id, std::move(device_name)) {}
  ~Opl3Tab() override = default;

  void drawChannelStrips(const DeviceSnapshot& s) override;
  void drawDetail(const DeviceSnapshot& s) override;
  void drawTracker(const DeviceSnapshot& s, const SimState* sim) override;
  void onMuteClick(int ch) override;
  // Dormant-voice fast escape: pushes to dormant voices store nothing.
  void pushWaveformData(int ch, const float* samples, std::size_t n) override;

  // "Voice 1 (Primary)" .. "Voice 9 (Primary)",
  // "Voice 10 (Secondary)" .. "Voice 18 (Secondary)".
  // Out-of-range voices return "Invalid".
  const char* voiceLabel(int voice) const override;

  void setOpl3Device(Opl3Device* dev);
  Opl3Device* opl3Device() const { return opl3_; }

  static constexpr int kVoices = 18;

  // "Primary" (voices 0-8), "Secondary" (voices 9-17), else "Invalid".
  static const char* bankName(int voice);

  // Headless operator-parameter probe: copy of the MOD (carrier=false) or
  // CAR (carrier=true) params for `voice`. Returns defaults when no device
  // is attached or the voice is out of range.
  FmOperatorParams opParams(int voice, bool carrier) const;

  // Zero-crossing stabilization for carrier scopes: index of the first
  // positive-slope zero crossing (samples[i] <= 0 < samples[i+1]).
  // Returns 0 when there is none (or the input is unusable: null, < 2
  // samples), so the scope falls back to the raw buffer start.
  static std::size_t findZeroCrossing(const float* samples, std::size_t n);

  // Headless probe: buffered scope samples for a voice.
  std::size_t testWaveSize(int ch) const { return waveSize(ch); }

 private:
  Opl3Device* opl3_ = nullptr;
};

}  // namespace audio_dbg

#endif  // PKMN_AUDIO_DBG_TABS_OPL3_TAB_H_
