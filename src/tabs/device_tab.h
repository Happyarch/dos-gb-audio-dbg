// Stage 2.1 + 2.5: Tier-1 universal device-tab base.
//
// `DeviceTab` is the polymorphic root for every per-device UI panel
// (OPL3, GB-APU, MT-32, GM, future devices). Concrete tabs implement the
// five pure virtuals; shared helpers below stay ImGui-free so this header
// compiles anywhere (including headless unit checks).
//
// Dormant rule (§2.5): draw*() implementations open each channel with
// `if (s.channels[i].dormant) continue;` so unused channels cost no UI
// work. pushWaveformData() applies the same escape before buffering.
//
// Scope feed (W6): the LIVE oscilloscope samples are captured on the AUDIO
// thread inside each backend's SoundDevice::render() and stored in the
// device-owned SPSC ring; a concrete tab's draw reads them with
// `<device>->copyWaveform(ch, dst, n)`. The `wave_rings_` storage below
// backs the retained `pushWaveformData()` probe API (tab-local, unit
// tests); production draw code must NOT read it — the UI thread never
// renders a scope or advances emulation to fill one.
//
// See docs/current_plan_debug_frontend.md §2.1 / §2.5.

#ifndef PKMN_AUDIO_DBG_TABS_DEVICE_TAB_H_
#define PKMN_AUDIO_DBG_TABS_DEVICE_TAB_H_

#include <cstddef>
#include <string>
#include <vector>

#include "devices/sound_device.h"

namespace audio_dbg {

class DeviceTab {
 public:
  DeviceTab(int device_id, std::string device_name)
      : device_id_(device_id), device_name_(std::move(device_name)) {}

  virtual ~DeviceTab() = default;

  DeviceTab(const DeviceTab&) = delete;
  DeviceTab& operator=(const DeviceTab&) = delete;
  DeviceTab(DeviceTab&&) = default;
  DeviceTab& operator=(DeviceTab&&) = default;

  // --- Pure virtuals: every device tab implements these ---
  virtual void drawChannelStrips(const DeviceSnapshot& s) = 0;
  virtual void drawDetail(const DeviceSnapshot& s) = 0;
  virtual void drawTracker(const DeviceSnapshot& s, const SimState* sim) = 0;
  virtual void onMuteClick(int ch) = 0;
  // Tab-local scope probe (dormant escape included). Retained for unit
  // tests and out-of-band callers; the live draw path reads the device-owned
  // ring via SoundDevice::copyWaveform() instead.
  virtual void pushWaveformData(int ch, const float* samples,
                                std::size_t n) = 0;

  int deviceId() const { return device_id_; }
  const std::string& deviceName() const { return device_name_; }

  // --- Common UI utilities (concrete, ImGui-free) ---

  // True when at least one channel has its solo toggle set.
  static bool anySoloed(const DeviceSnapshot& s) {
    for (const ChannelState& ch : s.channels) {
      if (ch.soloed) return true;
    }
    return false;
  }

  // Audibility under the mute/solo matrix (dormant channels are silent).
  // Out-of-range channels are inaudible.
  static bool channelAudible(const DeviceSnapshot& s, int ch) {
    if (ch < 0 || static_cast<std::size_t>(ch) >= s.channels.size()) return false;
    const ChannelState& st = s.channels[static_cast<std::size_t>(ch)];
    if (st.dormant || st.muted) return false;
    if (anySoloed(s) && !st.soloed) return false;
    return true;
  }

  // Munt-QT velocity grading: RGB(2*vel, 255-2*vel, 0), outputs 0..1.
  static void velocityColor(int velocity, float* out_r, float* out_g,
                            float* out_b) {
    if (velocity < 0) velocity = 0;
    if (velocity > 127) velocity = 127;
    const float r = static_cast<float>(2 * velocity) / 255.0f;
    const float g = static_cast<float>(255 - 2 * velocity) / 255.0f;
    if (out_r != nullptr) *out_r = r;
    if (out_g != nullptr) *out_g = g;
    if (out_b != nullptr) *out_b = 0.0f;
  }

  static const char* muteLabel(bool muted) { return muted ? "M*" : "M"; }
  static const char* soloLabel(bool soloed) { return soloed ? "S*" : "S"; }
  static const char* dormantLabel(bool dormant) {
    return dormant ? "dormant" : "live";
  }

  static float clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
  }

 protected:
  // Tab-local scope storage backing the pushWaveformData() probe API only.
  // Concrete overrides call ensureWaveStorage() once (when the channel count
  // is known) and then storeWaveform(); both apply the §2.5 dormant escape
  // via the caller. The live draw path uses SoundDevice::copyWaveform()
  // (device-owned, audio-thread-fed) and does not touch these rings.
  void ensureWaveStorage(std::size_t channels, std::size_t capacity = 4096) {
    if (wave_rings_.size() != channels) {
      wave_rings_.clear();
      wave_rings_.reserve(channels);
      for (std::size_t i = 0; i < channels; ++i) {
        wave_rings_.emplace_back(capacity);
      }
    }
  }

  void storeWaveform(int ch, const float* samples, std::size_t n) {
    if (ch < 0 || static_cast<std::size_t>(ch) >= wave_rings_.size()) return;
    if (samples == nullptr || n == 0) return;
    wave_rings_[static_cast<std::size_t>(ch)].push(samples, n);
  }

  std::size_t waveSize(int ch) const {
    if (ch < 0 || static_cast<std::size_t>(ch) >= wave_rings_.size()) return 0;
    return wave_rings_[static_cast<std::size_t>(ch)].size();
  }

  std::size_t copyWaveform(int ch, float* dst, std::size_t n) const {
    if (ch < 0 || static_cast<std::size_t>(ch) >= wave_rings_.size()) return 0;
    return wave_rings_[static_cast<std::size_t>(ch)].copyRecent(dst, n);
  }

 private:
  int device_id_ = 0;
  std::string device_name_;
  std::vector<WaveformRing> wave_rings_;
};

}  // namespace audio_dbg

#endif  // PKMN_AUDIO_DBG_TABS_DEVICE_TAB_H_
