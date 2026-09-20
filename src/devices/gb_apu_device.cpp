// Stage 4.2: GbApuDevice implementation.
// See gb_apu_device.h for the contract.

#include "devices/gb_apu_device.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "gb_apu/Gb_Apu.h"
#include "gb_apu/Multi_Buffer.h"

namespace audio_dbg {

// GbVoiceApu: method-for-method replica of Basic_Gb_Apu (Gb_Snd_Emu) with an
// explicit Blip_Buffer length. Basic_Gb_Apu::set_sample_rate(rate) takes the
// `blip_default_length` path whose 32-bit-era size expression
// `(ULONG_MAX >> 16) + 1 - ...` underflows to ~4.3 G samples (~16 GB memset)
// on 64-bit — measured 33 s for the 5 instances in the synth tests. The
// replica issues the identical Gb_Apu + Stereo_Buffer call sequence (same
// treble/bass EQ, same fake-CPU clock, same 70224-clock frame), so its PCM
// is bit-identical to Basic_Gb_Apu while allocating ~50 KB per buffer.
struct GbVoiceApu {
  Gb_Apu apu;
  Stereo_Buffer buf;
  blip_time_t time = 0;

  static constexpr blip_time_t kFrameLength = 70224;
  static constexpr int kBufferMsec = 500;

  GbVoiceApu() {
    // Match Basic_Gb_Apu's "tiny speaker" equalization.
    apu.treble_eq(-20.0);
    buf.bass_freq(461);
  }

  blargg_err_t set_sample_rate(long rate) {
    apu.output(buf.center(), buf.left(), buf.right());
    buf.clock_rate(4194304);
    return buf.set_sample_rate(rate, kBufferMsec);
  }

  blip_time_t clock() { return time += 4; }

  void write_register(gb_addr_t addr, int data) {
    apu.write_register(clock(), addr, data);
  }

  int read_register(gb_addr_t addr) { return apu.read_register(clock(), addr); }

  void end_frame() {
    time = 0;
    const bool stereo = apu.end_frame(kFrameLength);
    buf.end_frame(kFrameLength, stereo);
  }

  long samples_avail() const { return buf.samples_avail(); }

