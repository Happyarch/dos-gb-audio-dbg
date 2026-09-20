// Stage 4.3: Mt32Device implementation.
// See mt32_device.h for the contract.

#include "devices/mt32_device.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <mt32emu/mt32emu.h>

namespace audio_dbg {

namespace {

// Standard MT-32 memory-timbre bank order (128 display labels).
const char* const kMt32Timbres[128] = {
    "AcouPiano1", "AcouPiano2", "AcouPiano3", "ElecPiano1", "ElecPiano2",
    "ElecPiano3", "ElecPiano4", "Honkytonk", "Organ 1", "Organ 2", "Organ 3",
    "Organ 4", "PipeOrgan1", "PipeOrgan2", "Accordion",
    "Harpsi 1", "Harpsi 2", "Clavi 1", "Clavi 2", "Celesta 1",
    "SynMallet", "Glocken", "Music Box", "Vibes 1",
    "Marimba", "Xylophone", "TubularBel", "Santur", "OrganFlute", "TremFlute",
    "Church Org", "ReedOrgan", "FrenchAcc", "ItalAccord", "NylonStrGt",
    "SteelStrGt", "Jazz Gtr", "Clean Gtr", "Muted Gtr", "OverdrvGt",
    "DistortGt", "GtHarmonix", "AcouBass1", "ElecBass1",
    "ElecBass2", "SlapBass1", "SlapBass2", "Fretless 1",
    "Violin 1", "Cello 1", "Contrabass", "Harp 1",
    "Pizzicato", "Timpani", "Strings 1", "SlowStr",
    "SynStr1", "SynStr2", "SynStr3", "SynStr4", "OrchHit", "Trumpet 1",
    "Trombone1", "FrHorn 1",
    "Brass 1", "Brass 2", "SynBrass1", "SynBrass2",
    "SopranoSax", "Alto Sax", "Tenor Sax", "Bari Sax", "Oboe", "EnglHorn",
    "Bassoon", "Clarinet", "Piccolo", "Flute 1", "Flute 2", "Recorder",
    "Pan Pipes", "BottleBlw", "Shakuhachi", "Whistle 1", "Whistle 2",
    "Ocarina", "SquareLd1", "SquareLd2", "Saw Ld 1", "Saw Ld 2", "SynCalliope",
    "ChifferLd", "Charang", "Solo Vox", "5thSawWave", "Bass & Ld",
    "Fantasia", "Warm Pad", "Polysynth", "SpaceVoice", "BowedGlass",
    "Metal Pad", "Halo Pad", "Sweep Pad", "Ice Rain", "Soundtrack",
    "Crystal", "Atmosphere", "Brightness", "Goblin", "Echo Drops",
    "StarTheme", "Sitar", "Banjo", "Shamisen", "Koto", "Kalimba",
    "Bagpipe", "Fiddle", "Shanai", "TinkleBel", "Agogo", "SteelDrums",
    "Woodblock", "Taiko", "MelodTom1", "SynDrum", "RevCymbal",
};

constexpr double kTwoPi = 6.28318530717958647692;

}  // namespace

Mt32Device::Mt32Device(int device_id, std::string device_name,
                       std::uint32_t sample_rate)
    : MidiDevice(device_id, std::move(device_name), kChannels),
      sample_rate_(sample_rate == 0 ? kDefaultRate : sample_rate) {
  std::memset(lcd_.data(), ' ', kLcdChars);
  lcd_[kLcdChars] = '\0';
  const char* init_msg = "NO ROM - MOCK MODE";
  std::memcpy(lcd_.data(), init_msg, std::strlen(init_msg));
  for (auto& row : phases_) row.fill(0.0);
}

Mt32Device::~Mt32Device() { shutdown(); }

double Mt32Device::midiHz(int note) {
  return 440.0 * std::pow(2.0, (note - 69) / 12.0);
}

bool Mt32Device::synthAudible(int ch) const {
  if (!chInRange(ch) || isDormant(ch)) return false;
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

bool Mt32Device::resolveRomPair() {
  // MT32_ROM_DIR=none forces mock mode (headless CI / fallback tests).
  if (const char* force = std::getenv("MT32_ROM_DIR")) {
    if (std::strcmp(force, "none") == 0) return false;
  }
  static const char* const kCtrlNames[] = {"MT32_CONTROL.ROM",
                                           "CM32L_CONTROL.ROM",
                                           "mt32_control.rom",
                                           "cm32l_control.rom"};
  static const char* const kPcmNames[] = {"MT32_PCM.ROM", "CM32L_PCM.ROM",
                                          "mt32_pcm.rom", "cm32l_pcm.rom"};
  std::string dirs;
  if (const char* env = std::getenv("MT32_ROM_DIR")) dirs += env;
  dirs += ";roms;mt32-roms;.";
  if (const char* home = std::getenv("HOME")) {
    dirs += ";";
    dirs += home;
    dirs += "/.config/scummvm;";
    dirs += home;
    dirs += "/.config/munt";
  }
  dirs +=
      ";/usr/share/mt32-rom-data;/usr/share/86Box/roms/sound/mt32"
      ";/usr/share/86Box/roms/sound/mt32_new"
      ";/usr/share/86Box/roms/sound/cm32l"
      ";/usr/share/86Box/roms/sound/cm32ln;/mnt/sdb1/ROMs/MUNT";
  std::size_t pos = 0;
  while (pos <= dirs.size()) {
    const std::size_t sep = dirs.find(';', pos);
    const std::string dir =
        dirs.substr(pos, sep == std::string::npos ? sep : sep - pos);
    pos = (sep == std::string::npos) ? dirs.size() + 1 : sep + 1;
    if (dir.empty()) continue;
    for (const char* cn : kCtrlNames) {
      for (const char* pn : kPcmNames) {
        const std::string ctrl_path = dir + "/" + cn;
        const std::string pcm_path = dir + "/" + pn;
        MT32Emu::FileStream* cf = new MT32Emu::FileStream();
        if (!cf->open(ctrl_path.c_str())) {
          delete cf;
          continue;
        }
        MT32Emu::FileStream* pf = new MT32Emu::FileStream();
        if (!pf->open(pcm_path.c_str())) {
          delete cf;
          delete pf;
          continue;
        }
        const MT32Emu::ROMImage* ci =
            MT32Emu::ROMImage::makeROMImage(cf);
        const MT32Emu::ROMImage* pi =
            MT32Emu::ROMImage::makeROMImage(pf);
        if (ci == nullptr || pi == nullptr) {
          if (ci != nullptr) MT32Emu::ROMImage::freeROMImage(ci);
          if (pi != nullptr) MT32Emu::ROMImage::freeROMImage(pi);
          delete cf;
          delete pf;
          continue;
        }
        if (synth_ != nullptr && synth_->open(*ci, *pi)) {
          ctrl_file_ = cf;
          pcm_file_ = pf;
          ctrl_img_ = ci;
          pcm_img_ = pi;
          rom_dir_ = dir;
          synth_open_ = true;
          sample_rate_ = synth_->getStereoOutputSampleRate();
          return true;
        }
        MT32Emu::ROMImage::freeROMImage(ci);
        MT32Emu::ROMImage::freeROMImage(pi);
        delete cf;
        delete pf;
      }
    }
  }
  return false;
}

bool Mt32Device::init() {
  if (inited_) return true;
  if (synth_ == nullptr) synth_ = new MT32Emu::Synth();
  mock_ = !resolveRomPair();
  if (mock_) {
    std::memset(lcd_.data(), ' ', kLcdChars);
    const char* init_msg = "NO ROM - MOCK MODE";
    std::memcpy(lcd_.data(), init_msg, std::strlen(init_msg));
  } else {
    char buf[kLcdChars + 1];
    if (synth_->getDisplayState(buf)) {
      (void)buf;
    }
    std::memcpy(lcd_.data(), buf, kLcdChars);
    lcd_[kLcdChars] = '\0';
  }
  inited_ = true;
  return true;
}

void Mt32Device::shutdown() {
  if (synth_ != nullptr) {
    if (synth_open_) {
      synth_->close();
      synth_open_ = false;
    }
    delete synth_;
    synth_ = nullptr;
  }
  if (ctrl_img_ != nullptr) {
    MT32Emu::ROMImage::freeROMImage(ctrl_img_);
    ctrl_img_ = nullptr;
  }
  if (pcm_img_ != nullptr) {
    MT32Emu::ROMImage::freeROMImage(pcm_img_);
    pcm_img_ = nullptr;
  }
  delete ctrl_file_;
  ctrl_file_ = nullptr;
  delete pcm_file_;
  pcm_file_ = nullptr;
  rom_dir_.clear();
  inited_ = false;
  mock_ = true;
}

void Mt32Device::reset() {
  for (auto& row : phases_) row.fill(0.0);
  if (!inited_ || mock_ || synth_ == nullptr || !synth_open_) return;
  // All Sound Off + All Notes Off on every channel; base note tracking is
  // untouched (MidiDevice exposes no clear hook).
  for (int ch = 0; ch < kChannels; ++ch) {
    const std::uint32_t off =
        (0xB0u | static_cast<std::uint32_t>(ch)) | (120u << 8);
    const std::uint32_t notes_off =
        (0xB0u | static_cast<std::uint32_t>(ch)) | (123u << 8);
    synth_->playMsgNow(off);
    synth_->playMsgNow(notes_off);
  }
}

void Mt32Device::dispatchNoteOn(int ch, int note, int velocity) {
  if (!chInRange(ch) || note < 0 || note >= 128) return;
  channel(ch).freq = static_cast<float>(midiHz(note));
  channel(ch).last_note = note;
  if (!inited_ || mock_ || synth_ == nullptr || !synth_open_) return;
  if (velocity < 1) velocity = 1;
  if (velocity > 127) velocity = 127;
  const std::uint32_t msg =
      (0x90u | static_cast<std::uint32_t>(ch & 0x0F)) |
      (static_cast<std::uint32_t>(note) << 8) |
      (static_cast<std::uint32_t>(velocity) << 16);
  synth_->playMsgNow(msg);
}

void Mt32Device::dispatchNoteOff(int ch, int note) {
  if (!chInRange(ch) || note < 0 || note >= 128) return;
  if (activeNoteCount(ch) == 0) channel(ch).active = false;
  if (!inited_ || mock_ || synth_ == nullptr || !synth_open_) return;
  const std::uint32_t msg =
      (0x80u | static_cast<std::uint32_t>(ch & 0x0F)) |
      (static_cast<std::uint32_t>(note) << 8);
  synth_->playMsgNow(msg);
}

void Mt32Device::programChange(int ch, int program) {
  dispatchProgramChange(ch, program);
}

void Mt32Device::dispatchProgramChange(int ch, int program) {
  if (!chInRange(ch)) return;
  setProgram(ch, program);
  if (!inited_ || mock_ || synth_ == nullptr || !synth_open_) return;
  const int prog = MidiDevice::program(ch);
  const std::uint32_t msg =
      (0xC0u | static_cast<std::uint32_t>(ch & 0x0F)) |
      (static_cast<std::uint32_t>(prog) << 8);
  synth_->playMsgNow(msg);
}

void Mt32Device::dispatchControlChange(int ch, int cc, int value) {
  if (!chInRange(ch) || cc < 0 || cc > 127) return;
  setControlChange(ch, cc, value);
  if (!inited_ || mock_ || synth_ == nullptr || !synth_open_) return;
  const int val = controlChange(ch, cc);
  const std::uint32_t msg =
      (0xB0u | static_cast<std::uint32_t>(ch & 0x0F)) |
      (static_cast<std::uint32_t>(cc) << 8) |
      (static_cast<std::uint32_t>(val) << 16);
  synth_->playMsgNow(msg);
}

void Mt32Device::dispatchSysEx(const std::uint8_t* data, std::size_t len) {
  if (data == nullptr || len == 0) return;
  if (mock_) parseDisplaySysEx(data, len);
  if (!inited_ || mock_ || synth_ == nullptr || !synth_open_) return;
  synth_->playSysexNow(data, static_cast<MT32Emu::Bit32u>(len));
}

void Mt32Device::parseDisplaySysEx(const std::uint8_t* data,
                                   std::size_t len) {
  // Roland display write: F0 41 10 16 12 20 00 00 <20 ASCII> chk F7.
  if (len < 30) return;
  if (data[0] != 0xF0 || data[1] != 0x41 || data[2] != 0x10 ||
      data[3] != 0x16 || data[4] != 0x12 || data[5] != 0x20 ||
      data[6] != 0x00 || data[7] != 0x00) {
    return;
  }
  for (std::size_t i = 0; i < kLcdChars; ++i) {
    const std::uint8_t c = data[8 + i];
    lcd_[i] = (c >= 32 && c < 127) ? static_cast<char>(c) : ' ';
  }
  lcd_[kLcdChars] = '\0';
}

const char* Mt32Device::mt32TimbreName(int program) {
  if (program < 0) program = 0;
  if (program > 127) program = 127;
  return kMt32Timbres[static_cast<std::size_t>(program)];
}

std::string Mt32Device::resolveProgramName(int channel, int program) const {
  (void)channel;
  return std::string(mt32TimbreName(program));
}

int Mt32Device::activePartialCount() const {
  if (!inited_ || mock_ || synth_ == nullptr || !synth_open_) {
    // Mock: sounding notes capped at the 32-slot partial grid.
    int count = 0;
    for (int ch = 0; ch < kChannels; ++ch) {
      if (isDormant(ch)) continue;  // Fast escape.
      count += static_cast<int>(activeNoteCount(ch));
    }
    return count > kPartialSlots ? kPartialSlots : count;
  }
  const MT32Emu::Bit32u n = synth_->getPartialCount();
  std::vector<MT32Emu::PartialState> states(n);
  synth_->getPartialStates(states.data());
  int count = 0;
  for (MT32Emu::Bit32u i = 0; i < n; ++i) {
    if (states[i] != MT32Emu::PartialState_INACTIVE) ++count;
  }
  return count;
}

int Mt32Device::partialState(int slot) const {
  if (slot < 0 || slot >= kPartialSlots) return -1;
  if (!inited_ || mock_ || synth_ == nullptr || !synth_open_) {
    return slot < activePartialCount()
               ? static_cast<int>(MT32Emu::PartialState_SUSTAIN)
               : static_cast<int>(MT32Emu::PartialState_INACTIVE);
  }
  const MT32Emu::Bit32u n = synth_->getPartialCount();
  std::vector<MT32Emu::PartialState> states(n);
  synth_->getPartialStates(states.data());
  if (static_cast<MT32Emu::Bit32u>(slot) >= n) return -1;
  return static_cast<int>(states[static_cast<std::size_t>(slot)]);
}

bool Mt32Device::partActive(int part) const {
  if (part < 0 || part >= kParts) return false;
  if (!inited_ || mock_ || synth_ == nullptr || !synth_open_) {
    const int ch = (part == 8) ? kRhythmChannel : part + 1;
    return !isDormant(ch) && activeNoteCount(ch) > 0;
  }
  return ((synth_->getPartStates() >> part) & 1u) != 0;
}

std::string Mt32Device::lcdText() const {
  if (!inited_ || mock_ || synth_ == nullptr || !synth_open_) {
    return std::string(lcd_.data(), kLcdChars);
  }
  char buf[kLcdChars + 1];
  synth_->getDisplayState(buf);
  return std::string(buf);
}

void Mt32Device::renderMockMono(float* out, std::size_t frames,
                                int only_channel) {
  std::memset(out, 0, frames * sizeof(float));
  const double rate = static_cast<double>(sample_rate_);
  for (int ch = 0; ch < kChannels; ++ch) {
    if (only_channel >= 0 && ch != only_channel) continue;
    if (isDormant(ch)) continue;  // Fast escape: never woke, no voices.
    if (!synthAudible(ch)) continue;
    for (int note = 0; note < 128; ++note) {
      if (!isNoteSounding(ch, note)) continue;
      const double freq = midiHz(note);
      const double step = kTwoPi * freq / rate;
      const float gain =
          static_cast<float>(noteVelocity(ch, note)) / 127.0f * 0.15f;
      double& phase =
          phases_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(
              note)];
      for (std::size_t i = 0; i < frames; ++i) {
        out[i] += gain * static_cast<float>(std::sin(phase));
        phase += step;
        if (phase >= kTwoPi) phase -= kTwoPi;
      }
    }
  }
  for (std::size_t i = 0; i < frames; ++i) {
    if (out[i] > 1.0f)
      out[i] = 1.0f;
    else if (out[i] < -1.0f)
      out[i] = -1.0f;
  }
}

void Mt32Device::renderSynthMono(float* out, std::size_t frames) {
  constexpr std::size_t kChunk = 256;
  float stereo[kChunk * 2];
  std::size_t done = 0;
  while (done < frames) {
    const std::size_t want = std::min(kChunk, frames - done);
    synth_->render(stereo, static_cast<MT32Emu::Bit32u>(want));
    for (std::size_t i = 0; i < want; ++i) {
      out[done + i] = (stereo[i * 2] + stereo[i * 2 + 1]) * 0.5f;
    }
    done += want;
  }
  char buf[kLcdChars + 1];
  synth_->getDisplayState(buf);
  std::memcpy(lcd_.data(), buf, kLcdChars);
  lcd_[kLcdChars] = '\0';
}

void Mt32Device::muteOthersForStem(int solo_ch) {
  for (int ch = 0; ch < kChannels; ++ch) {
    const int vol = (ch == solo_ch) ? controlChange(ch, 7) : 0;
    const std::uint32_t msg =
        (0xB0u | static_cast<std::uint32_t>(ch & 0x0F)) | (7u << 8) |
        (static_cast<std::uint32_t>(vol) << 16);
    synth_->playMsgNow(msg);
  }
}

void Mt32Device::restoreVolumes() {
  // Converge the engine to the live mute matrix: audible channels get
  // their tracked CC7, muted/solo-excluded channels stay at 0. Without
  // this, a restore-to-base would re-arm full volume between renders and
  // voices allocated on a muted channel would be born loud (the MT-32
  // captures part volume at note start).
  for (int ch = 0; ch < kChannels; ++ch) {
    const int vol = synthAudible(ch) ? controlChange(ch, 7) : 0;
    const std::uint32_t msg =
        (0xB0u | static_cast<std::uint32_t>(ch & 0x0F)) | (7u << 8) |
        (static_cast<std::uint32_t>(vol) << 16);
    synth_->playMsgNow(msg);
  }
}

void Mt32Device::render(float* buf, std::size_t frames) {
  if (buf == nullptr || frames == 0) return;
  if (!inited_) {
    std::memset(buf, 0, frames * sizeof(float));
    return;
  }
  if (mock_) {
    renderMockMono(buf, frames, -1);
    return;
  }
  // Converge engine volumes to the mute matrix BEFORE the idle fast
  // escape: a skipped block must still flush pending mute/unmute/CC
  // changes, otherwise voices allocated afterwards are born under a stale
  // matrix (the MT-32 captures part volume at note start).
  restoreVolumes();
  if (!synth_->isActive()) {
    // Fast escape: no partials, no queued events, no reverb tail.
    std::memset(buf, 0, frames * sizeof(float));
    return;
  }
  // Single main mix; engine volumes already match the matrix, so muted/
  // solo-excluded channels contribute silence (newly triggered voices —
  // the MT-32 captures volume at note start) while held voices decay.
  renderSynthMono(buf, frames);
}

void Mt32Device::renderPerChannel(float** bufs, std::size_t frames) {
  if (bufs == nullptr || frames == 0) return;
  for (int ch = 0; ch < kChannels; ++ch) {
    if (bufs[ch] == nullptr) continue;
    if (isDormant(ch)) {
      // 1-cycle fast escape: never woke, nothing to render.
      std::memset(bufs[ch], 0, frames * sizeof(float));
      continue;
    }
    if (!synthAudible(ch)) {
      std::memset(bufs[ch], 0, frames * sizeof(float));
      continue;
    }
    if (!inited_ || mock_) {
      renderMockMono(bufs[ch], frames, ch);
      continue;
    }
    // Real synth: isolate this channel with the CC7 mute-others dance.
    muteOthersForStem(ch);
    renderSynthMono(bufs[ch], frames);
    restoreVolumes();
  }
}

}  // namespace audio_dbg
