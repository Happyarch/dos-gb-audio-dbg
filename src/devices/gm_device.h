// Stage 4.4: Concrete General MIDI synthesizer backend.
//
// `GmDevice` extends `MidiDevice` (Tier-2A) with FluidSynth synthesis:
//   - SoundFont loader: SOUNDFONT env, well-known system paths
//     (/usr/share/soundfonts/default.sf2, FluidR3_GM.sf2, ...).
//     SOUNDFONT=none forces mock mode (headless CI). When no SoundFont is
//     found the device enters mock/fallback mode: an internal
//     sine mixer driven by the MidiDevice note matrix, so dispatch,
//     rendering, and dormant-skipping stay testable on any machine.
//   - 16-channel MIDI render at 44100 Hz via fluid_synth_write_float().
//     Per-channel stem isolation uses a CC7 mute-others dance; every
//     render converges engine volumes to the live mute matrix
//     (audible?tracked-CC7:0), so muted channels stay silent across calls.
//   - standard 128-name General MIDI patch resolver.
//   - render()/renderPerChannel() take the 1-cycle fast escape on dormant
//     channels (zeroed buffer, no synth stepping) and gate muted/
//     solo-excluded channels to silence. Un-initialized devices render zeros.
//
// NOTE: dispatchProgramChange()/dispatchControlChange() are the
// synth-reaching paths (they update base tracking AND forward). Calling the
// base setProgram()/setControlChange() directly updates tracking only,
// because those base methods are non-virtual.
//
// See docs/current_plan_debug_frontend.md §4.4.

#ifndef PKMN_AUDIO_DBG_DEVICES_GM_DEVICE_H_
#define PKMN_AUDIO_DBG_DEVICES_GM_DEVICE_H_

#include <array>
#include <cstddef>
#include <string>

#include <fluidsynth.h>

#include "devices/midi_device.h"

namespace audio_dbg {

class GmDevice : public MidiDevice {
 public:
  static constexpr int kChannels = 16;
  static constexpr int kDrumChannel = 9;
  static constexpr std::uint32_t kDefaultRate = 44100;

  explicit GmDevice(int device_id = 13,
                    std::string device_name = "General MIDI",
                    std::uint32_t sample_rate = kDefaultRate);
  ~GmDevice() override;

  GmDevice(const GmDevice&) = delete;
  GmDevice& operator=(const GmDevice&) = delete;

  // --- SoundDevice lifecycle/rendering ---
  bool init() override;
  void shutdown() override;
  void reset() override;
  void render(float* buf, std::size_t frames) override;
  void renderPerChannel(float** bufs, std::size_t frames) override;

  // --- MidiDevice voice hooks (synth writes live here) ---
  // Called only for non-filtered notes, so muted channels allocate nothing.
  void dispatchNoteOn(int ch, int note, int velocity) override;
  void dispatchNoteOff(int ch, int note) override;

  // --- Synth-reaching program/CC (update base + forward) ---
  void programChange(int ch, int program) override;
  void sendControlChange(int ch, int cc, int value) override;
  void dispatchProgramChange(int ch, int program);
  void dispatchControlChange(int ch, int cc, int value);

  // --- Patch names ---
  // Standard General MIDI 128 program names. Channel 9 is the drum channel
  // (per-key sounds); the melodic table is returned for every channel.
  std::string resolveProgramName(int channel, int program) const;
  static const char* gmProgramName(int program);

  // --- Mode queries ---
  bool isMock() const { return mock_; }
  bool soundFontLoaded() const { return !mock_ && inited_ && sf_id_ >= 0; }
  std::uint32_t sampleRate() const { return sample_rate_; }
  const std::string& soundFontPath() const { return sf_path_; }

 private:
  static double midiHz(int note);
  static bool chInRange(int ch) { return ch >= 0 && ch < kChannels; }
  bool synthAudible(int ch) const;
  void renderMockMono(float* out, std::size_t frames, int only_channel);
  void renderSynthMono(float* out, std::size_t frames);
  void muteOthersForStem(int solo_ch);
  void restoreVolumes();
  bool loadSoundFont();

  std::uint32_t sample_rate_;
  bool inited_ = false;
  bool mock_ = true;
  std::string sf_path_;
  fluid_settings_t* settings_ = nullptr;
  fluid_synth_t* synth_ = nullptr;
  int sf_id_ = -1;
  // Mock voice phases, driven by the base note matrix.
  std::array<std::array<double, 128>, kChannels> phases_{};
};

}  // namespace audio_dbg

#endif  // PKMN_AUDIO_DBG_DEVICES_GM_DEVICE_H_
