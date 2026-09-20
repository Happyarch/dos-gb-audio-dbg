// Stage 2.2: Tier-2A MIDI intermediate device base.
//
// `MidiDevice` extends `SoundDevice` for all MIDI-type synthesizers (MT-32,
// GM, future FB-01). It owns:
//   - 16-channel MIDI state: program, pitch bend, CCs (volume/pan),
//   - active note matrix tracking velocity + sounding state per (ch, note),
//   - persistent piano-roll note history (`MidiNoteEvent`),
//   - pre-synth stream-filter muting: Note-On on a muted/solo-excluded
//     channel is suppressed before any synth voice allocation.
//
// Channel wake-up (§2.5): `noteOn()` and `handleCommand()` with a non-empty
// payload wake the addressed channel. Muted Note-On still wakes (strip
// becomes visible) but is not marked sounding and never reaches
// `dispatchNoteOn()`.
//
// See docs/current_plan_debug_frontend.md §2.2 / §2.5.

#ifndef PKMN_AUDIO_DBG_DEVICES_MIDI_DEVICE_H_
#define PKMN_AUDIO_DBG_DEVICES_MIDI_DEVICE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "devices/sound_device.h"

namespace audio_dbg {

// Persistent piano-roll bar: one entry per accepted-or-suppressed Note-On,
// closed by the matching Note-Off.
struct MidiNoteEvent {
  int channel = 0;
  int note = 60;
  int velocity = 0;          // 1..127 (0 = suppressed placeholder).
  bool sounding = false;     // True while the note holds a synth voice.
  std::uint32_t start_frame = 0;
  std::uint32_t end_frame = 0;  // == start_frame while still held.
};

class MidiDevice : public SoundDevice {
 public:
  static constexpr int kMidiChannels = 16;
  static constexpr int kNotesPerChannel = 128;

  MidiDevice(int device_id, std::string device_name,
             int channels = kMidiChannels);
  ~MidiDevice() override = default;

  // --- Note API (concrete) ---
  // Returns true when the Note-On is accepted (audible + dispatched).
  // Returns false when suppressed by the pre-synth mute/solo filter: the
  // channel still wakes, history records a non-sounding bar, but no voice
  // is allocated and dispatchNoteOn() is NOT called.
  bool noteOn(int ch, int note, int velocity);
  void noteOff(int ch, int note);

  bool isNoteSounding(int ch, int note) const;
  int noteVelocity(int ch, int note) const;  // 0 when off.
  std::size_t activeNoteCount(int ch) const;
  // Silences every sounding note (noteOff per held key, so the synth
  // receives matching note-offs and history bars close). Used by the
  // session engine for position-locked A/B switches (Stage 5.4).
  void allNotesOff();

  // --- Program / pitch / CC (concrete, wake on write) ---
  void setProgram(int ch, int program);
  int program(int ch) const;
  void setPitchBend(int ch, int value);  // 0..16383, 8192 = centre.
  int pitchBend(int ch) const;
  void setControlChange(int ch, int cc, int value);  // cc 0..127.
  int controlChange(int ch, int cc) const;

  // Pre-synth filter: true = suppress Note-On (muted or solo-excluded).
  // Out-of-range channels are always suppressed.
  bool shouldFilterNoteOn(int ch) const;

  const std::vector<MidiNoteEvent>& noteHistory() const {
    return note_history_;
  }

  // --- SoundDevice: concrete mute/solo/tick/command/snapshot ---
  void setMute(int ch, bool muted) override;
  void setSolo(int ch, bool soloed) override;
  void tick(std::uint32_t frame) override;
  // Non-empty payload wakes payload[0] % channelCount(); empty payload
  // takes the fast escape and wakes nothing.
  void handleCommand(std::uint8_t opcode, const std::uint8_t* payload,
                     std::size_t len) override;
  DeviceSnapshot snapshot() const override;

  // --- Backend hooks (pure virtual: voice allocation lives here) ---
  // Called ONLY for non-filtered Note-On/Note-Off, so the synth never
  // allocates a voice for a muted channel.
  virtual void dispatchNoteOn(int ch, int note, int velocity) = 0;
  virtual void dispatchNoteOff(int ch, int note) = 0;

 protected:
  bool midiValid(int ch) const;
  static bool noteValid(int note) { return note >= 0 && note < 128; }
  // Audibility under the live mute/solo matrix.
  bool channelAudible(int ch) const;

 private:
  std::vector<std::array<int, kNotesPerChannel> > velocities_;
  std::vector<std::array<bool, kNotesPerChannel> > sounding_;
  std::vector<int> programs_;
  std::vector<int> pitch_bends_;
  std::vector<std::array<int, 128> > ccs_;
  std::vector<MidiNoteEvent> note_history_;
};

}  // namespace audio_dbg

#endif  // PKMN_AUDIO_DBG_DEVICES_MIDI_DEVICE_H_
