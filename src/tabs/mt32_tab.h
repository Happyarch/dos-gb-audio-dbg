// Stage 7.1/7.2: MT-32 concrete tab.
//
// `Mt32Tab` extends `MidiTab` for the Roland MT-32 (MUNT-QT parity):
//   - 9 part strips (Parts 1-8 listen on 0-based MIDI channels 1-8,
//     Rhythm on 0-based channel 9; channel 0 is part-less on factory
//     hardware, exactly as `Mt32Device` documents).
//   - per-part mute toggles, timbre names, persistent piano-roll note bars
//     (`MidiTab::drawNoteBar`, velocity-graded), peak meters.
//   - 32-slot partial-state 4x8 LED matrix from
//     `Mt32Device::partialState()` plus an active-partial count readout.
//   - stylized 20-char dot-matrix LCD from `Mt32Device::lcdText()`.
//
// Draw methods early-return without an ImGui context; the ImGui-free
// helpers (`partChannel()`, `partLabel()`, `partStrip()`,
// `partialColor()`, `activePartialCount()`, `lcdText()`) are unit-testable.
//
// See docs/current_plan_debug_frontend.md §7.1 / §7.2.

#ifndef PKMN_AUDIO_DBG_TABS_MT32_TAB_H_
#define PKMN_AUDIO_DBG_TABS_MT32_TAB_H_

#include <cstddef>
#include <string>

#include "devices/mt32_device.h"
#include "imgui.h"
#include "tabs/midi_tab.h"

namespace audio_dbg {

class Mt32Tab : public MidiTab {
 public:
  Mt32Tab(int device_id = 12, std::string device_name = "MT-32")
      : MidiTab(device_id, std::move(device_name)) {}
  ~Mt32Tab() override = default;

  void drawChannelStrips(const DeviceSnapshot& s) override;
  void drawDetail(const DeviceSnapshot& s) override;
  void drawTracker(const DeviceSnapshot& s, const SimState* sim) override;
  void onMuteClick(int ch) override;
  // Dormant-channel fast escape: pushes to dormant channels store nothing.
  void pushWaveformData(int ch, const float* samples, std::size_t n) override;

  // MT-32 timbre names (e.g. "AcouPiano1", "SynBass1").
  const char* programName(int program) const override;

  void setMt32Device(Mt32Device* dev);
  Mt32Device* mt32Device() const { return mt32_; }

  static constexpr int kParts = 9;
  static constexpr int kPartialSlots = 32;

  // Part 0-7 -> 0-based MIDI channel part+1; part 8 (rhythm) -> channel 9.
  // Returns -1 when out of range.
  static int partChannel(int part);
  // "Part 1 [Ch 2]" .. "Part 8 [Ch 9]", "Rhythm [Ch 10]".
  static std::string partLabel(int part);

  // ImGui-free per-part summary for strips and unit checks.
  struct PartStrip {
    int part = -1;
    int channel = -1;
    bool active = false;
    bool muted = false;
    bool dormant = true;
    bool audible = false;
    std::size_t sounding = 0;
    float peak = 0.0f;
  };
  PartStrip partStrip(const DeviceSnapshot& s, int part) const;

  // Partial-slot LED colour (ImGui-free, no context needed):
  //   0 INACTIVE dim gray, 1 ATTACK bright orange/red,
  //   2 SUSTAIN vibrant green, 3 RELEASE amber/yellow.
  static ImVec4 partialColor(int state);
  static const char* partialStateName(int state);
  // Null-safe single LED primitive.
  static void drawPartialCell(ImDrawList* dl, ImVec2 origin, float size,
                              int state);

  // -1 when no device is attached.
  int activePartialCount() const;
  std::string lcdText() const;

  // Mute toggle addressed by part index (maps onto the part's channel).
  void onPartMuteClick(int part);

  // MUNT-QT parity pitch scale drawing live part notes.
  void drawPartPitchScale(ImDrawList* dl, ImVec2 origin, ImVec2 size, int part);

  // Headless probe: buffered scope samples for a channel.
  std::size_t testWaveSize(int ch) const { return waveSize(ch); }

 private:
  Mt32Device* mt32_ = nullptr;
};

}  // namespace audio_dbg

#endif  // PKMN_AUDIO_DBG_TABS_MT32_TAB_H_
