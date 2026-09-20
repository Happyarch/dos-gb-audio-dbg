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
//     default, the rest is forwarded harmlessly. Mute/solo is enforced by
//     the pre-synth Note-On filter plus Note-Off/CC120/CC123 for held
//     notes — NEVER by CC7: the MT-32 captures part volume at note start,
//     so a CC7=0 write aimed at a channel that is dormant at click time
//     silences that channel forever (nothing re-arms CC7). Per-channel
//     stem isolation silences the other channels' held notes through the
//     same Note-Off/CC120/CC123 path.
//   - MUNT is driven exclusively through the thread-safe playMsg()/
//     playSysex() event queue (return value checked, default depth 1024);
//     the SDL audio callback is the only thread that calls Synth::render().
//     Engine telemetry (partials, part states, patch names, playing notes,
//     LCD) is read by the render thread into a shadow snapshot that the UI
//     reads under a mutex, so the UI never touches the live synth.
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
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
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
  void setMute(int ch, bool muted) override;
  void setSolo(int ch, bool soloed) override;

  // --- MidiDevice voice hooks (synth writes live here) ---
  // Called only for non-filtered notes, so muted channels allocate nothing.
  void dispatchNoteOn(int ch, int note, int velocity) override;
  void dispatchNoteOff(int ch, int note) override;

  // --- Synth-reaching program/CC/SysEx (update base + forward) ---
  void programChange(int ch, int program) override;
  void sendControlChange(int ch, int cc, int value) override;
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

  // --- Part / voice queries (MUNT-QT parity) ---
  // Both read the telemetry shadow snapshot produced by the render thread,
  // never the live synth, so the UI thread stays race-free.
  std::string patchName(int part) const;
  int getPlayingNotes(int part, std::uint8_t* keys, std::uint8_t* velocities) const;

  // --- Engine queue health (thread-safe) ---
  // Number of short/SysEx messages the MUNT event queue rejected (full).
  std::uint32_t droppedMessages() const { return dropped_msgs_.load(); }

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

  // --- Thread-safe MUNT command queue ---
  // The UI/engine thread enqueues via Synth::playMsg()/playSysex() (safe to
  // call from any thread, no synchronisation needed with the renderer); the
  // SDL audio callback is the only thread that calls Synth::render().
  // The immediate-send variants (the "*Now" methods) are NEVER used: their
  // header requires the caller to be synchronised with the render thread.
  bool queueMsg(std::uint32_t msg);
  bool queueSysex(const std::uint8_t* data, std::size_t len);

  // Mute = pre-synth Note-On filter (MidiDevice) + note-offs for held notes.
  // NEVER CC7=0 (the MT-32 captures part volume at note start).
  void silenceChannel(int ch);
  void silenceOthersForStem(int solo_ch);

  // MUNT telemetry shadow. Refreshed only by the render thread; the UI reads
  // it under telemetry_mutex_.
  void updateTelemetry();
  void parseDisplaySysEx(const std::uint8_t* data, std::size_t len);
  bool resolveRomPair();

  static constexpr std::uint32_t kSysexQueueStorage = 4096;

  struct Telemetry {
    int partial_count = 0;
    std::array<int, kPartialSlots> partial_state{};
    std::array<int, kParts> part_active{};
    std::array<std::array<char, 24>, kParts> patch_name{};
    std::array<int, kParts> playing_count{};
    std::array<std::array<std::uint8_t, 32>, kParts> playing_keys{};
    std::array<std::array<std::uint8_t, 32>, kParts> playing_vel{};
    std::array<char, kLcdChars + 1> lcd{};
  };

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
  // Messages rejected by the full MUNT event queue (see droppedMessages()).
  std::atomic<std::uint32_t> dropped_msgs_{0};
  // Guards telemetry_ between the render thread and the UI thread.
  mutable std::mutex telemetry_mutex_;
  Telemetry telemetry_;
};

}  // namespace audio_dbg

#endif  // PKMN_AUDIO_DBG_DEVICES_MT32_DEVICE_H_
