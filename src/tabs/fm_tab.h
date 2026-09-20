// Stage 2.3: Tier-2B FM intermediate tab base.
//
// `FmTab` extends `DeviceTab` for operator-FM tabs (OPL3, future OPL2/OPM).
// It renders the voice grid, MOD/CAR operator matrix cards, the algorithm
// routing visualizer, and carrier scopes on top of an `FmDevice`.
//
// Draw methods early-return without an ImGui context; the ImGui-free
// helpers (`voiceView()`, `envelopePhaseName()`) are unit-testable.
//
// See docs/current_plan_debug_frontend.md §2.3.

#ifndef PKMN_AUDIO_DBG_TABS_FM_TAB_H_
#define PKMN_AUDIO_DBG_TABS_FM_TAB_H_

#include <cstddef>
#include <string>

#include "devices/fm_device.h"
#include "imgui.h"
#include "tabs/device_tab.h"

namespace audio_dbg {

class FmTab : public DeviceTab {
 public:
  using DeviceTab::DeviceTab;
  ~FmTab() override = default;

  void drawChannelStrips(const DeviceSnapshot& s) override;
  void drawDetail(const DeviceSnapshot& s) override;
  void drawTracker(const DeviceSnapshot& s, const SimState* sim) override;
  void onMuteClick(int ch) override;
  // Dormant-voice fast escape: pushes to dormant voices store nothing.
  void pushWaveformData(int ch, const float* samples, std::size_t n) override;

  void setDevice(FmDevice* dev) { device_ = dev; }
  FmDevice* device() const { return device_; }

  // ImGui-free per-voice summary for the grid and unit checks.
  struct VoiceView {
    bool dormant = true;
    bool keyed_on = false;
    float env_vol = 0.0f;
    int connection = 0;
    int feedback = 0;
  };
  VoiceView voiceView(int voice) const;

  // Software envelope phase name from key/envelope state.
  static const char* envelopePhaseName(bool keyed_on, float env_vol);

  // Algorithm routing visualizer primitive (null-safe).
  static void drawAlgorithm(ImDrawList* dl, ImVec2 origin, float size,
                            int connection, int feedback);

  // --- Backend hook (pure virtual: voice labelling differs per chip) ---
  virtual const char* voiceLabel(int voice) const = 0;

 private:
  FmDevice* device_ = nullptr;
};

}  // namespace audio_dbg

#endif  // PKMN_AUDIO_DBG_TABS_FM_TAB_H_
