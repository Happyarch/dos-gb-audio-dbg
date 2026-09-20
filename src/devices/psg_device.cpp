// Stage 2.4: PsgDevice implementation.
// See psg_device.h for the contract.

#include "devices/psg_device.h"

namespace audio_dbg {

PsgDevice::PsgDevice(int device_id, std::string device_name, int channels)
    : SoundDevice(device_id, std::move(device_name),
                  channels <= 0 ? 4 : channels),
      psg_(static_cast<std::size_t>(channelCount())) {
  // Default GB-APU layout: PULSE1, PULSE2, WAVE, NOISE, then PULSE1…
  for (std::size_t i = 0; i < psg_.size(); ++i) {
    switch (i % 4) {
      case 0:
        psg_[i].type = PsgChannelType::PULSE1;
        break;
      case 1:
        psg_[i].type = PsgChannelType::PULSE2;
        break;
      case 2:
        psg_[i].type = PsgChannelType::WAVE;
        break;
      default:
        psg_[i].type = PsgChannelType::NOISE;
        break;
    }
    psg_[i].wave_ram.fill(0);
  }
}

bool PsgDevice::psgValid(int ch) const {
  return ch >= 0 &&
         static_cast<std::size_t>(ch) < psg_.size();
}

const PsgChannel& PsgDevice::psgChannel(int ch) const {
  return psg_.at(static_cast<std::size_t>(ch));
}

PsgChannelType PsgDevice::channelType(int ch) const {
  if (!psgValid(ch)) return PsgChannelType::PULSE1;
  return psg_[static_cast<std::size_t>(ch)].type;
}

double PsgDevice::channelFrequency(int ch) const {
  if (!psgValid(ch)) return 0.0;
  return psg_[static_cast<std::size_t>(ch)].frequency;
}

int PsgDevice::channelDuty(int ch) const {
  if (!psgValid(ch)) return 0;
  return psg_[static_cast<std::size_t>(ch)].duty;
}

int PsgDevice::channelLfsrWidth(int ch) const {
  if (!psgValid(ch)) return 15;
  return psg_[static_cast<std::size_t>(ch)].lfsr_width;
}

void PsgDevice::setChannelType(int ch, PsgChannelType type) {
  if (!psgValid(ch)) return;
  wakeChannel(ch);
  PsgChannel& p = psg_[static_cast<std::size_t>(ch)];
  p.type = type;
  p.is_dormant = false;
  applyRegister(ch);
}

void PsgDevice::setFrequency(int ch, double hz) {
  if (!psgValid(ch)) return;
  wakeChannel(ch);
  PsgChannel& p = psg_[static_cast<std::size_t>(ch)];
  p.frequency = hz < 0.0 ? 0.0 : hz;
  p.is_dormant = false;
  applyRegister(ch);
}

void PsgDevice::setDuty(int ch, int duty) {
  if (!psgValid(ch)) return;
  wakeChannel(ch);
  PsgChannel& p = psg_[static_cast<std::size_t>(ch)];
  p.duty = duty & 3;
  p.is_dormant = false;
  applyRegister(ch);
}

void PsgDevice::setWaveRam(int ch,
                           const std::array<std::uint8_t, 32>& ram) {
  if (!psgValid(ch)) return;
  wakeChannel(ch);
  PsgChannel& p = psg_[static_cast<std::size_t>(ch)];
  for (std::size_t i = 0; i < 32; ++i) p.wave_ram[i] = ram[i] & 0x0F;
  p.is_dormant = false;
  applyRegister(ch);
}

void PsgDevice::setWaveEntry(int ch, int idx, std::uint8_t val) {
  if (!psgValid(ch)) return;
  if (idx < 0 || idx >= 32) return;
  wakeChannel(ch);
  PsgChannel& p = psg_[static_cast<std::size_t>(ch)];
  p.wave_ram[static_cast<std::size_t>(idx)] = val & 0x0F;
  p.is_dormant = false;
  applyRegister(ch);
}

void PsgDevice::setLfsrWidth(int ch, int width) {
  if (!psgValid(ch)) return;
  if (width != 7 && width != 15) return;
  wakeChannel(ch);
  PsgChannel& p = psg_[static_cast<std::size_t>(ch)];
  p.lfsr_width = width;
  p.is_dormant = false;
  applyRegister(ch);
}

void PsgDevice::setVolume(int ch, int volume) {
  if (!psgValid(ch)) return;
  wakeChannel(ch);
  if (volume < 0) volume = 0;
  if (volume > 15) volume = 15;
  PsgChannel& p = psg_[static_cast<std::size_t>(ch)];
  p.volume = volume;
  p.is_dormant = false;
  applyRegister(ch);
}

void PsgDevice::setEnvelope(int ch, int envelope) {
  if (!psgValid(ch)) return;
  wakeChannel(ch);
  PsgChannel& p = psg_[static_cast<std::size_t>(ch)];
  p.envelope = envelope;
  p.is_dormant = false;
  applyRegister(ch);
}

void PsgDevice::activateChannel(int ch) {
  if (!psgValid(ch)) return;
  wakeChannel(ch);
  PsgChannel& p = psg_[static_cast<std::size_t>(ch)];
  p.active = true;
  p.is_dormant = false;
  applyRegister(ch);
}

void PsgDevice::deactivateChannel(int ch) {
  if (!psgValid(ch)) return;
  wakeChannel(ch);
  PsgChannel& p = psg_[static_cast<std::size_t>(ch)];
  p.active = false;
  applyRegister(ch);
}

void PsgDevice::setMute(int ch, bool muted) { setMutedState(ch, muted); }

void PsgDevice::setSolo(int ch, bool soloed) { setSoloedState(ch, soloed); }

void PsgDevice::tick(std::uint32_t frame) { setFrame(frame); }

void PsgDevice::handleCommand(std::uint8_t /*opcode*/,
                              const std::uint8_t* payload, std::size_t len) {
  if (payload == nullptr || len == 0) return;
  if (channelCount() == 0) return;
  wakeChannel(static_cast<int>(payload[0]) % channelCount());
}

DeviceSnapshot PsgDevice::snapshot() const { return makeSnapshot(); }

}  // namespace audio_dbg
