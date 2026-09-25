// SPDX-License-Identifier: GPL-2.0-or-later
//
// imfc_synth_core.h — IBM Music Feature Card (IMFC) synthesis core for dgad.
//
// Adapts the Yamaha FB-01 sound processing state machine and YM2151 FM core
// from dos_port/tools/dosbox-x/src/hardware/imfc.cpp.

#ifndef PKMN_AUDIO_DBG_DEVICES_IMFC_SYNTH_CORE_H_
#define PKMN_AUDIO_DBG_DEVICES_IMFC_SYNTH_CORE_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace audio_dbg {

// 8-byte OperatorDefinition in Yamaha FB-01 VoiceDefinition
struct ImfcOperatorDef {
  uint8_t bytes[8] = {};

  uint8_t getTotalLevel() const { return bytes[0] & 0x7F; }
  uint8_t getMultiple() const { return bytes[3] & 0x0F; }
  uint8_t getDetune() const { return (bytes[3] >> 4) & 0x07; }
  uint8_t getAttackRate() const { return bytes[4] & 0x1F; }
  uint8_t getDecay1Rate() const { return bytes[5] & 0x1F; }
  uint8_t getDecay2Rate() const { return bytes[6] & 0x1F; }
  uint8_t getSustainLevel() const { return (bytes[7] >> 4) & 0x0F; }
  uint8_t getReleaseRate() const { return bytes[7] & 0x0F; }
};

// 64-byte VoiceDefinition in Yamaha FB-01 architecture
struct ImfcVoiceDef {
  char name[7] = {};
  uint8_t reserved1 = 0;
  uint8_t lfoSpeed = 0;
  uint8_t byte9 = 0;   // LFO load mode + AMD
  uint8_t byteA = 0;   // LFO sync mode + PMD
  uint8_t byteB = 0;   // Operators enabled
  uint8_t byteC = 0;   // Algorithm + Feedback
  uint8_t byteD = 0;   // PMS + AMS
  uint8_t byteE = 0;   // LFO waveform
  uint8_t transpose = 0;
  ImfcOperatorDef operators[4] = {};
  uint8_t reserved2[10] = {};
  uint8_t field_3A = 0; // Mono/poly + portamento
  uint8_t field_3B = 0; // Pitchbender range + PMD controller
  uint8_t reserved3[4] = {};

  uint8_t getAlgorithm() const { return byteC & 0x07; }
  uint8_t getFeedback() const { return (byteC >> 3) & 0x07; }
  std::string getName() const {
    char buf[8];
    for (int i = 0; i < 7; ++i) buf[i] = name[i];
    buf[7] = '\0';
    return std::string(buf);
  }
};
static_assert(sizeof(ImfcVoiceDef) == 64, "ImfcVoiceDef must be exactly 64 bytes");

struct ImfcInstrumentConfig {
  uint8_t numberOfNotes = 1;
  uint8_t midiChannel = 0;
  uint8_t keyLimitHigh = 127;
  uint8_t keyLimitLow = 0;
  uint8_t voiceBankNumber = 2; // Default: Bank 2 = ROM 1
  uint8_t voiceNumber = 0;     // 0..47
  uint8_t detune = 0;
  uint8_t noteShift = 0;
  uint8_t outputLevel = 127;
  uint8_t pan = 64;            // 0=L, 64=Center, 127=R
  uint8_t lfoEnable = 1;
  uint8_t portamento = 0;
  uint8_t pitchbenderRange = 2;
  uint8_t monoPolyMode = 0;    // 0=poly, 1=mono
};

class ImfcSynthCore {
 public:
  static constexpr int kChannels = 8;         // 8 FB-01 instruments
  static constexpr uint32_t kNativeRate = 55930;

  ImfcSynthCore();
  ~ImfcSynthCore();

  bool init(std::uint32_t sample_rate = kNativeRate);
  void reset();

  // Feed raw MIDI bytes (handles channel voice messages, controllers, and FB-01 SysEx)
  void sendMidiByte(std::uint8_t byte);
  void sendMidi(const std::uint8_t* msg, std::size_t len);

  // Audio rendering
  void renderSamples(float* out_l, float* out_r, std::size_t frames);
  void renderChannelSamples(int ch, float* out_l, float* out_r, std::size_t frames);

  // Inspector & state access
  bool isInstrumentSounding(int inst) const;
  int activeNotes(int inst) const;
  const ImfcInstrumentConfig& instrumentConfig(int inst) const;
  const ImfcVoiceDef& activeVoice(int inst) const;
  const ImfcVoiceDef& customVoice(int bank, int slot) const; // bank 0..1, slot 0..47
  const char* presetName(int bank, int prog) const;          // bank 0..6, prog 0..47

  uint8_t masterVolume() const;
  void setMasterVolume(uint8_t vol);

  bool memoryProtect() const;
  void setMemoryProtect(bool protect);

  // Direct parameter tweaking
  void setInstrumentBank(int inst, int bank);
  void setInstrumentProgram(int inst, int prog);
  void setInstrumentLevel(int inst, int level);
  void setInstrumentPan(int inst, int pan);

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace audio_dbg

#endif  // PKMN_AUDIO_DBG_DEVICES_IMFC_SYNTH_CORE_H_
