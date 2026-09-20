// Stage 2.2: MidiDevice implementation.
// See midi_device.h for the contract.

#include "devices/midi_device.h"

namespace audio_dbg {

namespace {

constexpr int kDefaultVolumeCc = 100;
constexpr int kDefaultPanCc = 64;
constexpr int kPitchCentre = 8192;

}  // namespace

MidiDevice::MidiDevice(int device_id, std::string device_name, int channels)
    : SoundDevice(device_id, std::move(device_name),
                  channels <= 0 ? kMidiChannels : channels) {
  const std::size_t n = static_cast<std::size_t>(channelCount());
  velocities_.assign(n, {});
  sounding_.assign(n, {});
  note_counts_.assign(n, {});
  for (std::size_t i = 0; i < n; ++i) {
    velocities_[i].fill(0);
    sounding_[i].fill(false);
    note_counts_[i].fill(0);
  }
  programs_.assign(n, 0);
  pitch_bends_.assign(n, kPitchCentre);
  ccs_.assign(n, {});
  for (std::size_t i = 0; i < n; ++i) {
    ccs_[i].fill(0);
    ccs_[i][7] = kDefaultVolumeCc;
    ccs_[i][10] = kDefaultPanCc;
  }
}

bool MidiDevice::midiValid(int ch) const {
  return ch >= 0 && ch < channelCount();
}

bool MidiDevice::channelAudible(int ch) const {
  if (!midiValid(ch)) return false;
  DeviceSnapshot s = makeSnapshot();
  const ChannelState& st = s.channels[static_cast<std::size_t>(ch)];
  if (st.muted) return false;
  bool any_solo = false;
  for (const ChannelState& c : s.channels) {
    if (c.soloed) {
      any_solo = true;
      break;
    }
  }
  if (any_solo && !st.soloed) return false;
  return true;
}

bool MidiDevice::shouldFilterNoteOn(int ch) const {
  if (!midiValid(ch)) return true;
  return !channelAudible(ch);
}

bool MidiDevice::noteOn(int ch, int note, int velocity) {
  if (!midiValid(ch) || !noteValid(note)) return false;
  if (velocity < 1) velocity = 1;
  if (velocity > 127) velocity = 127;
  // Wake-up: any Note-On makes the channel live (even when filtered).
  wakeChannel(ch);
  const std::size_t c = static_cast<std::size_t>(ch);
  const std::size_t n = static_cast<std::size_t>(note);
  if (shouldFilterNoteOn(ch)) {
    // Pre-synth filter: record a non-sounding bar, allocate nothing.
    MidiNoteEvent ev;
    ev.channel = ch;
    ev.note = note;
    ev.velocity = velocity;
    ev.sounding = false;
    ev.start_frame = frame();
    ev.end_frame = frame();
    if (note_history_.size() >= 512) {
      note_history_.erase(note_history_.begin(), note_history_.begin() + 128);
    }
    note_history_.push_back(ev);
    return false;
  }
  ++note_counts_[c][n];
  velocities_[c][n] = velocity;
  sounding_[c][n] = true;
  MidiNoteEvent ev;
  ev.channel = ch;
  ev.note = note;
  ev.velocity = velocity;
  ev.sounding = true;
  ev.start_frame = frame();
  ev.end_frame = frame();
  if (note_history_.size() >= 512) {
    note_history_.erase(note_history_.begin(), note_history_.begin() + 128);
  }
  note_history_.push_back(ev);
  dispatchNoteOn(ch, note, velocity);
  return true;
}

void MidiDevice::noteOff(int ch, int note) {
  if (!midiValid(ch) || !noteValid(note)) return;
  wakeChannel(ch);
  const std::size_t c = static_cast<std::size_t>(ch);
  const std::size_t n = static_cast<std::size_t>(note);
  if (note_counts_[c][n] > 0) {
    --note_counts_[c][n];
  }
  // Only turn off the voice when no overlapping note-on remains active.
  if (note_counts_[c][n] == 0) {
    velocities_[c][n] = 0;
    sounding_[c][n] = false;
  }
  // Close the newest open bar for this (ch, note).
  for (std::size_t i = note_history_.size(); i-- > 0;) {
    MidiNoteEvent& ev = note_history_[i];
    if (ev.channel == ch && ev.note == note && ev.end_frame == ev.start_frame &&
        ev.sounding) {
      ev.end_frame = frame();
      ev.sounding = false;
      break;
    }
  }
  if (note_counts_[c][n] == 0) {
    dispatchNoteOff(ch, note);
  }
}

bool MidiDevice::isNoteSounding(int ch, int note) const {
  if (!midiValid(ch) || !noteValid(note)) return false;
  return sounding_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(note)];
}

