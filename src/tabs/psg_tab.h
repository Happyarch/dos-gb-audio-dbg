// Stage 2.4: Tier-2C PSG intermediate tab base.
//
// `PsgTab` extends `DeviceTab` for PSG tabs (GB-APU, future SSI-2001/SID).
// It renders channel cards, the pulse duty-cycle visualizer, the 32-nibble
// wave-RAM bar graph, and the LFSR noise-mode indicator on top of a
// `PsgDevice`.
//
// Draw methods early-return without an ImGui context; the ImGui-free
// helpers (`channelTypeName()`, `dutyFraction()`) are unit-testable.
//
// See docs/current_plan_debug_frontend.md §2.4.

#ifndef PKMN_AUDIO_DBG_TABS_PSG_TAB_H_
#define PKMN_AUDIO_DBG_TABS_PSG_TAB_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "devices/psg_device.h"
#include "imgui.h"
#include "tabs/device_tab.h"

namespace audio_dbg {

class PsgTab : public DeviceTab {
 public:
  using DeviceTab::DeviceTab;
  ~PsgTab() override = default;

  void drawChannelStrips(const DeviceSnapshot& s) override;
  void drawDetail(const DeviceSnapshot& s) override;
  void drawTracker(const DeviceSnapshot& s, const SimState* sim) override;
  void onMuteClick(int ch) override;
  // Dormant-channel fast escape: pushes to dormant channels store nothing.
  void pushWaveformData(int ch, const float* samples, std::size_t n) override;

  void setDevice(PsgDevice* dev) { device_ = dev; }
  PsgDevice* device() const { return device_; }

  // ImGui-free helpers (unit-testable).
  static const char* channelTypeName(PsgChannelType type);
  // Duty index 0..3 -> 0.125 / 0.25 / 0.5 / 0.75.
  static float dutyFraction(int duty);
  // Null-safe ImDrawList primitives.
  static void drawDutyBar(ImDrawList* dl, ImVec2 origin, float width,
                          float height, int duty);
  static void drawWaveRam(ImDrawList* dl, ImVec2 origin, float width,
                          float height,
                          const std::array<std::uint8_t, 32>& ram);

  // --- Backend hook (pure virtual: envelope naming differs per chip) ---
  virtual const char* envelopeName(int envelope) const = 0;

 private:
  PsgDevice* device_ = nullptr;
};

}  // namespace audio_dbg

#endif  // PKMN_AUDIO_DBG_TABS_PSG_TAB_H_
