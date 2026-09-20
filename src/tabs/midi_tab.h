// Stage 2.2: Tier-2A MIDI intermediate tab base.
//
// `MidiTab` extends `DeviceTab` for all MIDI-type tabs (MT-32, GM, future
// FB-01). It implements the MUNT-QT style channel strips (mute toggles,
// patch names, persistent piano-roll note bars with velocity grading
// RGB(2*vel, 255-2*vel, 0)) on top of a `MidiDevice`.
//
// The ImDrawList helper `drawNoteBar()` is null-safe (early return) so
// headless unit checks can link it without an ImGui context.
//
// See docs/current_plan_debug_frontend.md §2.2.

#ifndef PKMN_AUDIO_DBG_TABS_MIDI_TAB_H_
#define PKMN_AUDIO_DBG_TABS_MIDI_TAB_H_

#include <cstddef>
#include <string>

#include "devices/midi_device.h"
#include "devices/sound_device.h"
#include "imgui.h"
#include "tabs/device_tab.h"

namespace audio_dbg {

class MidiTab : public DeviceTab {
 public:
  using DeviceTab::DeviceTab;
  ~MidiTab() override = default;

  void drawChannelStrips(const DeviceSnapshot& s) override;
  void drawDetail(const DeviceSnapshot& s) override;
  void drawTracker(const DeviceSnapshot& s, const SimState* sim) override;
  void onMuteClick(int ch) override;
  // Dormant-channel fast escape: muted/dormant pushes store nothing.
  void pushWaveformData(int ch, const float* samples, std::size_t n) override;

  void setDevice(MidiDevice* dev) { device_ = dev; }
  MidiDevice* device() const { return device_; }

  // ImGui-free strip summary used by drawChannelStrips() and unit checks.
  struct StripState {
    bool audible = false;
    bool muted = false;
    bool soloed = false;
    bool dormant = true;
    std::size_t active_notes = 0;
    float peak = 0.0f;
  };
  StripState stripState(const DeviceSnapshot& s, int ch) const;

  // MUNT-QT velocity-graded piano-roll bar. Null-safe: returns when
  // `dl == nullptr`. Colour matches DeviceTab::velocityColor().
  static void drawNoteBar(ImDrawList* dl, ImVec2 origin, float width,
                          float row_h, int velocity, bool sounding);

  // MUNT-QT musical keyboard pitch scale bar showing active sounding notes.
  static void drawPitchScale(ImDrawList* dl, ImVec2 origin, ImVec2 size,
                             MidiDevice* dev, int ch);

  // --- Backend hook (pure virtual: patch-name set differs per device) ---
  // MT-32 returns Timbre names, GM returns General MIDI program names.
  virtual const char* programName(int program) const = 0;

 private:
  MidiDevice* device_ = nullptr;
};

}  // namespace audio_dbg

#endif  // PKMN_AUDIO_DBG_TABS_MIDI_TAB_H_
