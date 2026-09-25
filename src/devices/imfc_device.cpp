// Stage 4.6: ImfcDevice implementation.
// See imfc_device.h for the contract.

#include "devices/imfc_device.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace audio_dbg {

ImfcDevice::ImfcDevice(int device_id, std::string device_name,
                       std::uint32_t sample_rate)
    : MidiDevice(device_id, std::move(device_name), kChannels),
      sample_rate_(sample_rate == 0 ? kDefaultRate : sample_rate),
      synth_(std::make_unique<ImfcSynthCore>()) {
  for (int i = 0; i < kChannels; ++i) {
    channel(i).name = "IMFC INST " + std::to_string(i + 1);
  }
}

ImfcDevice::~ImfcDevice() {
  shutdown();
}

bool ImfcDevice::init() {
  if (inited_) return true;
  if (!synth_) synth_ = std::make_unique<ImfcSynthCore>();
  if (!synth_->init(sample_rate_)) return false;
  inited_ = true;
  reset();
  return true;
}

void ImfcDevice::shutdown() {
  if (!inited_) return;
  reset();
  inited_ = false;
}

void ImfcDevice::reset() {
  MidiDevice::allNotesOff();
  if (synth_) synth_->reset();
  for (int i = 0; i < kChannels; ++i) {
    channel(i).dormant = true;
    channel(i).active = false;
    channel(i).peak = 0.0f;
  }
}

void ImfcDevice::render(float* buf, std::size_t frames) {
  if (buf == nullptr || frames == 0) return;
  std::memset(buf, 0, frames * sizeof(float));
  if (!inited_ || !synth_) return;

  std::vector<float> left(frames);
  std::vector<float> right(frames);
  synth_->renderSamples(left.data(), right.data(), frames);

  float peak = 0.0f;
  for (std::size_t i = 0; i < frames; ++i) {
    float m = (left[i] + right[i]) * 0.5f;
    buf[i] = m;
    float abs_m = std::fabs(m);
    if (abs_m > peak) peak = abs_m;
  }
  noteMasterPeak(peak);

  // Push waveform samples to active channel scopes
  std::vector<float> ch_l(frames);
  std::vector<float> ch_r(frames);
  for (int ch = 0; ch < kChannels; ++ch) {
    if (isDormant(ch)) continue;
    synth_->renderChannelSamples(ch, ch_l.data(), ch_r.data(), frames);
    pushWaveform(ch, ch_l.data(), frames);
    float ch_peak = 0.0f;
    for (std::size_t i = 0; i < frames; ++i) {
      float a = std::fabs(ch_l[i]);
      if (a > ch_peak) ch_peak = a;
    }
    channel(ch).peak = ch_peak;
    channel(ch).active = synth_->isInstrumentSounding(ch);
  }
}

void ImfcDevice::renderPerChannel(float** bufs, std::size_t frames) {
  if (bufs == nullptr || frames == 0 || !inited_ || !synth_) return;
  std::vector<float> dummy_r(frames);
  for (int ch = 0; ch < kChannels; ++ch) {
    if (bufs[ch] == nullptr) continue;
    if (isDormant(ch)) {
      std::memset(bufs[ch], 0, frames * sizeof(float));
      continue;
    }
    synth_->renderChannelSamples(ch, bufs[ch], dummy_r.data(), frames);
  }
}

DeviceSnapshot ImfcDevice::snapshot() const {
  DeviceSnapshot snap = makeSnapshot();
  for (std::size_t ch = 0; ch < static_cast<std::size_t>(kChannels); ++ch) {
    if (synth_) {
      snap.channels[ch].active = synth_->isInstrumentSounding(static_cast<int>(ch));
      snap.channels[ch].sounding_note = synth_->isInstrumentSounding(static_cast<int>(ch))
                                            ? synth_->instrumentConfig(static_cast<int>(ch)).voiceNumber
                                            : -1;
      snap.channels[ch].library_voices = true;
    }
  }
  return snap;
}

void ImfcDevice::setMute(int ch, bool muted) {
  MidiDevice::setMute(ch, muted);
  if (muted && synth_) {
    synth_->setInstrumentLevel(ch, 0);
  } else if (!muted && synth_) {
    synth_->setInstrumentLevel(ch, 127);
  }
}

void ImfcDevice::setSolo(int ch, bool soloed) {
  MidiDevice::setSolo(ch, soloed);
}

void ImfcDevice::tick(std::uint32_t frame) {
  (void)frame;
}

void ImfcDevice::handleCommand(std::uint8_t opcode, const std::uint8_t* payload,
                              std::size_t len) {
  (void)opcode;
  (void)payload;
  (void)len;
}

void ImfcDevice::dispatchNoteOn(int ch, int note, int velocity) {
  if (!channelValid(ch)) return;
  wakeChannel(ch);
  if (!inited_ || !synth_) return;

  uint8_t msg[3] = {
      static_cast<uint8_t>(0x90 | (ch & 0x0F)),
      static_cast<uint8_t>(note & 0x7F),
      static_cast<uint8_t>(velocity & 0x7F),
  };
  synth_->sendMidi(msg, 3);
}

void ImfcDevice::dispatchNoteOff(int ch, int note) {
  if (!channelValid(ch)) return;
  if (!inited_ || !synth_) return;

  uint8_t msg[3] = {
      static_cast<uint8_t>(0x80 | (ch & 0x0F)),
      static_cast<uint8_t>(note & 0x7F),
      0,
  };
  synth_->sendMidi(msg, 3);
}

void ImfcDevice::programChange(int ch, int program) {
  dispatchProgramChange(ch, program);
}

void ImfcDevice::dispatchProgramChange(int ch, int program) {
  if (!channelValid(ch)) return;
  wakeChannel(ch);
  setProgram(ch, program);
  if (!inited_ || !synth_) return;

  uint8_t msg[2] = {
      static_cast<uint8_t>(0xC0 | (ch & 0x0F)),
      static_cast<uint8_t>(program & 0x7F),
  };
  synth_->sendMidi(msg, 2);
}

void ImfcDevice::sendControlChange(int ch, int cc, int value) {
  dispatchControlChange(ch, cc, value);
}

void ImfcDevice::dispatchControlChange(int ch, int cc, int value) {
  if (!channelValid(ch)) return;
  wakeChannel(ch);
  setControlChange(ch, cc, value);
  if (!inited_ || !synth_) return;

  uint8_t msg[3] = {
      static_cast<uint8_t>(0xB0 | (ch & 0x0F)),
      static_cast<uint8_t>(cc & 0x7F),
      static_cast<uint8_t>(value & 0x7F),
  };
  synth_->sendMidi(msg, 3);
}

void ImfcDevice::dispatchSysEx(const std::uint8_t* data, std::size_t len) {
  if (data == nullptr || len == 0 || !inited_ || !synth_) return;
  synth_->sendMidi(data, len);
}

std::string ImfcDevice::resolveProgramName(int channel, int program) const {
  if (!synth_) return "Program " + std::to_string(program);
  int bank = synth_->instrumentConfig(channel).voiceBankNumber;
  return std::string(imfcPresetName(bank, program));
}

const char* ImfcDevice::imfcPresetName(int bank, int prog) const {
  if (!synth_) return "Preset";
  return synth_->presetName(bank, prog);
}

void ImfcDevice::applySongTimbres(const std::string& song,
                                  const std::vector<std::uint8_t>& setup,
                                  const std::vector<std::uint8_t>& cleanup) {
  (void)cleanup;
  current_song_ = song;
  if (!setup.empty() && synth_) {
    synth_->sendMidi(setup.data(), setup.size());
  }
}

}  // namespace audio_dbg
