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
  // One stereo buffer per oscillator (0=Sq1, 1=Sq2, 2=Wave, 3=Noise), so a
  // SINGLE Gb_Apu yields both the mixed sum and each channel's isolated stem
  // (for scopes) and per-channel mute/solo by skipping that oscillator in the
  // sum -- mGBA's channel-disable mask, not a second chip. This matches
  // audition.py: one APU, one deterministic end_frame() per 60 Hz tick.
  Stereo_Buffer buf[4];
  blip_time_t time = 0;

  static constexpr blip_time_t kFrameLength = 70224;
  static constexpr int kBufferMsec = 500;
  static constexpr int kOscCount = 4;

  GbVoiceApu() {
    // Match Basic_Gb_Apu's "tiny speaker" equalization.
    apu.treble_eq(-20.0);
    for (int i = 0; i < kOscCount; ++i) buf[i].bass_freq(461);
  }

  blargg_err_t set_sample_rate(long rate) {
    for (int i = 0; i < kOscCount; ++i) {
      buf[i].clock_rate(4194304);
      const blargg_err_t err = buf[i].set_sample_rate(rate, kBufferMsec);
      if (err != nullptr) return err;
      apu.osc_output(i, buf[i].center(), buf[i].left(), buf[i].right());
    }
    return nullptr;
  }

  blip_time_t clock() { return time += 4; }

  void write_register(gb_addr_t addr, int data) {
    apu.write_register(clock(), addr, data);
  }

  int read_register(gb_addr_t addr) { return apu.read_register(clock(), addr); }

  void end_frame() {
    time = 0;
    const bool stereo = apu.end_frame(kFrameLength);
    for (int i = 0; i < kOscCount; ++i) {
      buf[i].end_frame(kFrameLength, stereo);
    }
  }

  long samples_avail(int ch) const { return buf[ch].samples_avail(); }

  long read_samples(int ch, blip_sample_t* out, long count) {
    return buf[ch].read_samples(out, count);
  }
};

// Default Wave RAM pattern from Pokemon Yellow (audio/wave_samples.asm).
namespace {
constexpr std::uint8_t kDefaultWave[16] = {
    0x02, 0x46, 0x8A, 0xCE, 0xFF, 0xFE, 0xED, 0xCC,
    0xBA, 0x98, 0x76, 0x54, 0x43, 0x32, 0x21, 0x11};

// Authentic noise parameters per pret instrument id: (initial volume 0-15,
// envelope fade period, NR43). Mirrors audition/opl_renderer.py DRUM_PARAMS
// (used by both the GB precache and the live GB path); the gb MIDI target
// carries raw instrument ids on channel 9 so this lookup is exact.
// Unknown ids fall back to (8, 1, 34), as audition does.
struct DrumParams {
  int vol;
  int fade;
  std::uint8_t nr43;
};
DrumParams drumParams(int instrument) {
  switch (instrument) {
    case 1: return {12, 1, 51};
    case 2: return {11, 1, 51};
    case 3: return {10, 1, 51};
    case 4: return {8, 1, 51};
    case 5: return {8, 4, 55};
    case 6: return {5, 1, 42};
    case 7: return {4, 1, 43};
    case 8: return {8, 1, 16};
    case 9: return {8, 2, 35};
    case 10: return {8, 2, 37};
    case 11: return {8, 2, 38};
    case 12: return {10, 1, 16};
    case 13: return {10, 2, 17};
    case 14: return {10, 2, 80};
    case 15: return {10, 1, 24};
    case 16: return {9, 1, 40};
    case 17: return {9, 1, 34};
    case 18: return {7, 1, 34};
    case 19: return {6, 1, 34};
    default: return {8, 1, 34};
  }
}
}  // namespace

GbApuDevice::GbApuDevice(int device_id, std::string device_name,
                         long sample_rate)
    : PsgDevice(device_id, std::move(device_name), kChannels),
      sample_rate_(sample_rate <= 0 ? kDefaultRate : sample_rate) {
  regs_.fill(0);
}

GbApuDevice::~GbApuDevice() {
  shutdown();
}

