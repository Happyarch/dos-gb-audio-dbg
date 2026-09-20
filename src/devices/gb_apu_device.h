// Stage 4.2: Concrete Game Boy APU synthesizer backend.
//
// `GbApuDevice` extends `PsgDevice` (Tier-2C) with Shay Green's Gb_Snd_Emu
// (Gb_Apu + Stereo_Buffer — the same engine classes `Basic_Gb_Apu` wraps):
//   - master APU instance (full mix) + 4 shadow APUs for isolated
//     per-channel rendering (0: Pulse1, 1: Pulse2, 2: Wave, 3: Noise),
//   - Game Boy audio register decode (0xFF10-0xFF25 + wave RAM 0xFF30-0xFF3F):
//     pulse duty (12.5/25/50/75%), frequency, volume, sweep; wave RAM
//     (32 nibbles); noise LFSR 7/15-bit mode + polynomial divisor/shift,
//   - lazy channel wake-up: a channel wakes on its first write/trigger;
//     renderPerChannel() takes the fast escape on dormant channels.
//
// Writes are fanned out to the master APU always, and to the addressed
// shadow APU when the register belongs to one channel. Global registers
// (NR50/NR51/NR52) mirror to all initialized shadows.
//
// NOTE on Basic_Gb_Apu: the device does NOT hold `Basic_Gb_Apu` directly.
// Gb_Snd_Emu 0.1.4's `Blip_Buffer::set_sample_rate(rate)` default path
// computes `(ULONG_MAX >> 16) + 1 - ...` as `unsigned`, which is 65536 on
// 32-bit but truncates to 0 and underflows to ~4.3 G samples (~16 GB
// memset) on 64-bit — measured 33 s per test binary for 5 instances.
// The device therefore replicates `Basic_Gb_Apu` exactly (same Gb_Apu +
// Stereo_Buffer classes, same treble/bass EQ, same fake-CPU clocking and
// frame length, hence bit-identical output) with an explicit 500 ms buffer
// length. See gb_apu_device.cpp `GbVoiceApu`.
//
// See docs/current_plan_debug_frontend.md §4.2.

#ifndef PKMN_AUDIO_DBG_DEVICES_GB_APU_DEVICE_H_
#define PKMN_AUDIO_DBG_DEVICES_GB_APU_DEVICE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "devices/psg_device.h"

// Forward declaration: the Gb_Apu + Stereo_Buffer voice wrapper lives in
// gb_apu_device.cpp (see the Basic_Gb_Apu note above).
namespace audio_dbg {
struct GbVoiceApu;
}  // namespace audio_dbg

namespace audio_dbg {

class GbApuDevice : public PsgDevice {
 public:
  static constexpr int kChannels = 4;
  // Native rate == the mixer's 48 kHz output rate, so the device takes the
  // dev_rate == out_rate 1:1 passthrough in AudioMixer::renderBlock and the
  // mixer needs no resampler for it. Pitch is unaffected: GbVoiceApu pins
  // Stereo_Buffer's clock_rate to 4194304 independently of the sample rate
  // (gb_apu_device.cpp set_sample_rate), so Blip_Buffer just synthesizes its
  // band-limited output at 48000 instead of 49716.
  static constexpr long kDefaultRate = 48000;

  // GB audio register addresses.
  static constexpr std::uint16_t kRegBase = 0xFF10;
  static constexpr std::uint16_t kRegEnd = 0xFF3F;
  static constexpr std::uint16_t kWaveBase = 0xFF30;

  explicit GbApuDevice(int device_id = 11,
                       std::string device_name = "GB-APU",
                       long sample_rate = kDefaultRate);
  ~GbApuDevice() override;

  GbApuDevice(const GbApuDevice&) = delete;
  GbApuDevice& operator=(const GbApuDevice&) = delete;

  // --- SoundDevice lifecycle/rendering ---
  bool init() override;
  void shutdown() override;
  void reset() override;
  void render(float* buf, std::size_t frames) override;
  void renderPerChannel(float** bufs, std::size_t frames) override;

  // --- Raw GB register write (0xFF10-0xFF3F) ---
  // Updates the register shadow, forwards to master + addressed shadow
  // APU(s), and decodes duty/frequency/volume/sweep/wave/LFSR state into
  // the PsgDevice base (waking the channel). Out-of-range addresses are
  // ignored.
  void writeRegister(std::uint16_t addr, std::uint8_t val);
  std::uint8_t readRegister(std::uint16_t addr) const;

