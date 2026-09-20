// Stage 2.4: Tier-2C PSG intermediate device base.
//
// `PsgDevice` extends `SoundDevice` for programmable sound generators
// (GB-APU, future Innovation SSI-2001/SID, PC Speaker). It owns:
//   - discrete channel types (pulse/duty, wave RAM, LFSR noise),
//   - register-decoded frequency, duty cycle, 32-nibble wave RAM,
//     LFSR width, volume and hardware envelope,
//   - active/dormant state with wake-up on channel activation.
//
// State lives in the base; the register-write hook `applyRegister()` is
// pure virtual for concrete backends (Basic_Gb_Apu shadow chips, …).
//
// See docs/current_plan_debug_frontend.md §2.4 / §2.5.

#ifndef PKMN_AUDIO_DBG_DEVICES_PSG_DEVICE_H_
#define PKMN_AUDIO_DBG_DEVICES_PSG_DEVICE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "devices/sound_device.h"

namespace audio_dbg {

// Discrete PSG channel modes.
enum class PsgChannelType {
  PULSE1 = 0,  // Square with sweep (GB CH1).
  PULSE2 = 1,  // Square without sweep (GB CH2).
  WAVE = 2,    // 32-nibble wave RAM (GB CH3).
  NOISE = 3    // LFSR noise (GB CH4).
};

struct PsgChannel {
  PsgChannelType type = PsgChannelType::PULSE1;
  double frequency = 0.0;  // Hz, 0 = silent/unknown.
  int duty = 2;            // 0..3 -> 12.5/25/50/75%.
  std::array<std::uint8_t, 32> wave_ram{};  // 4-bit nibbles 0..15.
  int lfsr_width = 15;     // 7 or 15 stages.
  int volume = 0;          // 0..15.
  int envelope = 0;        // Hardware envelope setting 0..7.
  bool active = false;
  bool is_dormant = true;
};

class PsgDevice : public SoundDevice {
 public:
  PsgDevice(int device_id, std::string device_name, int channels);
  ~PsgDevice() override = default;

  const PsgChannel& psgChannel(int ch) const;
  PsgChannelType channelType(int ch) const;
  double channelFrequency(int ch) const;
  int channelDuty(int ch) const;
  int channelLfsrWidth(int ch) const;

  // --- Concrete setters (wake + update + call hardware hook) ---
  void setChannelType(int ch, PsgChannelType type);
  void setFrequency(int ch, double hz);
  void setDuty(int ch, int duty);  // Masked to 0..3.
  void setWaveRam(int ch, const std::array<std::uint8_t, 32>& ram);
  void setWaveEntry(int ch, int idx, std::uint8_t val);  // Nibble 0..15.
  void setLfsrWidth(int ch, int width);  // 7 or 15; others ignored.
  void setVolume(int ch, int volume);    // Clamped 0..15.
  void setEnvelope(int ch, int envelope);
  // Wake-up on activation: marks live + sounding.
  void activateChannel(int ch);
  void deactivateChannel(int ch);

  // --- SoundDevice: concrete mute/solo/tick/command/snapshot ---
  void setMute(int ch, bool muted) override;
  void setSolo(int ch, bool soloed) override;
  void tick(std::uint32_t frame) override;
  // Non-empty payload wakes payload[0] % channelCount(); empty wakes nothing.
  void handleCommand(std::uint8_t opcode, const std::uint8_t* payload,
                     std::size_t len) override;
  DeviceSnapshot snapshot() const override;

  // --- Hardware hook (pure virtual: register write lives here) ---
  virtual void applyRegister(int ch) = 0;

 protected:
  bool psgValid(int ch) const;

 private:
  std::vector<PsgChannel> psg_;
};

}  // namespace audio_dbg

#endif  // PKMN_AUDIO_DBG_DEVICES_PSG_DEVICE_H_