bool GbApuDevice::init() {
  if (inited_) return true;
  if (apu_ == nullptr) {
    apu_ = new (std::nothrow) GbVoiceApu();
    if (apu_ == nullptr) return false;
  }
  if (apu_->set_sample_rate(sample_rate_) != 0) return false;
  // Realtime render scratch, sized once on the UI thread: no allocation is
  // allowed inside render(). Guarded by `inited_`; resize() on this path
  // never touches a running callback.
  tmp_scratch_.assign(kScratchFrames, 0.0f);
  regs_.fill(0);
  // Init sequence (mirrors audition gb_synth.cpp).
  apu_->write_register(0xFF26, 0x80);
  regs_[0x26 - 0x10] = 0x80;
  apu_->write_register(0xFF24, 0x77);
  regs_[0x24 - 0x10] = 0x77;
  apu_->write_register(0xFF25, 0xFF);
  regs_[0x25 - 0x10] = 0xFF;
  for (int i = 0; i < 16; ++i) {
    apu_->write_register(static_cast<unsigned>(0xFF30 + i), kDefaultWave[i]);
    regs_[0x20 + i] = kDefaultWave[i];
  }
  inited_ = true;
  return true;
}

void GbApuDevice::shutdown() {
  if (apu_ != nullptr) {
    delete apu_;
    apu_ = nullptr;
  }
  inited_ = false;
}