  // --- PsgDevice hardware hook (register writes live here) ---
  void applyRegister(int ch) override;
  void handleCommand(std::uint8_t opcode, const std::uint8_t* payload,
                     std::size_t len) override;

  // --- Mute/solo/snapshot: keep the lock-free gate cache current ---
  // Every UI read (snapshot) and every gate mutation (setMute/setSolo)
  // refreshes `gate_muted_`/`gate_soloed_`/`any_solo_`, so the audio thread
  // never has to snapshot the ChannelState vector.
  void setMute(int ch, bool muted) override;
  void setSolo(int ch, bool soloed) override;
  DeviceSnapshot snapshot() const override;

  long sampleRate() const { return sample_rate_; }
  bool isShadowInitialized(int ch) const;

  // --- Decoded GB state accessors (for tabs/deep diagnostics) ---
  std::uint8_t sweep(int ch) const;       // NR10 raw (ch0 only, else 0).
  std::uint8_t noiseDivisor() const;      // NR43 bits 2-0.
  std::uint8_t noiseShift() const;        // NR43 bits 7-4.
  bool noiseSevenBit() const;             // NR43 bit 3.
  std::uint8_t waveVolumeCode() const;    // NR32 bits 6-5 (0-3).
  std::uint8_t masterVolume() const;      // NR50 raw.
  std::uint8_t panning() const;           // NR51 raw.
  std::uint8_t power() const;             // NR52 raw.

 private:
  static int channelForAddr(std::uint16_t addr);
  static bool isGlobalAddr(std::uint16_t addr);
  void ensureShadow(int ch);
  void pushToApu(GbVoiceApu* apu, std::uint16_t addr, std::uint8_t val);
  void decodeAndTrack(std::uint16_t addr, std::uint8_t val);
  void decodePulse(int ch, std::uint16_t addr);
  void decodeWave(std::uint16_t addr);
  void decodeNoise(std::uint16_t addr);
  static double pulseFreqHz(int freq_val);
  static double waveFreqHz(int freq_val);
  static double noiseFreqHz(std::uint8_t nr43);
  // Per-channel mute/solo gate. Reads the cached per-channel gate flags
  // (`gate_muted_` / `gate_soloed_`, refreshed by snapshot()/setMute()/
  // setSolo()) instead of building a DeviceSnapshot -- the old
  // makeSnapshot() call heap-copied every channel name once per channel per
  // audio callback.
  bool channelAudible(int ch) const;
  // True when any live channel has its mute toggle set (fast gate-cache read).
  bool hasMutedChannel() const;
  // Rebuilds the lock-free per-channel gate cache + `any_solo_` from the live
  // ChannelState. UI thread (snapshot()/setMute()/setSolo()); the audio
  // callback only reads. No re-snapshot from the render path.
  void refreshAudibleCache() const;
  // Drains one APU frame into mono floats; returns frames written.
  std::size_t drainApu(GbVoiceApu* apu, float* out, std::size_t max_frames);

  long sample_rate_;
  bool inited_ = false;
  // Register shadow for 0xFF10-0xFF3F (0x30 bytes).
  std::array<std::uint8_t, 0x30> regs_{};
  GbVoiceApu* master_ = nullptr;
  GbVoiceApu* shadows_[kChannels] = {nullptr, nullptr, nullptr, nullptr};
  std::array<bool, kChannels> shadow_init_{};

  // Realtime render scratch, preallocated at init() so the audio callback
  // never allocates: `tmp_scratch_` is the muted/soloed mix staging buffer
  // (render()) and `discard_scratch_` the drain-only sink used to keep
  // gates/audible-but-replacing shadows clocked.
  std::vector<float> tmp_scratch_;
  std::vector<float> discard_scratch_;

  // Lock-free per-channel gate cache (see channelAudible/refreshAudibleCache).
  // `mutable` so the const UI-thread snapshot() can refresh it.
  mutable std::array<bool, kChannels> gate_muted_{};
  mutable std::array<bool, kChannels> gate_soloed_{};
  mutable bool any_solo_ = false;

  // Realtime scratch floor (frames): the SDL callback block (512) and the
  // headless per-frame render both fit well under this.
  static constexpr std::size_t kScratchFrames = 8192;
};

}  // namespace audio_dbg

#endif  // PKMN_AUDIO_DBG_DEVICES_GB_APU_DEVICE_H_
