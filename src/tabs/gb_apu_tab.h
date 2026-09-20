// Stage 7.4: GB-APU concrete tab.
//
// `GbApuTab` extends `PsgTab` for the Game Boy APU (Blargg Gb_Snd_Emu):
//   - Pulse 1: duty visualizer, period/frequency, envelope volume, sweep.
//   - Pulse 2: duty visualizer, period/frequency, envelope volume.
//   - Wave: 32-nibble Wave RAM visualizer + hex readouts, playback
//     frequency, volume level.
//   - Noise: 7-bit vs 15-bit LFSR mode, polynomial step, clock shift,
//     divisor.
//   - Per-channel oscilloscopes with zero-crossing stabilization.
//
// Draw methods early-return without an ImGui context; the ImGui-free
// helpers (`channelLabel()`, `envelopeName()`, `noiseWidthName()`,
// `waveVolumeName()`, `channelCard()`, `findZeroCrossing()`) are
// unit-testable.
//
// See docs/current_plan_debug_frontend.md §7.4.

#ifndef PKMN_AUDIO_DBG_TABS_GB_APU_TAB_H_
#define PKMN_AUDIO_DBG_TABS_GB_APU_TAB_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "devices/gb_apu_device.h"
#include "imgui.h"
#include "tabs/psg_tab.h"

namespace audio_dbg {

class GbApuTab : public PsgTab {
 public:
  GbApuTab(int device_id = 11, std::string device_name = "GB-APU")
      : PsgTab(device_id, std::move(device_name)) {}
  ~GbApuTab() override = default;

  void drawChannelStrips(const DeviceSnapshot& s) override;
  void drawDetail(const DeviceSnapshot& s) override;
  void drawTracker(const DeviceSnapshot& s, const SimState* sim) override;
  void onMuteClick(int ch) override;
  // Dormant-channel fast escape: pushes to dormant channels store nothing.
  void pushWaveformData(int ch, const float* samples, std::size_t n) override;

  // "Env <pace> (<up|down>)": envelope pace 0..7 with direction bit
  // (bit 3 of NRx2). Out-of-range envelopes clamp into 0..7.
  const char* envelopeName(int envelope) const override;

  void setGbApuDevice(GbApuDevice* dev);
  GbApuDevice* gbApuDevice() const { return gb_; }

  static constexpr int kChannels = 4;
  static constexpr int kPulse1 = 0;
  static constexpr int kPulse2 = 1;
  static constexpr int kWave = 2;
  static constexpr int kNoise = 3;

  // "Pulse 1" / "Pulse 2" / "Wave" / "Noise", else "Invalid".
  static std::string channelLabel(int ch);
  // "7-bit" when seven_bit is true, else "15-bit".
  static const char* noiseWidthName(bool seven_bit);
  // NR32 volume code 0..3 -> "Mute" / "100%" / "50%" / "25%".
  static const char* waveVolumeName(int code);

  // ImGui-free per-channel summary for cards and unit checks.
  struct ChannelCard {
    int channel = -1;
    bool dormant = true;
    bool muted = false;
    bool active = false;
    double frequency = 0.0;
    int duty = 0;
    int volume = 0;
    int envelope = 0;
    std::uint8_t sweep = 0;
    std::array<std::uint8_t, 32> wave_ram{};
    int wave_volume_code = 0;
    bool noise_seven_bit = false;
    std::uint8_t noise_shift = 0;
    std::uint8_t noise_divisor = 0;
  };
  ChannelCard channelCard(int ch) const;

  // Zero-crossing stabilization for channel scopes (same contract as
  // Opl3Tab::findZeroCrossing): first positive-slope crossing, else 0.
  static std::size_t findZeroCrossing(const float* samples, std::size_t n);

  // Headless probe: buffered scope samples for a channel.
  std::size_t testWaveSize(int ch) const { return waveSize(ch); }

 private:
  GbApuDevice* gb_ = nullptr;
};

}  // namespace audio_dbg

#endif  // PKMN_AUDIO_DBG_TABS_GB_APU_TAB_H_