void GbApuDevice::reset() {
  if (!inited_ || apu_ == nullptr) return;
  // Toggle NR52 + restore globals + wave RAM (mirrors gb_synth reset).
  apu_->write_register(0xFF26, 0x00);
  apu_->write_register(0xFF26, 0x80);
  apu_->write_register(0xFF24, 0x77);
  apu_->write_register(0xFF25, 0xFF);
  for (int i = 0; i < 16; ++i) {
    apu_->write_register(static_cast<unsigned>(0xFF30 + i), kDefaultWave[i]);
  }
  regs_.fill(0);
  regs_[0x26 - 0x10] = 0x80;
  regs_[0x24 - 0x10] = 0x77;
  regs_[0x25 - 0x10] = 0xFF;
  for (int i = 0; i < 16; ++i) regs_[0x20 + i] = kDefaultWave[i];
  for (int i = 0; i < kChannels; ++i) {
    if (channelValid(i)) {
      channel(i).active = false;
      channel(i).freq = 0.0f;
      channel(i).last_note = -1;
      channel(i).dormant = true;
      channel(i).peak = 0.0f;
    }
  }
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

bool GbApuDevice::isShadowInitialized(int ch) const {
  // Single-APU model: there is no shadow chip. "Initialized" now means the
  // channel has been woken (written) since init/reset -- the same sticky
  // state the old shadow lazy-init tracked.
  if (ch < 0 || ch >= kChannels) return false;
  return !isDormant(ch);
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
  if (!inited_ || apu_ == nullptr) return;
  if (addr < kRegBase || addr > kRegEnd) return;
  regs_[addr - 0xFF10] = val;
  apu_->write_register(addr, val);
  // Decode into the PsgDevice base for the UI (duty/frequency/volume/active/
  // dormancy). The single Gb_Apu already got the write above; decode only
  // updates the decoded state -- there is no register re-push (audition
  // writes each register exactly once).
  if (!isGlobalAddr(addr)) {
    const int ch = channelForAddr(addr);
    if (ch >= 0) decodeAndTrack(addr, val);
  }
}

std::uint8_t GbApuDevice::readRegister(std::uint16_t addr) const {
  if (addr < kRegBase || addr > kRegEnd) return 0xFF;
  return regs_[addr - 0xFF10];
}

void GbApuDevice::applyRegister(int ch) {
  // Single-APU model: writeRegister() already delivered the write to the
  // Gb_Apu. The base setters (setFrequency/setDuty/...) only update the
  // decoded UI state, so there is nothing to re-push here. (The old shadow
  // fan-out re-triggered NR14 with stale/mixed registers -- a genuine
  // misapplication deleted with the shadow layer.)
  (void)ch;
}

void GbApuDevice::handleCommand(std::uint8_t opcode,
                                const std::uint8_t* payload, std::size_t len) {
  if (payload == nullptr || len == 0 || !inited_) return;
  const int raw_ch = static_cast<int>(payload[0]);
  int ch = 0;
  if (raw_ch == 9) ch = 3;       // MIDI drums (ch 9) -> GB Noise (ch 3)
  else if (raw_ch == 3) ch = 2;  // MIDI ch 3 -> GB Wave (ch 2)
  else if (raw_ch == 2) ch = 1;  // MIDI ch 2 -> GB Pulse 2 (ch 1)
  else if (raw_ch == 1 || raw_ch == 0) ch = 0; // MIDI ch 1 / 0 -> GB Pulse 1 (ch 0)
  else return;  // MIDI ch 4-8 are the MT-32 enhancement tracks (sub_bass,
                // low_pad, sparkle): not GB voices. audition.py drives only the
                // 4 pret GB channels; the old `% kChannels` fold retriggered
                // the melody voice at bass pitch -- the "misses notes + off
                // key" bug. Drop them.
  wakeChannel(ch);

  if (opcode == 0x90 && len >= 3) {
    const int note = static_cast<int>(payload[1]);
    const int vel = static_cast<int>(payload[2]);
    if (vel == 0) {
      if (ch == 0) {
        writeRegister(0xFF12, 0x00);
      } else if (ch == 1) {
        writeRegister(0xFF17, 0x00);
      } else if (ch == 2) {
        writeRegister(0xFF1C, 0x00);
      } else if (ch == 3) {
        // Mirrors audition's silence_channel(3): zero volume + retrigger so
        // the voice stops immediately.
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
      // Raw pret noise instrument id (gb MIDI target). Mirrors audition's
      // trigger_noise via DRUM_PARAMS: per-instrument volume/envelope/NR43
      // (including 7-bit width codes the old GM pitch-match search could
      // never produce). Envelope fade direction is always decay, as there.
      const DrumParams p = drumParams(note);
      writeRegister(0xFF21, static_cast<std::uint8_t>((p.vol << 4) | p.fade));
      writeRegister(0xFF22, p.nr43);
      writeRegister(0xFF23, 0x80);
    }
  } else if (opcode == 0x80) {
    if (ch == 0) {
      writeRegister(0xFF12, 0x00);
    } else if (ch == 1) {
      writeRegister(0xFF17, 0x00);
    } else if (ch == 2) {
      writeRegister(0xFF1C, 0x00);
    } else if (ch == 3) {
      // Mirrors audition's silence_channel(3).
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

void GbApuDevice::refreshAudibleCache() const {
  const int n = channelCount();
  bool any = false;
  for (int i = 0; i < kChannels; ++i) {
    if (i >= n) {
      gate_muted_[static_cast<std::size_t>(i)] = true;
      gate_soloed_[static_cast<std::size_t>(i)] = false;
      continue;
    }
    const ChannelState& st = channel(static_cast<int>(i));
    gate_muted_[static_cast<std::size_t>(i)] = st.muted;
    gate_soloed_[static_cast<std::size_t>(i)] = st.soloed;
    if (st.soloed) any = true;
  }
  // Non-atomic publication: the audio thread consumes a write that is at
  // worst one snapshot stale (the previous UI snapshot), and an int/bool
  // write followed by the UI loop's own read-back is torn-read safe on x86.
  any_solo_ = any;
}

bool GbApuDevice::channelAudible(int ch) const {
  if (ch < 0 || ch >= channelCount()) return false;
  if (gate_muted_[static_cast<std::size_t>(ch)]) return false;
  if (any_solo_ && !gate_soloed_[static_cast<std::size_t>(ch)]) return false;
  return true;
}

bool GbApuDevice::hasMutedChannel() const {
  const int n = channelCount();
  for (int i = 0; i < kChannels && i < n; ++i) {
    if (gate_muted_[static_cast<std::size_t>(i)]) return true;
  }
  return false;
}

std::size_t GbApuDevice::readStem(int ch, float* out, std::size_t frames,
                                  bool accumulate) {
  if (out == nullptr || frames == 0 || apu_ == nullptr) return 0;
  constexpr float kGbLevelScale = 0.55f;
  std::size_t done = 0;
  blip_sample_t tmp[512];
  while (done < frames) {
    long avail = apu_->samples_avail(ch);
    if (avail < 2) {
      // Lazy frame synthesis: the single APU produces one ~803.6-sample frame
      // when its buffers run dry. All four oscillators advance in lockstep, so
      // whichever channel drains first triggers the shared end_frame().
      apu_->end_frame();
      avail = apu_->samples_avail(ch);
      if (avail < 2) break;
    }
    const std::size_t needed_stereo = (frames - done) * 2;
    const long to_read = static_cast<long>(
        std::min(static_cast<std::size_t>(avail), needed_stereo)) & ~1L;
    if (to_read <= 0) break;
    const long chunk = std::min(to_read, 512L);
    const long got = apu_->read_samples(ch, tmp, chunk);
    if (got <= 0) break;
    const std::size_t mono = static_cast<std::size_t>(got) / 2;
    for (std::size_t i = 0; i < mono; ++i) {
      const float l = static_cast<float>(tmp[i * 2]) / 32768.0f;
      const float r = static_cast<float>(tmp[i * 2 + 1]) / 32768.0f;
      const float v = (l + r) * 0.5f * kGbLevelScale;
      if (accumulate) out[done + i] += v; else out[done + i] = v;
    }
    done += mono;
  }
  return done;
}

void GbApuDevice::render(float* buf, std::size_t frames) {
  if (buf == nullptr || frames == 0) return;
  if (!inited_ || apu_ == nullptr) {
    std::memset(buf, 0, frames * sizeof(float));
    return;
  }
  std::memset(buf, 0, frames * sizeof(float));
  // Sum each audible oscillator stem. The mute/solo gate is the lock-free
  // per-channel cache (channelAudible). Awake-but-not-audible channels are
  // still drained (into scratch) so their buffer never accumulates stale
  // audio, and each awake stem is tapped into the scope ring.
  const std::size_t n = std::min(frames, tmp_scratch_.size());
  for (int c = 0; c < kChannels; ++c) {
    if (isDormant(c)) continue;  // Fast escape: untouched oscillator.
    float* tmp = tmp_scratch_.data();
    const std::size_t got = readStem(c, tmp, n, /*accumulate=*/false);
    if (channelAudible(c)) {
      for (std::size_t i = 0; i < got; ++i) buf[i] += tmp[i];
      pushWaveform(c, tmp, got);  // Audio-thread scope tap.
    }
  }
  float peak = 0.0f;
  for (std::size_t i = 0; i < frames; ++i) {
    if (buf[i] > 1.0f)
      buf[i] = 1.0f;
    else if (buf[i] < -1.0f)
      buf[i] = -1.0f;
    const float a = std::fabs(buf[i]);
    if (a > peak) peak = a;
  }
  noteMasterPeak(peak);
}

void GbApuDevice::setMute(int ch, bool muted) {
  PsgDevice::setMute(ch, muted);
  refreshAudibleCache();
}

void GbApuDevice::setSolo(int ch, bool soloed) {
  PsgDevice::setSolo(ch, soloed);
  refreshAudibleCache();
}

DeviceSnapshot GbApuDevice::snapshot() const {
  refreshAudibleCache();
  return makeSnapshot();
}

void GbApuDevice::renderPerChannel(float** bufs, std::size_t frames) {
  if (bufs == nullptr || frames == 0) return;
  for (int c = 0; c < kChannels; ++c) {
    float* out = bufs[c];
    if (out == nullptr) continue;
    // Dormant, not-inited, or muted/solo-excluded channels are silence.
    if (!inited_ || apu_ == nullptr || isDormant(c) || !channelAudible(c)) {
      std::memset(out, 0, frames * sizeof(float));
      continue;
    }
    const std::size_t got = readStem(c, out, frames, /*accumulate=*/false);
    if (got < frames) {
      std::memset(out + got, 0, (frames - got) * sizeof(float));
    }
    // No scope push here: renderPerChannel runs on the stems/record thread.
    // Scopes are fed exclusively by render() on the audio thread (W6), which
    // keeps the SoundDevice ring single-producer/lock-free.
  }
}

}  // namespace audio_dbg
