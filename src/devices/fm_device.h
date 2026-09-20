// Stage 2.3: Tier-2B FM intermediate device base.
//
// `FmDevice` extends `SoundDevice` for operator-based FM synthesizers
// (OPL3, future OPL2, Yamaha OPM/YM2151). It owns:
//   - per-voice operator parameters (MULT, TL, AR, DR, SL, RR, KSL, WS plus
//     AM/VIB/EGT tremolo/vibrato/sustain flags),
//   - per-voice frequency (F-number + block), feedback, connection
//     (algorithm), key state and envelope level,
//   - up to 32 voices with lazy wake-up on key-on.
//
// State lives in the base; hardware hooks are pure virtual so concrete
// backends (NukedOPL3 shadow chips, YM2151, …) implement register writes.
// Concrete setters update state, wake the voice, then call the hook.
//
// See docs/current_plan_debug_frontend.md §2.3 / §2.5.

#ifndef PKMN_AUDIO_DBG_DEVICES_FM_DEVICE_H_
#define PKMN_AUDIO_DBG_DEVICES_FM_DEVICE_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "devices/sound_device.h"

namespace audio_dbg {

// One FM operator (MOD or CAR): OPL-style register fields.
struct FmOperatorParams {
  int mult = 1;    // Frequency multiplier 0..15.
  int ar = 15;     // Attack rate 0..15.
  int dr = 0;      // Decay rate 0..15.
  int sl = 0;      // Sustain level 0..15.
  int rr = 0;      // Release rate 0..15.
  int tl = 0;      // Total level 0..63.
  int ksr = 0;     // Key scale rate 0..1.
  bool am = false;   // Tremolo enable.
  bool vib = false;  // Vibrato enable.
  bool egt = false;  // Sustain (envelope-hold) enable.
  int ws = 0;        // Waveform select 0..7.
};

// Full per-voice FM state: modulator + carrier, frequency, routing,
// key/envelope and dormancy.
struct FmVoice {
  FmOperatorParams mod;  // Modulator operator.
  FmOperatorParams car;  // Carrier operator.
  int fnum = 0;          // Frequency number 0..1023.
  int block = 0;         // Octave block 0..7.
  int feedback = 0;      // Modulator self-feedback 0..7.
  int connection = 0;    // Algorithm/connection select (0/1 on OPL2).
  bool keyed_on = false;
  float env_vol = 0.0f;  // 0..1 software envelope estimate.
  bool is_dormant = true;
};

class FmDevice : public SoundDevice {
 public:
  static constexpr int kMaxVoices = 32;

  FmDevice(int device_id, std::string device_name, int voices);
  ~FmDevice() override = default;

  int voiceCount() const { return static_cast<int>(voices_.size()); }
  const FmVoice& voice(int v) const { return voices_.at(static_cast<std::size_t>(v)); }
  bool isVoiceDormant(int v) const;
  // Wakes the voice and its SoundDevice channel (idempotent).
  void wakeVoice(int v);

  // --- Concrete state setters (wake + update + call hardware hook) ---
  void writeOperator(int voice, bool carrier, const FmOperatorParams& p);
  void writeFrequency(int voice, int fnum, int block);
  void writeFeedback(int voice, int feedback, int connection);
  void voiceKeyOn(int voice);
  void voiceKeyOff(int voice);

  // --- SoundDevice: concrete mute/solo/tick/command/snapshot ---
  void setMute(int ch, bool muted) override;
  void setSolo(int ch, bool soloed) override;
  void tick(std::uint32_t frame) override;
  // Non-empty payload wakes payload[0] % voiceCount(); empty wakes nothing.
  void handleCommand(std::uint8_t opcode, const std::uint8_t* payload,
                     std::size_t len) override;
  DeviceSnapshot snapshot() const override;

  // --- Hardware hooks (pure virtual: register writes live here) ---
  virtual void configureOperator(int voice, bool carrier,
                                 const FmOperatorParams& p) = 0;
  virtual void applyFrequency(int voice, int fnum, int block) = 0;
  virtual void applyKeyOn(int voice) = 0;
  virtual void applyKeyOff(int voice) = 0;

 protected:
  bool voiceValid(int v) const;

 private:
  std::vector<FmVoice> voices_;
};

}  // namespace audio_dbg

#endif  // PKMN_AUDIO_DBG_DEVICES_FM_DEVICE_H_
