// Stage 7.5: General MIDI concrete tab.
//
// `GmTab` extends `MidiTab` for General MIDI (FluidSynth):
//   - 16 channel strips (Channels 1-16, 0-based 0-15), Channel 10 marked
//     "[Drums]".
//   - per-channel mute ("[M]") and solo ("[S]") toggles, GM program names
//     with a patch selector combo, volume/pan readouts (from CC7/CC10),
//     persistent piano-roll note bars (`MidiTab::drawNoteBar`,
//     velocity-graded), peak meters.
//
// Draw methods early-return without an ImGui context; the ImGui-free
// helpers (`channelLabel()`, `channelStrip()`) are unit-testable.
//
// See docs/current_plan_debug_frontend.md §7.5.

#ifndef PKMN_AUDIO_DBG_TABS_GM_TAB_H_
#define PKMN_AUDIO_DBG_TABS_GM_TAB_H_

#include <cstddef>
#include <string>

#include "devices/gm_device.h"
#include "imgui.h"
#include "tabs/midi_tab.h"

namespace audio_dbg {

class GmTab : public MidiTab {
 public:
  GmTab(int device_id = 13, std::string device_name = "General MIDI")
      : MidiTab(device_id, std::move(device_name)) {}
  ~GmTab() override = default;

  void drawChannelStrips(const DeviceSnapshot& s) override;
  void drawDetail(const DeviceSnapshot& s) override;
  void drawTracker(const DeviceSnapshot& s, const SimState* sim) override;
  void onMuteClick(int ch) override;
  // Dormant-channel fast escape: pushes to dormant channels store nothing.
  void pushWaveformData(int ch, const float* samples, std::size_t n) override;

  // General MIDI program names (e.g. "Acoustic Grand Piano").
  const char* programName(int program) const override;

  void setGmDevice(GmDevice* dev);
  GmDevice* gmDevice() const { return gm_; }

  static constexpr int kChannels = 16;
  static constexpr int kDrumChannel = 9;

  // "Ch 1" .. "Ch 16"; channel 9 (0-based) gains the drum marker:
  // "Ch 10 [Drums]".
  static std::string channelLabel(int ch);

  // ImGui-free per-channel summary for strips and unit checks.
  struct ChannelStrip {
    int channel = -1;
    bool muted = false;
    bool soloed = false;
    bool dormant = true;
    bool audible = false;
    std::size_t sounding = 0;
    int program = 0;
    float volume = 0.0f;  // CC7 / 127.
    float pan = 0.0f;     // (CC10 - 64) / 64, -1 (L) .. +1 (R).
    float peak = 0.0f;
  };
  ChannelStrip channelStrip(const DeviceSnapshot& s, int ch) const;

  // Solo toggle addressed by channel index.
  void onSoloClick(int ch);

  // Headless probe: buffered scope samples for a channel.
  std::size_t testWaveSize(int ch) const { return waveSize(ch); }

 private:
  GmDevice* gm_ = nullptr;
};

}  // namespace audio_dbg

#endif  // PKMN_AUDIO_DBG_TABS_GM_TAB_H_