int MidiDevice::noteVelocity(int ch, int note) const {
  if (!midiValid(ch) || !noteValid(note)) return 0;
  return velocities_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(note)];
}

std::size_t MidiDevice::activeNoteCount(int ch) const {
  if (!midiValid(ch)) return 0;
  std::size_t count = 0;
  const auto& row = sounding_[static_cast<std::size_t>(ch)];
  for (bool s : row) {
    if (s) ++count;
  }
  return count;
}

void MidiDevice::allNotesOff() {
  for (int ch = 0; ch < channelCount(); ++ch) {
    for (int note = 0; note < kNotesPerChannel; ++note) {
      if (sounding_[static_cast<std::size_t>(ch)]
                   [static_cast<std::size_t>(note)]) {
        note_counts_[static_cast<std::size_t>(ch)]
                    [static_cast<std::size_t>(note)] = 1;
        noteOff(ch, note);
      }
    }
  }
}

void MidiDevice::programChange(int ch, int program) {
  setProgram(ch, program);
}

void MidiDevice::setProgram(int ch, int program) {
  if (!midiValid(ch)) return;
  wakeChannel(ch);
  if (program < 0) program = 0;
  if (program > 127) program = 127;
  programs_[static_cast<std::size_t>(ch)] = program;
}

int MidiDevice::program(int ch) const {
  if (!midiValid(ch)) return 0;
  return programs_[static_cast<std::size_t>(ch)];
}

void MidiDevice::setPitchBend(int ch, int value) {
  if (!midiValid(ch)) return;
  wakeChannel(ch);
  if (value < 0) value = 0;
  if (value > 16383) value = 16383;
  pitch_bends_[static_cast<std::size_t>(ch)] = value;
}

int MidiDevice::pitchBend(int ch) const {
  if (!midiValid(ch)) return kPitchCentre;
  return pitch_bends_[static_cast<std::size_t>(ch)];
}

void MidiDevice::sendControlChange(int ch, int cc, int value) {
  setControlChange(ch, cc, value);
}

void MidiDevice::setControlChange(int ch, int cc, int value) {
  if (!midiValid(ch) || cc < 0 || cc > 127) return;
  wakeChannel(ch);
  if (value < 0) value = 0;
  if (value > 127) value = 127;
  ccs_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(cc)] = value;
}

int MidiDevice::controlChange(int ch, int cc) const {
  if (!midiValid(ch) || cc < 0 || cc > 127) return 0;
  return ccs_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(cc)];
}

void MidiDevice::setMute(int ch, bool muted) { setMutedState(ch, muted); }

void MidiDevice::setSolo(int ch, bool soloed) { setSoloedState(ch, soloed); }

void MidiDevice::tick(std::uint32_t frame) { setFrame(frame); }

void MidiDevice::handleCommand(std::uint8_t /*opcode*/,
                               const std::uint8_t* payload, std::size_t len) {
  // Fast escape: empty commands wake nothing.
  if (payload == nullptr || len == 0) return;
  if (channelCount() == 0) return;
  const int ch =
      static_cast<int>(payload[0]) % channelCount();
  wakeChannel(ch);
}

DeviceSnapshot MidiDevice::snapshot() const { return makeSnapshot(); }

}  // namespace audio_dbg
