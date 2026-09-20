// Stage 4.1: Opl3Device implementation.
// See opl3_device.h for the contract.

#include "devices/opl3_device.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "nukedopl.h"

namespace audio_dbg {

namespace {

// OPL3 global/mode registers mirrored to every initialized shadow chip.
bool IsGlobalReg(std::uint16_t reg) {
  switch (reg) {
    case 0x001:
    case 0x008:
    case 0x0BD:
    case 0x101:
    case 0x104:
    case 0x105:
      return true;
    default:
      return false;
  }
}

}  // namespace

Opl3Device::Opl3Device(int device_id, std::string device_name,
                       std::uint32_t sample_rate)
    : FmDevice(device_id, std::move(device_name), kVoices),
      sample_rate_(sample_rate == 0 ? kDefaultRate : sample_rate) {
  regs_.fill(0);
  shadow_init_.fill(false);
}

Opl3Device::~Opl3Device() {
  shutdown();
}

bool Opl3Device::init() {
  if (inited_) return true;
  if (main_chip_ == nullptr) {
    main_chip_ =
        static_cast<opl3_chip*>(std::calloc(1, sizeof(opl3_chip)));
    if (main_chip_ == nullptr) return false;
  }
  if (shadows_ == nullptr) {
    shadows_ = static_cast<opl3_chip*>(
        std::calloc(static_cast<std::size_t>(kVoices), sizeof(opl3_chip)));
    if (shadows_ == nullptr) return false;
  }
  regs_.fill(0);
  shadow_init_.fill(false);
  OPL3_Reset(main_chip_, sample_rate_);
  // OPL3 mode + WS enable + 2-op (mirrors audition OplEngine.reset()).
  OPL3_WriteReg(main_chip_, 0x105, 0x01);
  regs_[0x105] = 0x01;
  OPL3_WriteReg(main_chip_, 0x001, 0x20);
  regs_[0x001] = 0x20;
  OPL3_WriteReg(main_chip_, 0x104, 0x00);
  regs_[0x104] = 0x00;
  inited_ = true;
  return true;
}

void Opl3Device::shutdown() {
  if (main_chip_ != nullptr) {
    std::free(main_chip_);
    main_chip_ = nullptr;
  }
  if (shadows_ != nullptr) {
    std::free(shadows_);
    shadows_ = nullptr;
  }
  shadow_init_.fill(false);
  inited_ = false;
}

void Opl3Device::reset() {
  if (!inited_) return;
  regs_.fill(0);
  shadow_init_.fill(false);
  OPL3_Reset(main_chip_, sample_rate_);
  OPL3_WriteReg(main_chip_, 0x105, 0x01);
  regs_[0x105] = 0x01;
  OPL3_WriteReg(main_chip_, 0x001, 0x20);
  regs_[0x001] = 0x20;
  OPL3_WriteReg(main_chip_, 0x104, 0x00);
  regs_[0x104] = 0x00;
}

bool Opl3Device::regInBank(std::uint16_t reg, int* bank, std::uint16_t* off) {
  if (reg >= 0x200) return false;
  if (reg >= 0x100) {
    if (bank != nullptr) *bank = 1;
    if (off != nullptr) *off = reg - 0x100;
  } else {
    if (bank != nullptr) *bank = 0;
    if (off != nullptr) *off = reg;
  }
  return true;
}

int Opl3Device::voiceLocalFromSlot(std::uint8_t slot, bool* is_carrier) {
  for (int v = 0; v < 9; ++v) {
    if (slot == kModSlots[v]) {
      if (is_carrier != nullptr) *is_carrier = false;
      return v;
    }
    if (slot == static_cast<std::uint8_t>(kModSlots[v] + 3)) {
      if (is_carrier != nullptr) *is_carrier = true;
      return v;
    }
  }
  return -1;
}

int Opl3Device::slotVoice(std::uint8_t slot, int bank,
                          bool* is_carrier) const {
  const int v_local = voiceLocalFromSlot(slot, is_carrier);
  if (v_local < 0) return -1;
  return bank * 9 + v_local;
}

void Opl3Device::writeChipReg(opl3_chip* chip, std::uint16_t reg,
                              std::uint8_t val) {
  if (chip == nullptr || !inited_) return;
  OPL3_WriteReg(chip, reg, val);
}

void Opl3Device::ensureShadow(int voice) {
  if (voice < 0 || voice >= kVoices) return;
  if (shadow_init_[static_cast<std::size_t>(voice)]) return;
  if (shadows_ == nullptr || !inited_) return;
  opl3_chip* sh = &shadows_[static_cast<std::size_t>(voice)];
  OPL3_Reset(sh, sample_rate_);
  // Replay global/mode registers so the isolated voice runs in the same
  // mode as the main mix (full addresses, no bank duplication).
  for (std::uint16_t g : {0x001u, 0x008u, 0x0BDu, 0x101u, 0x104u, 0x105u}) {
    OPL3_WriteReg(sh, g, regs_[g]);
  }
  // Replay this voice's operator/frequency/connection registers.
  const int bank = voice / 9;
  const int v_local = voice % 9;
  const std::uint16_t base = bank == 1 ? 0x100 : 0x000;
  const std::uint8_t mod = kModSlots[v_local];
  const std::uint8_t car = static_cast<std::uint8_t>(mod + 3);
  for (std::uint8_t slot : {mod, car}) {
    for (std::uint16_t r : {0x20u, 0x40u, 0x60u, 0x80u, 0xE0u}) {
      const std::uint16_t reg = base + r + slot;
      OPL3_WriteReg(sh, reg, regs_[reg]);
    }
  }
  OPL3_WriteReg(sh, base + 0xA0 + static_cast<std::uint16_t>(v_local),
                regs_[base + 0xA0 + static_cast<std::size_t>(v_local)]);
  OPL3_WriteReg(sh, base + 0xB0 + static_cast<std::size_t>(v_local),
                regs_[base + 0xB0 + static_cast<std::size_t>(v_local)]);
  OPL3_WriteReg(sh, base + 0xC0 + static_cast<std::size_t>(v_local),
                regs_[base + 0xC0 + static_cast<std::size_t>(v_local)]);
  shadow_init_[static_cast<std::size_t>(voice)] = true;
}

bool Opl3Device::isShadowInitialized(int voice) const {
  if (voice < 0 || voice >= kVoices) return false;
  return shadow_init_[static_cast<std::size_t>(voice)];
}

FmOperatorParams Opl3Device::decodeSlot(std::uint8_t slot, int bank) const {
  FmOperatorParams p;
  const std::uint16_t base = bank == 1 ? 0x100 : 0x000;
  const std::uint8_t v20 = regs_[base + 0x20 + slot];
  const std::uint8_t v40 = regs_[base + 0x40 + slot];
  const std::uint8_t v60 = regs_[base + 0x60 + slot];
  const std::uint8_t v80 = regs_[base + 0x80 + slot];
  const std::uint8_t vE0 = regs_[base + 0xE0 + slot];
  p.mult = v20 & 0x0F;
  p.ksr = (v20 >> 4) & 0x01;
  p.egt = (v20 & 0x20) != 0;
  p.vib = (v20 & 0x40) != 0;
  p.am = (v20 & 0x80) != 0;
  p.tl = v40 & 0x3F;
  p.ksr = (v20 >> 4) & 0x01;
  // KSL lives in v40 bits 7-6; FmOperatorParams has no KSL field, so the
  // carrier TL tracked in the base stays exact while KSL remains visible
  // in the raw shadow matrix (readReg) for the tab.
  p.ar = (v60 >> 4) & 0x0F;
  p.dr = v60 & 0x0F;
  p.sl = (v80 >> 4) & 0x0F;
  p.rr = v80 & 0x0F;
  p.ws = vE0 & 0x07;
  (void)bank;
  return p;
}

void Opl3Device::decodeOperatorSlot(int voice, bool carrier) {
  if (voice < 0 || voice >= kVoices) return;
  const int bank = voice / 9;
  const int v_local = voice % 9;
  const std::uint8_t slot = carrier
                                ? static_cast<std::uint8_t>(kModSlots[v_local] + 3)
                                : kModSlots[v_local];
  const FmOperatorParams p = decodeSlot(slot, bank);
  // Base setter wakes the voice and calls configureOperator (which
  // re-writes the same 5 registers — harmless duplicate of writeReg's
  // direct chip write).
  writeOperator(voice, carrier, p);
}

void Opl3Device::decodeFrequency(int voice, int bank, int v_local) {
  if (voice < 0 || voice >= kVoices) return;
  const std::uint16_t base = bank == 1 ? 0x100 : 0x000;
  const std::uint8_t a0 =
      regs_[base + 0xA0 + static_cast<std::size_t>(v_local)];
  const std::uint8_t b0 =
      regs_[base + 0xB0 + static_cast<std::size_t>(v_local)];
  const int fnum = (a0 | ((b0 & 0x03) << 8)) & 0x3FF;
  const int block = (b0 >> 2) & 0x07;
  const bool key_on = (b0 & 0x20) != 0;
  writeFrequency(voice, fnum, block);
  syncChannelFreq(voice, fnum, block);
  if (key_on) {
    voiceKeyOn(voice);
  } else {
    voiceKeyOff(voice);
  }
}

void Opl3Device::decodeConnection(int voice, int bank, int v_local) {
  if (voice < 0 || voice >= kVoices) return;
  const std::uint16_t base = bank == 1 ? 0x100 : 0x000;
  const std::uint8_t c0 =
      regs_[base + 0xC0 + static_cast<std::size_t>(v_local)];
  const int feedback = (c0 >> 1) & 0x07;
  const int connection = c0 & 0x01;
  // writeFeedback wakes but has no hardware hook; writeReg already wrote
  // the chip directly, so no duplicate write occurs here.
  writeFeedback(voice, feedback, connection);
}

double Opl3Device::oplFreqHz(int fnum, int block, std::uint32_t rate) {
  if (fnum <= 0) return 0.0;
  if (block < 0) block = 0;
  if (block > 7) block = 7;
  return static_cast<double>(fnum) * static_cast<double>(rate) /
         static_cast<double>(1 << (20 - block));
}

void Opl3Device::syncChannelFreq(int voice, int fnum, int block) {
  if (voice < 0 || voice >= channelCount()) return;
  channel(voice).freq = static_cast<float>(oplFreqHz(fnum, block, sample_rate_));
}

void Opl3Device::writeReg(std::uint16_t reg, std::uint8_t val) {
  int bank = 0;
  std::uint16_t off = 0;
  if (!regInBank(reg, &bank, &off)) return;
  if (!inited_) return;
  regs_[reg] = val;
  // Direct chip writes (single path; base setters below re-write the same
  // values through the hooks — duplicates are benign at control rate).
  writeChipReg(main_chip_, reg, val);
  if (IsGlobalReg(reg)) {
    for (int v = 0; v < kVoices; ++v) {
      if (shadow_init_[static_cast<std::size_t>(v)]) {
        writeChipReg(&shadows_[static_cast<std::size_t>(v)], reg, val);
      }
    }
    return;
  }
  // Per-voice registers: A0/B0/C0 address the voice; 0x20/0x40/0x60/0x80/
  // 0xE0 address the operator slot.
  if (off >= 0xA0 && off <= 0xA8) {
    const int v_local = off - 0xA0;
    const int voice = bank * 9 + v_local;
    ensureShadow(voice);
    if (isShadowInitialized(voice)) {
      writeChipReg(&shadows_[static_cast<std::size_t>(voice)], reg, val);
    }
    decodeFrequency(voice, bank, v_local);
    return;
  }
  if (off >= 0xB0 && off <= 0xB8) {
    const int v_local = off - 0xB0;
    const int voice = bank * 9 + v_local;
    ensureShadow(voice);
    if (isShadowInitialized(voice)) {
      writeChipReg(&shadows_[static_cast<std::size_t>(voice)], reg, val);
    }
    decodeFrequency(voice, bank, v_local);
    return;
  }
  if (off >= 0xC0 && off <= 0xC8) {
    const int v_local = off - 0xC0;
    const int voice = bank * 9 + v_local;
    ensureShadow(voice);
    if (isShadowInitialized(voice)) {
      writeChipReg(&shadows_[static_cast<std::size_t>(voice)], reg, val);
    }
    decodeConnection(voice, bank, v_local);
    return;
  }
  // Operator registers.
  std::uint16_t base = 0;
  std::uint8_t slot = 0;
  bool is_op = false;
  for (std::uint16_t r : {0x20u, 0x40u, 0x60u, 0x80u, 0xE0u}) {
    if (off >= r && off <= r + 0x15) {
      base = r;
      slot = static_cast<std::uint8_t>(off - r);
      is_op = true;
      break;
    }
  }
  if (!is_op) {
    // Other registers (e.g. 0xBD rhythm, timers): main-chip only.
    return;
  }
  bool is_carrier = false;
  const int voice = slotVoice(slot, bank, &is_carrier);
  if (voice < 0) return;  // Unmapped slot (e.g. rhythm gap): no voice wakes.
  ensureShadow(voice);
  if (isShadowInitialized(voice)) {
    writeChipReg(&shadows_[static_cast<std::size_t>(voice)], reg, val);
  }
  (void)base;
  decodeOperatorSlot(voice, is_carrier);
}

std::uint8_t Opl3Device::readReg(std::uint16_t reg) const {
  if (reg >= 0x200) return 0;
  return regs_[reg];
}

void Opl3Device::configureOperator(int voice, bool carrier,
                                   const FmOperatorParams& p) {
  if (voice < 0 || voice >= kVoices || !inited_) return;
  ensureShadow(voice);
  const int bank = voice / 9;
  const int v_local = voice % 9;
  const std::uint16_t base = bank == 1 ? 0x100 : 0x000;
  const std::uint8_t slot = carrier
                                ? static_cast<std::uint8_t>(kModSlots[v_local] + 3)
                                : kModSlots[v_local];
  const std::uint8_t v20 = static_cast<std::uint8_t>(
      (p.am ? 0x80 : 0) | (p.vib ? 0x40 : 0) | (p.egt ? 0x20 : 0) |
      ((p.ksr & 0x01) << 4) | (p.mult & 0x0F));
  const std::uint8_t v40 = static_cast<std::uint8_t>(p.tl & 0x3F);
  const std::uint8_t v60 = static_cast<std::uint8_t>(((p.ar & 0x0F) << 4) |
                                                    (p.dr & 0x0F));
  const std::uint8_t v80 = static_cast<std::uint8_t>(((p.sl & 0x0F) << 4) |
                                                    (p.rr & 0x0F));
  const std::uint8_t vE0 = static_cast<std::uint8_t>(p.ws & 0x07);
  regs_[base + 0x20 + slot] = v20;
  regs_[base + 0x40 + slot] = v40;
  regs_[base + 0x60 + slot] = v60;
  regs_[base + 0x80 + slot] = v80;
  regs_[base + 0xE0 + slot] = vE0;
  writeChipReg(main_chip_, base + 0x20 + slot, v20);
  writeChipReg(main_chip_, base + 0x40 + slot, v40);
  writeChipReg(main_chip_, base + 0x60 + slot, v60);
  writeChipReg(main_chip_, base + 0x80 + slot, v80);
  writeChipReg(main_chip_, base + 0xE0 + slot, vE0);
  if (isShadowInitialized(voice)) {
    opl3_chip* sh = &shadows_[static_cast<std::size_t>(voice)];
    writeChipReg(sh, base + 0x20 + slot, v20);
    writeChipReg(sh, base + 0x40 + slot, v40);
    writeChipReg(sh, base + 0x60 + slot, v60);
    writeChipReg(sh, base + 0x80 + slot, v80);
    writeChipReg(sh, base + 0xE0 + slot, vE0);
  }
}

void Opl3Device::applyFrequency(int voice, int fnum, int block) {
  if (voice < 0 || voice >= kVoices || !inited_) return;
  ensureShadow(voice);
  if (fnum < 0) fnum = 0;
  if (fnum > 1023) fnum = 1023;
  if (block < 0) block = 0;
  if (block > 7) block = 7;
  const int bank = voice / 9;
  const int v_local = voice % 9;
  const std::uint16_t base = bank == 1 ? 0x100 : 0x000;
  const std::uint8_t a0 = static_cast<std::uint8_t>(fnum & 0xFF);
  // Preserve the live KEYON bit: frequency slides must not key off.
  const std::uint8_t live_b0 =
      regs_[base + 0xB0 + static_cast<std::size_t>(v_local)];
  const std::uint8_t b0 = static_cast<std::uint8_t>(
      (live_b0 & 0x20) | ((block & 0x07) << 2) | ((fnum >> 8) & 0x03));
  regs_[base + 0xA0 + static_cast<std::size_t>(v_local)] = a0;
  regs_[base + 0xB0 + static_cast<std::size_t>(v_local)] = b0;
  writeChipReg(main_chip_, base + 0xA0 + static_cast<std::size_t>(v_local), a0);
  writeChipReg(main_chip_, base + 0xB0 + static_cast<std::size_t>(v_local), b0);
  if (isShadowInitialized(voice)) {
    opl3_chip* sh = &shadows_[static_cast<std::size_t>(voice)];
    writeChipReg(sh, base + 0xA0 + static_cast<std::size_t>(v_local), a0);
    writeChipReg(sh, base + 0xB0 + static_cast<std::size_t>(v_local), b0);
  }
  syncChannelFreq(voice, fnum, block);
}

void Opl3Device::applyKeyOn(int voice) {
  if (voice < 0 || voice >= kVoices || !inited_) return;
  ensureShadow(voice);
  const int bank = voice / 9;
  const int v_local = voice % 9;
  const std::uint16_t base = bank == 1 ? 0x100 : 0x000;
  const FmVoice& vs = this->voice(voice);
  const std::uint8_t b0 = static_cast<std::uint8_t>(
      0x20 | ((vs.block & 0x07) << 2) | ((vs.fnum >> 8) & 0x03));
  regs_[base + 0xA0 + static_cast<std::size_t>(v_local)] =
      static_cast<std::uint8_t>(vs.fnum & 0xFF);
  regs_[base + 0xB0 + static_cast<std::size_t>(v_local)] = b0;
  writeChipReg(main_chip_, base + 0xA0 + static_cast<std::size_t>(v_local),
               static_cast<std::uint8_t>(vs.fnum & 0xFF));
  writeChipReg(main_chip_, base + 0xB0 + static_cast<std::size_t>(v_local), b0);
  if (isShadowInitialized(voice)) {
    opl3_chip* sh = &shadows_[static_cast<std::size_t>(voice)];
    writeChipReg(sh, base + 0xA0 + static_cast<std::size_t>(v_local),
                 static_cast<std::uint8_t>(vs.fnum & 0xFF));
    writeChipReg(sh, base + 0xB0 + static_cast<std::size_t>(v_local), b0);
  }
  syncChannelFreq(voice, vs.fnum, vs.block);
}

void Opl3Device::applyKeyOff(int voice) {
  if (voice < 0 || voice >= kVoices || !inited_) return;
  ensureShadow(voice);
  const int bank = voice / 9;
  const int v_local = voice % 9;
  const std::uint16_t base = bank == 1 ? 0x100 : 0x000;
  const FmVoice& vs = this->voice(voice);
  const std::uint8_t b0 = static_cast<std::uint8_t>(
      ((vs.block & 0x07) << 2) | ((vs.fnum >> 8) & 0x03));
  regs_[base + 0xB0 + static_cast<std::size_t>(v_local)] = b0;
  writeChipReg(main_chip_, base + 0xB0 + static_cast<std::size_t>(v_local), b0);
  if (isShadowInitialized(voice)) {
    writeChipReg(&shadows_[static_cast<std::size_t>(voice)],
                 base + 0xB0 + static_cast<std::size_t>(v_local), b0);
  }
}

void Opl3Device::handleCommand(std::uint8_t opcode,
                               const std::uint8_t* payload, std::size_t len) {
  if (payload == nullptr || len == 0 || !inited_) return;
  const int ch = static_cast<int>(payload[0]);
  const int voice = ch % kVoices;
  wakeVoice(voice);

  if (opcode == 0x90 && len >= 3) {
    const int note = static_cast<int>(payload[1]);
    const int vel = static_cast<int>(payload[2]);
    if (vel == 0) {
      applyKeyOff(voice);
      channel(voice).active = false;
      return;
    }
    channel(voice).last_note = note;
    channel(voice).active = true;
    const double f = 440.0 * std::pow(2.0, (static_cast<double>(note) - 69.0) / 12.0);
    channel(voice).freq = static_cast<float>(f);
    int block = 7;
    int fnum = 1023;
    for (int b = 0; b < 8; ++b) {
      const double fn =
          (f * static_cast<double>(1 << (20 - b))) / static_cast<double>(sample_rate_);
      if (fn <= 1023.0) {
        block = b;
        fnum = static_cast<int>(fn + 0.5);
        break;
      }
    }
    // Set default operator patch if never configured on chip.
    const int v_local = voice % 9;
    const std::uint16_t c_base = (voice / 9) == 1 ? 0x100 : 0x000;
    if (readReg(c_base + 0xC0 + static_cast<std::size_t>(v_local)) == 0) {
      FmOperatorParams mod_p;
      mod_p.mult = 1; mod_p.tl = 0x1A; mod_p.ar = 15; mod_p.dr = 0; mod_p.sl = 0; mod_p.rr = 7;
      mod_p.egt = true;
      FmOperatorParams car_p;
      car_p.mult = 1; car_p.tl = 0; car_p.ar = 15; car_p.dr = 0; car_p.sl = 0; car_p.rr = 7;
      car_p.egt = true;
      writeOperator(voice, false, mod_p);
      writeOperator(voice, true, car_p);
      writeFeedback(voice, 1, 0);
      writeReg(c_base + 0xC0 + static_cast<std::size_t>(v_local), 0x32);
    }
    writeFrequency(voice, fnum, block);
    voiceKeyOn(voice);
  } else if (opcode == 0x80) {
    voiceKeyOff(voice);
    channel(voice).active = false;
  }
}

bool Opl3Device::channelAudible(int ch) const {
  if (ch < 0 || ch >= channelCount()) return false;
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

void Opl3Device::renderMonoFromChip(opl3_chip* chip, float* out,
                                    std::size_t frames) {
  if (out == nullptr || frames == 0 || chip == nullptr) return;
  // OPL3_GenerateStream writes interleaved stereo int16; convert to mono.
  constexpr std::size_t kChunk = 256;
  std::int16_t stereo[kChunk * 2];
  std::size_t done = 0;
  while (done < frames) {
    const std::size_t want =
        std::min(kChunk, frames - done);
    OPL3_GenerateStream(chip, stereo, static_cast<std::uint32_t>(want));
    for (std::size_t i = 0; i < want; ++i) {
      const float l =
          static_cast<float>(stereo[i * 2]) / 32768.0f;
      const float r =
          static_cast<float>(stereo[i * 2 + 1]) / 32768.0f;
      out[done + i] = (l + r) * 0.5f;
    }
    done += want;
  }
}

void Opl3Device::render(float* buf, std::size_t frames) {
  if (buf == nullptr || frames == 0) return;
  if (!inited_ || main_chip_ == nullptr) {
    std::memset(buf, 0, frames * sizeof(float));
    return;
  }
  // Fast path: no mute/solo active -> single main-chip mix.
  bool any_gate = false;
  {
    DeviceSnapshot s = makeSnapshot();
    for (const ChannelState& c : s.channels) {
      if (c.muted || c.soloed) {
        any_gate = true;
        break;
      }
    }
  }
  if (!any_gate) {
    renderMonoFromChip(main_chip_, buf, frames);
  } else {
    // Gated path: sum audible shadow voices so muted/solo-excluded voices
    // contribute silence.
    std::memset(buf, 0, frames * sizeof(float));
    constexpr std::size_t kChunk = 256;
    float tmp[kChunk];
    for (int v = 0; v < kVoices; ++v) {
      if (isVoiceDormant(v)) continue;  // Fast escape.
      if (!channelAudible(v)) continue;
      if (!isShadowInitialized(v)) continue;
      std::size_t done = 0;
      while (done < frames) {
        const std::size_t want = std::min(kChunk, frames - done);
        renderMonoFromChip(&shadows_[static_cast<std::size_t>(v)], tmp, want);
        for (std::size_t i = 0; i < want; ++i) buf[done + i] += tmp[i];
        done += want;
      }
    }
    // Clamp the summed mix to [-1, 1].
    for (std::size_t i = 0; i < frames; ++i) {
      if (buf[i] > 1.0f)
        buf[i] = 1.0f;
      else if (buf[i] < -1.0f)
        buf[i] = -1.0f;
    }
  }
  // Scope tap + peak tracking for woken voices.
  float peak = 0.0f;
  for (std::size_t i = 0; i < frames; ++i) {
    const float a = std::fabs(buf[i]);
    if (a > peak) peak = a;
  }
  noteMasterPeak(peak);
}

void Opl3Device::renderPerChannel(float** bufs, std::size_t frames) {
  if (bufs == nullptr || frames == 0) return;
  if (!inited_ || shadows_ == nullptr) {
    for (int v = 0; v < kVoices; ++v) {
      if (bufs[v] != nullptr) std::memset(bufs[v], 0, frames * sizeof(float));
    }
    return;
  }
  for (int v = 0; v < kVoices; ++v) {
    float* out = bufs[v];
    if (out == nullptr) continue;
    // 1-cycle fast escape: dormant voices cost a bounds-checked load and a
    // memset — no shadow-chip stepping, no buffer updates beyond silence.
    if (isVoiceDormant(v) || !isShadowInitialized(v)) {
      std::memset(out, 0, frames * sizeof(float));
      continue;
    }
    if (!channelAudible(v)) {
      std::memset(out, 0, frames * sizeof(float));
      continue;
    }
    renderMonoFromChip(&shadows_[static_cast<std::size_t>(v)], out, frames);
    pushWaveform(v, out, frames);
  }
}

}  // namespace audio_dbg
