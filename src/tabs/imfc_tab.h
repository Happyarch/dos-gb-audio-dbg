// Stage 4.6: Concrete IMFC tab for dgad.
//
// `ImfcTab` extends `MidiTab` for the IBM Music Feature Card (Yamaha FB-01):
//   - 8 instrument strips (Inst 1-8 listening on 0-based MIDI channels 0-7).
//   - Per-instrument mute/solo toggles, active sounding LED, preset name
//     (ROM 1..5 / RAM A..B), volume/pan, note bars, and peak meters.
//   - Detailed 4-operator FM voice inspection (AR, D1R, D1L, D2R, RR, TL, MULT,
//     DT, Algorithm, Feedback).
//   - Master volume, memory protect status, and RAM custom voice bank inspector.
//
// Draw methods early-return without an ImGui context; the ImGui-free
// helpers are unit-testable.

#ifndef PKMN_AUDIO_DBG_TABS_IMFC_TAB_H_
#define PKMN_AUDIO_DBG_TABS_IMFC_TAB_H_

#include <cstddef>
#include <string>

#include "devices/imfc_device.h"
#include "imgui.h"
#include "tabs/midi_tab.h"

namespace audio_dbg {

class ImfcTab : public MidiTab {
 public:
  static constexpr int kInstruments = 8;

  explicit ImfcTab(int device_id = 14,
                   std::string device_name = "IBM Music Feature");
  ~ImfcTab() override = default;

  void drawChannelStrips(const DeviceSnapshot& s) override;
  void drawDetail(const DeviceSnapshot& s) override;
  void drawTracker(const DeviceSnapshot& s, const SimState* sim) override;
  void onMuteClick(int ch) override;
  void pushWaveformData(int ch, const float* samples, std::size_t n) override;

  const char* programName(int program) const override;

  void setImfcDevice(ImfcDevice* dev);
  ImfcDevice* imfcDevice() const { return imfc_; }

  static std::string instrumentLabel(int inst);

  struct InstrumentStrip {
    int inst = -1;
    int channel = -1;
    bool active = false;
    bool muted = false;
    bool soloed = false;
    bool dormant = true;
    bool audible = false;
    std::size_t sounding = 0;
    float peak = 0.0f;
    int bank = 2;
    int program = 0;
    std::string voice_name;
  };
  InstrumentStrip instrumentStrip(const DeviceSnapshot& s, int inst) const;

  // Selected instrument in detail inspector (0..7)
  int selectedInstrument() const { return selected_inst_; }
  void setSelectedInstrument(int inst) {
    if (inst >= 0 && inst < kInstruments) selected_inst_ = inst;
  }

 private:
  ImfcDevice* imfc_ = nullptr;
  int selected_inst_ = 0;
};

}  // namespace audio_dbg

#endif  // PKMN_AUDIO_DBG_TABS_IMFC_TAB_H_
