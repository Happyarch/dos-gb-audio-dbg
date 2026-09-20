// Stage 4.3: Concrete MT-32 synthesizer backend.
//
// `Mt32Device` extends `MidiDevice` (Tier-2A) with headless Munt synthesis
// (`MT32Emu::Synth` from libmt32emu):
//   - ROM resolution: MT32_ROM_DIR env, cwd-relative roms/, well-known system
//     paths (86Box, scummvm config, mt32-rom-data, ...). MT32_ROM_DIR=none
//     forces mock mode (headless CI). When no valid control+PCM pair is
//     found the device enters mock/fallback mode: an
//     internal sine mixer driven by the MidiDevice note matrix, so dispatch,
//     rendering, and dormant-skipping all stay testable on any machine.
//   - factory part mapping is kept (parts 1-8 listen on 0-based channels
//     1-8, i.e. MIDI channels 2-9, rhythm on 0-based 9): channel 0
//     (MIDI channel 1) is part-less, exactly as on hardware and in
//     Munt-QT. A channel-assignment remap SysEx was tried and dropped —
//     the guessed system-area address left parts unmoved and clobbered
//     the rhythm assignment (measured via probe), so the device does not
//     second-guess the ROM. Channels 10-15 (0-based) stay unmapped:
//     the MT-32 has only 9 parts.
//   - native engine telemetry, zero heuristics: activePartialCount() counts
//     non-INACTIVE slots from Synth::getPartialStates(), partActive() wraps
//     Synth::getPartStates(), lcdText() wraps Synth::getDisplayState().
//     In mock mode the same queries are derived from the note matrix and a
//     20-char LCD buffer (Roland display SysEx updates it, like hardware).
//   - 16 MIDI channels track in the base; MT-32 parts 1-8 listen on
//     0-based channels 1-8 (MIDI 2-9) and rhythm on 0-based 9 by factory
//     default, the rest is forwarded harmlessly. Mute/solo render gating
//     rides CC7 part volume, which the MT-32 captures at note start
//     (measured: CC7=0 leaves a held piano at full level but drops a
//     subsequently triggered voice ~150x — authentic hardware behavior).
//     So the gate silences newly triggered voices while already-held
//     voices decay naturally, and the pre-synth filter stops muted
//     channels allocating anything new. Per-channel stem isolation uses
//     a CC7 mute-others dance (volumes saved/restored from the base CC
//     matrix), so each audible stem holds only its own channel's voices.
//   - render()/renderPerChannel() take the 1-cycle fast escape on dormant
//     channels (zeroed buffer, no synth stepping) and gate muted/
//     solo-excluded channels to silence. Un-initialized devices render zeros.
//
// NOTE: dispatchProgramChange()/dispatchControlChange()/dispatchSysEx() are
// the synth-reaching paths (they update base tracking AND forward). Calling
// the base setProgram()/setControlChange() directly updates tracking only,
// because those base methods are non-virtual.
//
// See docs/current_plan_debug_frontend.md §4.3.

#ifndef PKMN_AUDIO_DBG_DEVICES_MT32_DEVICE_H_
#define PKMN_AUDIO_DBG_DEVICES_MT32_DEVICE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "devices/midi_device.h"

namespace MT32Emu {
class Synth;
class FileStream;
class ROMImage;
}  // namespace MT32Emu

namespace audio_dbg {

class Mt32Device : public MidiDevice {
 public:
  static constexpr int kChannels = 16;
  static constexpr int kPartialSlots = 32;
  static constexpr int kParts = 9;  // Parts 1-8 + rhythm.
  static constexpr int kRhythmChannel = 9;
  static constexpr std::uint32_t kDefaultRate = 32000;
  static constexpr std::size_t kLcdChars = 20;

  explicit Mt32Device(int device_id = 12,
                      std::string device_name = "MT-32",
                      std::uint32_t sample_rate = kDefaultRate);
  ~Mt32Device() override;

  Mt32Device(const Mt32Device&) = delete;
  Mt32Device& operator=(const Mt32Device&) = delete;

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

  // --- Synth-reaching program/CC/SysEx (update base + forward) ---
  void dispatchProgramChange(int ch, int program);
  void dispatchControlChange(int ch, int cc, int value);
  void dispatchSysEx(const std::uint8_t* data, std::size_t len);

  // --- Patch names ---
  // Standard MT-32 timbre bank (128). Channel 9 is the rhythm channel
  // (per-key sounds); the melodic table is returned for every channel.
  std::string resolveProgramName(int channel, int program) const;
  static const char* mt32TimbreName(int program);

  // --- Native engine telemetry (no heuristics) ---
  // Real synth: non-INACTIVE partial slots from getPartialStates().
  // Mock: sounding notes capped at kPartialSlots.
  int activePartialCount() const;
  // 0=INACTIVE 1=ATTACK 2=SUSTAIN 3=RELEASE, -1 when slot out of range.
  int partialState(int slot) const;
  // Parts 0-7 + rhythm (8). Real: getPartStates bitset. Mock: any sounding
  // note on the mapped channel (part p <-> channel p+1, rhythm <-> ch 9).
  bool partActive(int part) const;
  std::string lcdText() const;

  // --- Mode queries ---
  bool isMock() const { return mock_; }
  bool romLoaded() const { return !mock_ && inited_ && synth_open_; }
  std::uint32_t sampleRate() const { return sample_rate_; }
  const std::string& romDir() const { return rom_dir_; }

 private:
  static double midiHz(int note);
  static bool chInRange(int ch) { return ch >= 0 && ch < kChannels; }
  bool synthAudible(int ch) const;
  void renderMockMono(float* out, std::size_t frames, int only_channel);
  void renderSynthMono(float* out, std::size_t frames);
  void muteOthersForStem(int solo_ch);
  void restoreVolumes();
  void parseDisplaySysEx(const std::uint8_t* data, std::size_t len);
  bool resolveRomPair();

  std::uint32_t sample_rate_;
  bool inited_ = false;
  bool mock_ = true;
  bool synth_open_ = false;
  std::string rom_dir_;
  MT32Emu::Synth* synth_ = nullptr;
  MT32Emu::FileStream* ctrl_file_ = nullptr;
  MT32Emu::FileStream* pcm_file_ = nullptr;
  const MT32Emu::ROMImage* ctrl_img_ = nullptr;
  const MT32Emu::ROMImage* pcm_img_ = nullptr;
  std::array<char, kLcdChars + 1> lcd_{};
  // Mock voice phases, driven by the base note matrix.
  std::array<std::array<double, 128>, kChannels> phases_{};
};

}  // namespace audio_dbg

#endif  // PKMN_AUDIO_DBG_DEVICES_MT32_DEVICE_H_
