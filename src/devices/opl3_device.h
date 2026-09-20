// Stage 4.1: Concrete OPL3 synthesizer backend.
//
// `Opl3Device` extends `FmDevice` (Tier-2B) with NukedOPL3 synthesis:
//   - main OPL3 chip (full mix) + 18 shadow chips for isolated per-voice
//     rendering (one shadow per voice, 0-8 bank 0, 9-17 bank 1),
//   - lazy shadow init: shadows are only Reset() on first key-on/write to
//     that voice; dormant voices take a 1-cycle fast escape in
//     renderPerChannel() (zeroed buffer, no chip stepping),
//   - register shadow matrix (512 regs across banks 0/1),
//   - raw OPL register writes decoded into FmVoice/ChannelState so the base
//     snapshot and GUI tabs reflect hardware registers.
//
// Register map (YMF262):
//   0x20+slot: AM/VIB/EGT/KSR/MULT   0x40+slot: KSL/TL
//   0x60+slot: AR/DR                 0x80+slot: SL/RR
//   0xE0+slot: WS                    0xA0+vl: FNUM lo
//   0xB0+vl: KEYON/BLOCK/FNUM hi     0xC0+vl: FB/CON + pan
// Slots: MOD_SLOTS[v_local] + bank base; carrier = mod + 3.
//
// See docs/current_plan_debug_frontend.md §4.1.

#ifndef PKMN_AUDIO_DBG_DEVICES_OPL3_DEVICE_H_
#define PKMN_AUDIO_DBG_DEVICES_OPL3_DEVICE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "devices/fm_device.h"

struct _opl3_chip;
typedef struct _opl3_chip opl3_chip;

namespace audio_dbg {

class Opl3Device : public FmDevice {
 public:
  static constexpr int kVoices = 18;
  static constexpr int kRegs = 512;
  static constexpr std::uint32_t kDefaultRate = 49716;

  // Operator slot offsets within a bank (modulator slots; carrier = mod+3).
  static constexpr std::uint8_t kModSlots[9] = {
      0x00, 0x01, 0x02, 0x08, 0x09, 0x0A, 0x10, 0x11, 0x12};

  explicit Opl3Device(int device_id = 10,
                      std::string device_name = "OPL3",
                      std::uint32_t sample_rate = kDefaultRate);
  ~Opl3Device() override;

  Opl3Device(const Opl3Device&) = delete;
  Opl3Device& operator=(const Opl3Device&) = delete;

  // --- SoundDevice lifecycle/rendering ---
  bool init() override;
  void shutdown() override;
  void reset() override;
  void render(float* buf, std::size_t frames) override;
  void renderPerChannel(float** bufs, std::size_t frames) override;

  // --- Raw OPL register write (banked 0x000-0x1F5) ---
  // Updates the shadow matrix, forwards to main + shadow chips, and decodes
  // operator/frequency/key state into the FmDevice base (waking the voice).
  // Out-of-range regs (>= 0x200) are ignored. Global regs (0x001, 0x104,
  // 0x105, 0xBD, 0x08) mirror to all initialized shadows.
  void writeReg(std::uint16_t reg, std::uint8_t val);
  std::uint8_t readReg(std::uint16_t reg) const;

  // --- FmDevice hardware hooks (chip writes live here) ---
  void configureOperator(int voice, bool carrier,
                         const FmOperatorParams& p) override;
  void applyFrequency(int voice, int fnum, int block) override;
  void applyKeyOn(int voice) override;
  void applyKeyOff(int voice) override;
  void handleCommand(std::uint8_t opcode, const std::uint8_t* payload,
                     std::size_t len) override;

  std::uint32_t sampleRate() const { return sample_rate_; }
  bool isShadowInitialized(int voice) const;

 private:
  // Voice/slot helpers.
  static bool regInBank(std::uint16_t reg, int* bank, std::uint16_t* off);
  static int voiceLocalFromSlot(std::uint8_t slot, bool* is_carrier);
  int slotVoice(std::uint8_t slot, int bank, bool* is_carrier) const;
  void ensureShadow(int voice);
  void writeChipReg(opl3_chip* chip, std::uint16_t reg, std::uint8_t val);
  void decodeOperatorSlot(int voice, bool carrier);
  FmOperatorParams decodeSlot(std::uint8_t slot, int bank) const;
  void decodeFrequency(int voice, int bank, int v_local);
  void decodeConnection(int voice, int bank, int v_local);
  static double oplFreqHz(int fnum, int block, std::uint32_t rate);
  bool channelAudible(int ch) const;
  void renderMonoFromChip(opl3_chip* chip, float* out, std::size_t frames);
  void syncChannelFreq(int voice, int fnum, int block);

  std::uint32_t sample_rate_;
  bool inited_ = false;
  std::array<std::uint8_t, kRegs> regs_{};
  opl3_chip* main_chip_ = nullptr;
  opl3_chip* shadows_ = nullptr;  // Array of 18, lazy Reset().
  std::array<bool, kVoices> shadow_init_{};
};

}  // namespace audio_dbg

#endif  // PKMN_AUDIO_DBG_DEVICES_OPL3_DEVICE_H_
