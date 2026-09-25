// Stage 4.6: Concrete IMFC synthesizer backend for dgad.
//
// `ImfcDevice` extends `MidiDevice` (Tier-2A) with the IBM Music Feature Card
// (Yamaha FB-01 / YM2164 4-operator FM sound processor) core adapted from
// DOSBox-X (GPL-2.0-or-later).
//
// Features:
//   - 8 polyphonic channels / instruments.
//   - 240 ROM presets (ROM 1..5 in Banks 2..6) + 96 RAM slots (Bank 0 and 1).
//   - Full Yamaha FB-01 SysEx engine (Memory Protect, Level, Config, Voice Upload,
//     and Store to RAM Bank).
//   - Real-time oscilloscope feed per channel.

#ifndef PKMN_AUDIO_DBG_DEVICES_IMFC_DEVICE_H_
#define PKMN_AUDIO_DBG_DEVICES_IMFC_DEVICE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "devices/imfc_synth_core.h"
#include "devices/midi_device.h"

namespace audio_dbg {

class ImfcDevice : public MidiDevice {
 public:
  static constexpr int kChannels = 8;
  static constexpr std::uint32_t kDefaultRate = 55930;

  explicit ImfcDevice(int device_id = 14,
                      std::string device_name = "IBM Music Feature",
                      std::uint32_t sample_rate = kDefaultRate);
  ~ImfcDevice() override;

  ImfcDevice(const ImfcDevice&) = delete;
  ImfcDevice& operator=(const ImfcDevice&) = delete;

  std::uint32_t sampleRate() const { return sample_rate_; }

  // --- SoundDevice lifecycle & rendering ---
  bool init() override;
  void shutdown() override;
  void reset() override;
  void render(float* buf, std::size_t frames) override;
  void renderPerChannel(float** bufs, std::size_t frames) override;
  DeviceSnapshot snapshot() const override;
  void setMute(int ch, bool muted) override;
  void setSolo(int ch, bool soloed) override;
  void tick(std::uint32_t frame) override;
  void handleCommand(std::uint8_t opcode, const std::uint8_t* payload,
                     std::size_t len) override;

  // --- MidiDevice voice hooks ---
  void dispatchNoteOn(int ch, int note, int velocity) override;
  void dispatchNoteOff(int ch, int note) override;

  // --- Program, CC, and SysEx ---
  void programChange(int ch, int program) override;
  void sendControlChange(int ch, int cc, int value) override;
  void dispatchProgramChange(int ch, int program);
  void dispatchControlChange(int ch, int cc, int value);
  void dispatchSysEx(const std::uint8_t* data, std::size_t len);

  // --- Preset Name Resolution ---
  std::string resolveProgramName(int channel, int program) const;
  const char* imfcPresetName(int bank, int prog) const;

  // --- Custom Voices & SysEx Handover ---
  void applySongTimbres(const std::string& song,
                        const std::vector<std::uint8_t>& setup,
                        const std::vector<std::uint8_t>& cleanup);

  // Access to underlying synth core for ImfcTab
  ImfcSynthCore* synthCore() { return synth_.get(); }
  const ImfcSynthCore* synthCore() const { return synth_.get(); }

 private:
  std::uint32_t sample_rate_;
  bool inited_ = false;
  std::unique_ptr<ImfcSynthCore> synth_;
  std::string current_song_;
};

}  // namespace audio_dbg

#endif  // PKMN_AUDIO_DBG_DEVICES_IMFC_DEVICE_H_
