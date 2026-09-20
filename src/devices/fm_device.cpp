// Stage 2.3: FmDevice implementation.
// See fm_device.h for the contract.

#include "devices/fm_device.h"

namespace audio_dbg {

FmDevice::FmDevice(int device_id, std::string device_name, int voices)
    : SoundDevice(device_id, std::move(device_name),
                  voices <= 0 ? 0
                  : voices > kMaxVoices ? kMaxVoices
                                        : voices),
      voices_(static_cast<std::size_t>(channelCount())) {}

bool FmDevice::voiceValid(int v) const {
  return v >= 0 &&
         static_cast<std::size_t>(v) < voices_.size();
}

bool FmDevice::isVoiceDormant(int v) const {
  if (!voiceValid(v)) return true;
  return voices_[static_cast<std::size_t>(v)].is_dormant;
}

void FmDevice::wakeVoice(int v) {
  if (!voiceValid(v)) return;
  voices_[static_cast<std::size_t>(v)].is_dormant = false;
  wakeChannel(v);
}

void FmDevice::resetVoices() {
  for (std::size_t i = 0; i < voices_.size(); ++i) {
    voices_[i].keyed_on = false;
    voices_[i].env_vol = 0.0f;
    if (channelValid(static_cast<int>(i))) {
      ChannelState& st = channel(static_cast<int>(i));
      st.active = false;
      st.freq = 0.0f;
      st.last_note = -1;
      st.peak = 0.0f;
    }
  }
}

void FmDevice::writeOperator(int voice, bool carrier,
                             const FmOperatorParams& p) {
  if (!voiceValid(voice)) return;
  wakeVoice(voice);
  FmVoice& vs = voices_[static_cast<std::size_t>(voice)];
  if (carrier) {
    vs.car = p;
  } else {
    vs.mod = p;
  }
  configureOperator(voice, carrier, p);
}

void FmDevice::writeFrequency(int voice, int fnum, int block) {
  if (!voiceValid(voice)) return;
  wakeVoice(voice);
  if (fnum < 0) fnum = 0;
  if (fnum > 1023) fnum = 1023;
  if (block < 0) block = 0;
  if (block > 7) block = 7;
  FmVoice& vs = voices_[static_cast<std::size_t>(voice)];
  vs.fnum = fnum;
  vs.block = block;
  applyFrequency(voice, fnum, block);
}

void FmDevice::writeFeedback(int voice, int feedback, int connection) {
  if (!voiceValid(voice)) return;
  wakeVoice(voice);
  if (feedback < 0) feedback = 0;
  if (feedback > 7) feedback = 7;
  FmVoice& vs = voices_[static_cast<std::size_t>(voice)];
  vs.feedback = feedback;
  vs.connection = connection;
}

void FmDevice::voiceKeyOn(int voice) {
  if (!voiceValid(voice)) return;
  wakeVoice(voice);
  FmVoice& vs = voices_[static_cast<std::size_t>(voice)];
  vs.keyed_on = true;
  vs.env_vol = 1.0f;
  applyKeyOn(voice);
}

void FmDevice::voiceKeyOff(int voice) {
  if (!voiceValid(voice)) return;
  wakeVoice(voice);
  FmVoice& vs = voices_[static_cast<std::size_t>(voice)];
  vs.keyed_on = false;
  vs.env_vol = 0.0f;
  applyKeyOff(voice);
}

void FmDevice::setMute(int ch, bool muted) { setMutedState(ch, muted); }

void FmDevice::setSolo(int ch, bool soloed) { setSoloedState(ch, soloed); }

void FmDevice::tick(std::uint32_t frame) { setFrame(frame); }

void FmDevice::handleCommand(std::uint8_t /*opcode*/,
                             const std::uint8_t* payload, std::size_t len) {
  if (payload == nullptr || len == 0) return;
  if (voiceCount() == 0) return;
  wakeVoice(static_cast<int>(payload[0]) % voiceCount());
}

DeviceSnapshot FmDevice::snapshot() const { return makeSnapshot(); }

}  // namespace audio_dbg