  long read_samples(blip_sample_t* out, long count) {
    return buf.read_samples(out, count);
  }
};

// Default Wave RAM pattern from Pokemon Yellow (audio/wave_samples.asm).
namespace {
constexpr std::uint8_t kDefaultWave[16] = {
    0x02, 0x46, 0x8A, 0xCE, 0xFF, 0xFE, 0xED, 0xCC,
    0xBA, 0x98, 0x76, 0x54, 0x43, 0x32, 0x21, 0x11};
}  // namespace

GbApuDevice::GbApuDevice(int device_id, std::string device_name,
                         long sample_rate)
    : PsgDevice(device_id, std::move(device_name), kChannels),
      sample_rate_(sample_rate <= 0 ? kDefaultRate : sample_rate) {
  regs_.fill(0);
  shadow_init_.fill(false);
}

GbApuDevice::~GbApuDevice() {
  shutdown();
}

bool GbApuDevice::init() {
  if (inited_) return true;
  if (master_ == nullptr) {
    master_ = new (std::nothrow) GbVoiceApu();
    if (master_ == nullptr) return false;
  }
  if (master_->set_sample_rate(sample_rate_) != 0) return false;
  regs_.fill(0);
  shadow_init_.fill(false);
  for (int i = 0; i < kChannels; ++i) {
    if (shadows_[i] != nullptr) {
      delete shadows_[i];
      shadows_[i] = nullptr;
    }
  }
  // Master init sequence (mirrors audition gb_synth.cpp).
  master_->write_register(0xFF26, 0x80);
  regs_[0x26 - 0x10] = 0x80;
  master_->write_register(0xFF24, 0x77);
  regs_[0x24 - 0x10] = 0x77;
  master_->write_register(0xFF25, 0xFF);
  regs_[0x25 - 0x10] = 0xFF;
  for (int i = 0; i < 16; ++i) {
    master_->write_register(static_cast<unsigned>(0xFF30 + i), kDefaultWave[i]);
    regs_[0x20 + i] = kDefaultWave[i];
  }
  inited_ = true;
  return true;
}

void GbApuDevice::shutdown() {
  if (master_ != nullptr) {
    delete master_;
    master_ = nullptr;
  }
  for (int i = 0; i < kChannels; ++i) {
    if (shadows_[i] != nullptr) {
      delete shadows_[i];
      shadows_[i] = nullptr;
    }
  }
  shadow_init_.fill(false);
  inited_ = false;
}

void GbApuDevice::reset() {
  if (!inited_ || master_ == nullptr) return;
  // Toggle NR52 + restore globals + wave RAM (mirrors gb_synth reset).
  master_->write_register(0xFF26, 0x00);
  master_->write_register(0xFF26, 0x80);
  master_->write_register(0xFF24, 0x77);
  master_->write_register(0xFF25, 0xFF);
  for (int i = 0; i < 16; ++i) {
    master_->write_register(static_cast<unsigned>(0xFF30 + i), kDefaultWave[i]);
  }
  regs_.fill(0);
  regs_[0x26 - 0x10] = 0x80;
  regs_[0x24 - 0x10] = 0x77;
  regs_[0x25 - 0x10] = 0xFF;
  for (int i = 0; i < 16; ++i) regs_[0x20 + i] = kDefaultWave[i];
  // Drop shadows: they re-lazy-init on next write (keeps reset cheap and
  // preserves the dormant invariant for never-touched channels).
  for (int i = 0; i < kChannels; ++i) {
    if (shadows_[i] != nullptr) {
      delete shadows_[i];
      shadows_[i] = nullptr;
    }
    if (channelValid(i)) {
      channel(i).active = false;
      channel(i).freq = 0.0f;
      channel(i).last_note = -1;
      channel(i).dormant = true;
      channel(i).peak = 0.0f;
    }
  }
  shadow_init_.fill(false);
}

int GbApuDevice::channelForAddr(std::uint16_t addr) {
  if (addr >= 0xFF10 && addr <= 0xFF14) return 0;
  if (addr >= 0xFF16 && addr <= 0xFF19) return 1;
  if (addr >= 0xFF1A && addr <= 0xFF1E) return 2;
  if (addr >= 0xFF20 && addr <= 0xFF23) return 3;
  if (addr >= 0xFF30 && addr <= 0xFF3F) return 2;
  return -1;
}

bool GbApuDevice::isGlobalAddr(std::uint16_t addr) {
  return addr == 0xFF24 || addr == 0xFF25 || addr == 0xFF26;
}

void GbApuDevice::pushToApu(GbVoiceApu* apu, std::uint16_t addr,
                            std::uint8_t val) {
  if (apu == nullptr) return;
  apu->write_register(addr, val);
}

void GbApuDevice::ensureShadow(int ch) {
  if (ch < 0 || ch >= kChannels) return;
  if (shadow_init_[static_cast<std::size_t>(ch)]) return;
  if (!inited_) return;
  if (shadows_[ch] == nullptr) {
    shadows_[ch] = new (std::nothrow) GbVoiceApu();
    if (shadows_[ch] == nullptr) return;
    if (shadows_[ch]->set_sample_rate(sample_rate_) != 0) {
      delete shadows_[ch];
      shadows_[ch] = nullptr;
      return;
    }
  }
  GbVoiceApu* sh = shadows_[ch];
  // Replay globals + wave RAM + this channel's registers.
  sh->write_register(0xFF26, regs_[0x26 - 0x10] | 0x80);
  sh->write_register(0xFF24, regs_[0x24 - 0x10]);
  sh->write_register(0xFF25, regs_[0x25 - 0x10]);
  for (int i = 0; i < 16; ++i) {
    sh->write_register(static_cast<unsigned>(0xFF30 + i),
                       regs_[0x20 + i]);
  }
  const std::uint16_t addrs[4][5] = {
      {0xFF10, 0xFF11, 0xFF12, 0xFF13, 0xFF14},
      {0xFF16, 0xFF16, 0xFF17, 0xFF18, 0xFF19},
      {0xFF1A, 0xFF1B, 0xFF1C, 0xFF1D, 0xFF1E},
      {0xFF20, 0xFF21, 0xFF22, 0xFF23, 0xFFFF},
  };
  // ch1 has no sweep reg; slot [1] duplicates NR21 for the replay loop.
  for (int i = 0; i < 5; ++i) {
    const std::uint16_t a = addrs[ch][i];
    if (a == 0xFFFF) break;
    if (ch == 1 && i == 0) continue;  // No FF15; skip the placeholder.
    sh->write_register(a, regs_[a - 0xFF10]);
  }
  shadow_init_[static_cast<std::size_t>(ch)] = true;
}

bool GbApuDevice::isShadowInitialized(int ch) const {
  if (ch < 0 || ch >= kChannels) return false;
  return shadow_init_[static_cast<std::size_t>(ch)];
}

double GbApuDevice::pulseFreqHz(int freq_val) {
  const int denom = 2048 - (freq_val & 0x7FF);
  if (denom <= 0) return 0.0;
  return 131072.0 / static_cast<double>(denom);
}

double GbApuDevice::waveFreqHz(int freq_val) {
  const int denom = 2048 - (freq_val & 0x7FF);
  if (denom <= 0) return 0.0;
  return 65536.0 / static_cast<double>(denom);
}

double GbApuDevice::noiseFreqHz(std::uint8_t nr43) {
  const int r = nr43 & 0x07;
  const int s = (nr43 >> 4) & 0x0F;
  const int divisor = (r == 0) ? 8 : r * 16;
  const long period = static_cast<long>(divisor) << s;
  if (period <= 0) return 0.0;
  return 4194304.0 / static_cast<double>(period);
}

void GbApuDevice::decodePulse(int ch, std::uint16_t addr) {
  // ch 0: FF10-FF14; ch 1: FF16-FF19 (FF15 gap).
  const std::uint16_t lo_addr = (ch == 0) ? 0xFF13 : 0xFF18;
  const std::uint16_t hi_addr = (ch == 0) ? 0xFF14 : 0xFF19;
  const std::uint16_t duty_addr = (ch == 0) ? 0xFF11 : 0xFF16;
  const std::uint16_t env_addr = (ch == 0) ? 0xFF12 : 0xFF17;
  if (addr == duty_addr) {
    setDuty(ch, (regs_[duty_addr - 0xFF10] >> 6) & 0x03);
  } else if (addr == env_addr) {
    const std::uint8_t v = regs_[env_addr - 0xFF10];
    setVolume(ch, (v >> 4) & 0x0F);
    setEnvelope(ch, v & 0x07);
  } else if (addr == lo_addr || addr == hi_addr) {
    const int lo = regs_[lo_addr - 0xFF10];
    const int hi = regs_[hi_addr - 0xFF10];
    const int freq_val = ((hi & 0x07) << 8) | lo;
    setFrequency(ch, pulseFreqHz(freq_val));
    if (hi & 0x80) {
      activateChannel(ch);
    }
  } else if (addr == 0xFF10 && ch == 0) {
    // Sweep: no dedicated base field; wake the strip so the tab shows it.
    setChannelType(0, PsgChannelType::PULSE1);
  }
}

void GbApuDevice::decodeWave(std::uint16_t addr) {
  if (addr == 0xFF1A) {
    const std::uint8_t v = regs_[0xFF1A - 0xFF10];
    if ((v & 0x80) == 0) {
      deactivateChannel(2);
    } else {
      setChannelType(2, PsgChannelType::WAVE);
    }
  } else if (addr == 0xFF1B) {
    setChannelType(2, PsgChannelType::WAVE);  // Length: wake only.
  } else if (addr == 0xFF1C) {
    const int code = (regs_[0xFF1C - 0xFF10] >> 5) & 0x03;
    int vol = 0;
    switch (code) {
      case 0:
        vol = 0;
        break;
      case 1:
        vol = 15;
        break;
      case 2:
        vol = 8;
        break;
      default:
        vol = 4;
        break;
    }
    setVolume(2, vol);
  } else if (addr == 0xFF1D || addr == 0xFF1E) {
    const int lo = regs_[0xFF1D - 0xFF10];
    const int hi = regs_[0xFF1E - 0xFF10];
    const int freq_val = ((hi & 0x07) << 8) | lo;
    setFrequency(2, waveFreqHz(freq_val));
    if (hi & 0x80) {
      activateChannel(2);
    }
  } else if (addr >= 0xFF30 && addr <= 0xFF3F) {
    std::array<std::uint8_t, 32> ram{};
    for (int i = 0; i < 16; ++i) {
      const std::uint8_t b = regs_[0x20 + i];
      ram[static_cast<std::size_t>(i * 2)] =
          static_cast<std::uint8_t>((b >> 4) & 0x0F);
      ram[static_cast<std::size_t>(i * 2 + 1)] =
          static_cast<std::uint8_t>(b & 0x0F);
    }
    setWaveRam(2, ram);
  }
}

void GbApuDevice::decodeNoise(std::uint16_t addr) {
  if (addr == 0xFF20) {
    setChannelType(3, PsgChannelType::NOISE);  // Length: wake only.
  } else if (addr == 0xFF21) {
    const std::uint8_t v = regs_[0xFF21 - 0xFF10];
    setVolume(3, (v >> 4) & 0x0F);
    setEnvelope(3, v & 0x07);
  } else if (addr == 0xFF22) {
    const std::uint8_t v = regs_[0xFF22 - 0xFF10];
    setLfsrWidth(3, (v & 0x08) ? 7 : 15);
    setFrequency(3, noiseFreqHz(v));
  } else if (addr == 0xFF23) {
    const std::uint8_t v = regs_[0xFF23 - 0xFF10];
    // Frequency tracks NR43; trigger activates.
    setFrequency(3, noiseFreqHz(regs_[0xFF22 - 0xFF10]));
    if (v & 0x80) {
      activateChannel(3);
    }
  }
}

void GbApuDevice::decodeAndTrack(std::uint16_t addr, std::uint8_t val) {
  (void)val;
  const int ch = channelForAddr(addr);
  if (ch == 0) {
    decodePulse(0, addr);
  } else if (ch == 1) {
    if (addr == 0xFF15) {
      setChannelType(1, PsgChannelType::PULSE2);  // Unused: wake only.
    } else {
      decodePulse(1, addr);
    }
  } else if (ch == 2) {
    decodeWave(addr);
  } else if (ch == 3) {
    decodeNoise(addr);
  }
}

void GbApuDevice::writeRegister(std::uint16_t addr, std::uint8_t val) {
  if (!inited_ || master_ == nullptr) return;
  if (addr < kRegBase || addr > kRegEnd) return;
  regs_[addr - 0xFF10] = val;
  if (isGlobalAddr(addr)) {
    pushToApu(master_, addr, val);
    for (int c = 0; c < kChannels; ++c) {
      if (shadow_init_[static_cast<std::size_t>(c)] && shadows_[c] != nullptr) {
        pushToApu(shadows_[c], addr, val);
      }
    }
    return;
  }
  const int ch = channelForAddr(addr);
  if (ch < 0) return;  // Unused gap (FF15/FF1F/FF27-FF2F): shadow only.
  ensureShadow(ch);
  pushToApu(master_, addr, val);
  if (shadow_init_[static_cast<std::size_t>(ch)] && shadows_[ch] != nullptr) {
    pushToApu(shadows_[ch], addr, val);
  }
  // Decode into the PsgDevice base (wakes the channel; setters re-push the
  // same registers through applyRegister — benign duplicates at control
  // rate that keep the single chip-write path in the hook).
  decodeAndTrack(addr, val);
}

std::uint8_t GbApuDevice::readRegister(std::uint16_t addr) const {
  if (addr < kRegBase || addr > kRegEnd) return 0xFF;
  return regs_[addr - 0xFF10];
}

void GbApuDevice::applyRegister(int ch) {
  if (ch < 0 || ch >= kChannels || !inited_ || master_ == nullptr) return;
  ensureShadow(ch);
  // Push this channel's full register set from the shadow matrix so direct
  // base-setter calls (setDuty/setVolume/...) land on both chips.
  const std::uint16_t sets[4][6] = {
      {0xFF10, 0xFF11, 0xFF12, 0xFF13, 0xFF14, 0xFFFF},
      {0xFF16, 0xFF17, 0xFF18, 0xFF19, 0xFFFF, 0xFFFF},
      {0xFF1A, 0xFF1B, 0xFF1C, 0xFF1D, 0xFF1E, 0xFFFF},
      {0xFF20, 0xFF21, 0xFF22, 0xFF23, 0xFFFF, 0xFFFF},
  };
  for (int i = 0; i < 6; ++i) {
    const std::uint16_t a = sets[ch][i];
    if (a == 0xFFFF) break;
    const std::uint8_t v = regs_[a - 0xFF10];
    pushToApu(master_, a, v);
    if (shadow_init_[static_cast<std::size_t>(ch)] && shadows_[ch] != nullptr) {
      pushToApu(shadows_[ch], a, v);
    }
  }
  if (ch == 2) {
    for (int i = 0; i < 16; ++i) {
      const std::uint16_t a = static_cast<std::uint16_t>(0xFF30 + i);
      const std::uint8_t v = regs_[0x20 + i];
      pushToApu(master_, a, v);
      if (shadow_init_[2] && shadows_[2] != nullptr) {
        pushToApu(shadows_[2], a, v);
      }
    }
  }
}

void GbApuDevice::handleCommand(std::uint8_t opcode,
                                const std::uint8_t* payload, std::size_t len) {
  if (payload == nullptr || len == 0 || !inited_) return;
  const int ch = static_cast<int>(payload[0]) % kChannels;
  wakeChannel(ch);

  if (opcode == 0x90 && len >= 3) {
    const int note = static_cast<int>(payload[1]);
    const int vel = static_cast<int>(payload[2]);
    if (vel == 0) {
      if (ch == 0) {
        writeRegister(0xFF12, 0x00);
        writeRegister(0xFF14, 0x80);
      } else if (ch == 1) {
        writeRegister(0xFF17, 0x00);
        writeRegister(0xFF19, 0x80);
      } else if (ch == 2) {
        writeRegister(0xFF1A, 0x00);
      } else if (ch == 3) {
        writeRegister(0xFF21, 0x00);
        writeRegister(0xFF23, 0x80);
      }
      channel(ch).active = false;
      return;
    }

    channel(ch).last_note = note;
    channel(ch).active = true;
    const double f = 440.0 * std::pow(2.0, (static_cast<double>(note) - 69.0) / 12.0);
    const int vol = std::clamp(vel * 15 / 127, 1, 15);

    if (ch == 0 || ch == 1) {
      int gb_freq = static_cast<int>(std::round(2048.0 - (131072.0 / f)));
      gb_freq = std::clamp(gb_freq, 0, 2047);
      const std::uint8_t lo = static_cast<std::uint8_t>(gb_freq & 0xFF);
      const std::uint8_t hi = static_cast<std::uint8_t>(0x80 | ((gb_freq >> 8) & 0x07));
      if (ch == 0) {
        writeRegister(0xFF11, 0x80);
        writeRegister(0xFF12, static_cast<std::uint8_t>(vol << 4));
        writeRegister(0xFF13, lo);
        writeRegister(0xFF14, hi);
      } else {
        writeRegister(0xFF16, 0x80);
        writeRegister(0xFF17, static_cast<std::uint8_t>(vol << 4));
        writeRegister(0xFF18, lo);
        writeRegister(0xFF19, hi);
      }
    } else if (ch == 2) {
      int gb_freq = static_cast<int>(std::round(2048.0 - (65536.0 / f)));
      gb_freq = std::clamp(gb_freq, 0, 2047);
      const std::uint8_t lo = static_cast<std::uint8_t>(gb_freq & 0xFF);
      const std::uint8_t hi = static_cast<std::uint8_t>(0x80 | ((gb_freq >> 8) & 0x07));
      writeRegister(0xFF1A, 0x80);
      writeRegister(0xFF1B, 0xFF);
      writeRegister(0xFF1C, 0x20);
      writeRegister(0xFF1D, lo);
      writeRegister(0xFF1E, hi);
    } else if (ch == 3) {
      std::uint8_t best_nr43 = 0;
      double min_diff = 1e9;
      for (int s = 0; s < 14; ++s) {
        for (int r = 0; r < 8; ++r) {
          const std::uint8_t code = static_cast<std::uint8_t>((s << 4) | r);
          const double nf = noiseFreqHz(code);
          const double diff = std::fabs(nf - f);
          if (diff < min_diff) {
            min_diff = diff;
            best_nr43 = code;
          }
        }
      }
      writeRegister(0xFF20, 0x00);
      writeRegister(0xFF21, static_cast<std::uint8_t>(vol << 4));
      writeRegister(0xFF22, best_nr43);
      writeRegister(0xFF23, 0x80);
    }
  } else if (opcode == 0x80) {
    if (ch == 0) {
      writeRegister(0xFF12, 0x00);
      writeRegister(0xFF14, 0x80);
    } else if (ch == 1) {
      writeRegister(0xFF17, 0x00);
      writeRegister(0xFF19, 0x80);
    } else if (ch == 2) {
      writeRegister(0xFF1A, 0x00);
    } else if (ch == 3) {
      writeRegister(0xFF21, 0x00);
      writeRegister(0xFF23, 0x80);
    }
    channel(ch).active = false;
  }
}

std::uint8_t GbApuDevice::sweep(int ch) const {
  if (ch != 0) return 0;
  return regs_[0xFF10 - 0xFF10];
}

std::uint8_t GbApuDevice::noiseDivisor() const {
  return regs_[0xFF22 - 0xFF10] & 0x07;
}

std::uint8_t GbApuDevice::noiseShift() const {
  return (regs_[0xFF22 - 0xFF10] >> 4) & 0x0F;
}

bool GbApuDevice::noiseSevenBit() const {
  return (regs_[0xFF22 - 0xFF10] & 0x08) != 0;
}

std::uint8_t GbApuDevice::waveVolumeCode() const {
  return (regs_[0xFF1C - 0xFF10] >> 5) & 0x03;
}

std::uint8_t GbApuDevice::masterVolume() const {
  return regs_[0xFF24 - 0xFF10];
}

std::uint8_t GbApuDevice::panning() const {
  return regs_[0xFF25 - 0xFF10];
}

std::uint8_t GbApuDevice::power() const {
  return regs_[0xFF26 - 0xFF10];
}

bool GbApuDevice::channelAudible(int ch) const {
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

std::size_t GbApuDevice::drainApu(GbVoiceApu* apu, float* out,
                                  std::size_t max_frames) {
  if (apu == nullptr || out == nullptr || max_frames == 0) return 0;
  std::size_t done = 0;
  // One end_frame yields ~sample_rate/60 mono frames; loop for large asks.
  while (done < max_frames) {
    apu->end_frame();
    long avail = apu->samples_avail();  // Total int16 stereo samples.
    if (avail <= 0) break;
    std::vector<blip_sample_t> tmp(static_cast<std::size_t>(avail));
    const long got = apu->read_samples(tmp.data(), avail);
    if (got <= 0) break;
    const std::size_t mono_avail = static_cast<std::size_t>(got) / 2;
    for (std::size_t i = 0; i < mono_avail && done < max_frames; ++i) {
      const float l =
          static_cast<float>(tmp[i * 2]) / 32768.0f;
      float r = l;
      if (i * 2 + 1 < static_cast<std::size_t>(got)) {
        r = static_cast<float>(tmp[i * 2 + 1]) / 32768.0f;
      }
      out[done++] = (l + r) * 0.5f;
    }
    // If the APU produced less than asked, loop for another frame.
    if (mono_avail == 0) break;
  }
  return done;
}

void GbApuDevice::render(float* buf, std::size_t frames) {
  if (buf == nullptr || frames == 0) return;
  if (!inited_ || master_ == nullptr) {
    std::memset(buf, 0, frames * sizeof(float));
    return;
  }
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
    const std::size_t got = drainApu(master_, buf, frames);
    if (got < frames) {
      std::memset(buf + got, 0, (frames - got) * sizeof(float));
    }
    // Keep shadows clocked when they are awake so a later stem render does
    // not observe a stale frame. Dormant shadows take the fast escape.
    for (int c = 0; c < kChannels; ++c) {
      if (isDormant(c) || !shadow_init_[static_cast<std::size_t>(c)]) continue;
      // Drain-and-discard keeps the shadow's Blip_Buffer from growing
      // without bound between master-only renders.
      float discard[64];
      drainApu(shadows_[c], discard, 64);
    }
  } else {
    std::memset(buf, 0, frames * sizeof(float));
    std::vector<float> tmp(frames);
    for (int c = 0; c < kChannels; ++c) {
      if (isDormant(c)) continue;  // Fast escape.
      if (!channelAudible(c)) continue;
      if (!shadow_init_[static_cast<std::size_t>(c)]) continue;
      const std::size_t got = drainApu(shadows_[c], tmp.data(), frames);
      for (std::size_t i = 0; i < got; ++i) buf[i] += tmp[i];
    }
    for (std::size_t i = 0; i < frames; ++i) {
      if (buf[i] > 1.0f)
        buf[i] = 1.0f;
      else if (buf[i] < -1.0f)
        buf[i] = -1.0f;
    }
    // Keep the master clocked so a later unmuted render stays continuous.
    float discard[64];
    drainApu(master_, discard, 64);
  }
  float peak = 0.0f;
  for (std::size_t i = 0; i < frames; ++i) {
    const float a = std::fabs(buf[i]);
    if (a > peak) peak = a;
  }
  noteMasterPeak(peak);
}

void GbApuDevice::renderPerChannel(float** bufs, std::size_t frames) {
  if (bufs == nullptr || frames == 0) return;
  if (!inited_) {
    for (int c = 0; c < kChannels; ++c) {
      if (bufs[c] != nullptr) std::memset(bufs[c], 0, frames * sizeof(float));
    }
    return;
  }
  for (int c = 0; c < kChannels; ++c) {
    float* out = bufs[c];
    if (out == nullptr) continue;
    // Fast escape: dormant channels cost a bounds-checked load + memset —
    // no shadow-APU stepping.
    if (isDormant(c) || !shadow_init_[static_cast<std::size_t>(c)]) {
      std::memset(out, 0, frames * sizeof(float));
      continue;
    }
    if (!channelAudible(c)) {
      std::memset(out, 0, frames * sizeof(float));
      // Still step the shadow so unmuting resumes in sync.
      float discard[16];
      drainApu(shadows_[c], discard, 16);
      continue;
    }
    const std::size_t got = drainApu(shadows_[c], out, frames);
    if (got < frames) {
      std::memset(out + got, 0, (frames - got) * sizeof(float));
    }
    pushWaveform(c, out, frames);
  }
}

}  // namespace audio_dbg
