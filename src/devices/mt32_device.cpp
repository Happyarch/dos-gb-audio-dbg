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

// The shared runtime Python bridge (sysex_bridge, header-inline) plus the
// SysExMessages/SongTimbreSysex types. Included from the .cpp only: the
// mechanics are header-inline, so device-only test binaries that do not
// compile enhancement_manager.cpp still link.
#include "enhancement_manager.h"

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
    // Realtime-safe SysEx storage for the MIDI event queue: the UI/engine
    // thread enqueues SysEx while the audio thread renders. The default
    // storage allocates/frees per event and is explicitly not realtime-safe.
    synth_->configureMIDIEventQueueSysexStorage(kSysexQueueStorage);
    // Seed the telemetry shadow so the UI has valid data before the first
    // audio block (init runs before the mixer is attached to this device).
    updateTelemetry();
  }
  inited_ = true;
  // The Timbre Memory bank is uploaded later, once the configured repo root is
  // known (see setRepoRoot()): init() runs before the config root is resolved,
  // and the device must not discover the repo via a CWD walk-up.
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
  // untouched (MidiDevice exposes no clear hook). Queued, not immediate:
  // the audio callback owns the render thread.
  for (int ch = 0; ch < kChannels; ++ch) {
    queueMsg((0xB0u | static_cast<std::uint32_t>(ch)) | (120u << 8));
    queueMsg((0xB0u | static_cast<std::uint32_t>(ch)) | (123u << 8));
  }
}

bool Mt32Device::queueMsg(std::uint32_t msg) {
  if (synth_ == nullptr || !synth_open_) return false;
  // playMsg() is the thread-safe enqueue (no sync needed with the renderer).
  // It returns false when the queue is full; count the drop instead of
  // silently losing the event.
  if (synth_->playMsg(msg)) return true;
  dropped_msgs_.fetch_add(1);
  return false;
}

bool Mt32Device::queueSysex(const std::uint8_t* data, std::size_t len) {
  if (data == nullptr || len == 0 || synth_ == nullptr || !synth_open_) {
    return false;
  }
  if (synth_->playSysex(data, static_cast<MT32Emu::Bit32u>(len))) return true;
  dropped_msgs_.fetch_add(1);
  return false;
}

void Mt32Device::silenceChannel(int ch) {
  if (!chInRange(ch)) return;
  // Close the base tracking bars and forward a real Note-Off per held key.
  for (int note = 0; note < 128; ++note) {
    if (isNoteSounding(ch, note)) noteOff(ch, note);
  }
  if (synth_ == nullptr || !synth_open_) return;
  // CC123 All Notes Off then CC120 All Sound Off: kills sustained voices a
  // plain Note-Off would leave ringing. NEVER CC7=0 — the MT-32 captures
  // part volume at note start, so a CC7 write would silence this part on
  // its next note-on.
  queueMsg((0xB0u | static_cast<std::uint32_t>(ch)) | (123u << 8));
  queueMsg((0xB0u | static_cast<std::uint32_t>(ch)) | (120u << 8));
}

void Mt32Device::silenceOthersForStem(int solo_ch) {
  // Stem isolation: silence every other live channel. Fast-escape dormant
  // channels (they own no voices and cost nothing).
  for (int ch = 0; ch < kChannels; ++ch) {
    if (ch == solo_ch || isDormant(ch)) continue;
    silenceChannel(ch);
  }
}

void Mt32Device::dispatchNoteOn(int ch, int note, int velocity) {
  if (!chInRange(ch) || note < 0 || note >= 128) return;
  // Defense in depth for the pre-synth mute/solo filter: direct callers
  // (bypassing MidiDevice::noteOn) must not allocate a voice on a muted or
  // solo-excluded channel. Mute never rides CC7, so this is the gate.
  if (shouldFilterNoteOn(ch)) return;
  channel(ch).freq = static_cast<float>(midiHz(note));
  channel(ch).last_note = note;
  if (!inited_ || mock_ || synth_ == nullptr || !synth_open_) return;
  if (velocity < 1) velocity = 1;
  if (velocity > 127) velocity = 127;
  const std::uint32_t msg =
      (0x90u | static_cast<std::uint32_t>(ch & 0x0F)) |
      (static_cast<std::uint32_t>(note) << 8) |
      (static_cast<std::uint32_t>(velocity) << 16);
  queueMsg(msg);
}

void Mt32Device::dispatchNoteOff(int ch, int note) {
  if (!chInRange(ch) || note < 0 || note >= 128) return;
  if (activeNoteCount(ch) == 0) channel(ch).active = false;
  if (!inited_ || mock_ || synth_ == nullptr || !synth_open_) return;
  const std::uint32_t msg =
      (0x80u | static_cast<std::uint32_t>(ch & 0x0F)) |
      (static_cast<std::uint32_t>(note) << 8);
  queueMsg(msg);
}

void Mt32Device::programChange(int ch, int program) {
  dispatchProgramChange(ch, program);
}

void Mt32Device::dispatchProgramChange(int ch, int program) {
  if (!chInRange(ch)) return;
  int prog = program;
  if (prog < 0) prog = 0;
  if (prog > 127) prog = 127;
  if (ch == kRhythmChannel) {
    // Channel 9 is the fixed rhythm part: MUNT's RhythmPart::setProgram() is
    // a no-op and the part has no melodic program, so do not maintain a
    // melodic programs_[9] shadow. The rhythm row reads MUNT's patch name /
    // getPartStates() bit 8 instead. The channel still wakes so its strip
    // becomes visible, and the program change is still forwarded (harmless).
    wakeChannel(ch);
  } else {
    setProgram(ch, prog);
  }
  if (!inited_ || mock_ || synth_ == nullptr || !synth_open_) return;
  const std::uint32_t msg =
      (0xC0u | static_cast<std::uint32_t>(ch & 0x0F)) |
      (static_cast<std::uint32_t>(prog) << 8);
  queueMsg(msg);
}

void Mt32Device::sendControlChange(int ch, int cc, int value) {
  dispatchControlChange(ch, cc, value);
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
  queueMsg(msg);
}

void Mt32Device::dispatchSysEx(const std::uint8_t* data, std::size_t len) {
  if (data == nullptr || len == 0) return;
  if (mock_) parseDisplaySysEx(data, len);
  if (!inited_ || mock_ || synth_ == nullptr || !synth_open_) return;
  // playSysex() copies the payload into the queue's preallocated storage
  // (configureMIDIEventQueueSysexStorage), so the caller's buffer may be
  // transient. It is safe to call from the UI thread.
  queueSysex(data, len);
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

void Mt32Device::loadCustomTimbreBank() {
  if (mock_ || !synth_open_ || repo_root_.empty() || timbre_bank_loaded_) return;
  std::vector<std::vector<std::uint8_t> > timbres;
  SongTimbreSysex unused;
  // The bridge imports the repository's own encoder (gen_mt32_patches); if
  // python3/the script/the repo is unavailable this is a silent no-op, which
  // is exactly the pre-existing factory-occupant behaviour.
  if (!sysex_bridge::run(repo_root_, "timbres", "", &timbres, &unused)) return;
  sendSysExMessages(timbres);
  timbre_bank_loaded_ = true;
}

void Mt32Device::setRepoRoot(const std::string& root) {
  if (root != repo_root_) {
    repo_root_ = root;
    timbre_bank_loaded_ = false;  // re-upload if the project changed
  }
  loadCustomTimbreBank();
}

void Mt32Device::sendSysExMessages(
    const std::vector<std::vector<std::uint8_t> >& msgs) {
  for (const std::vector<std::uint8_t>& msg : msgs) {
    if (msg.empty()) continue;
    dispatchSysEx(msg.data(), msg.size());
  }
}

void Mt32Device::applySongTimbres(
    const std::string& song,
    const std::vector<std::vector<std::uint8_t> >& setup,
    const std::vector<std::vector<std::uint8_t> >& cleanup) {
  // Device-tab switch (same track): the previously armed timbres are already
  // latched in Patch Memory -- re-sending would be redundant, and re-latching
  // a Program Change is the caller's job (the event stream re-plays it).
  if (!song.empty() && song == timbre_song_) return;
  // Cleanup-then-setup, exactly like the DOS game's midi_seq_start: restore
  // the previous track's Patch Memory before pointing at this track's.
  sendSysExMessages(timbre_cleanup_);
  sendSysExMessages(setup);
  timbre_cleanup_ = cleanup;
  timbre_song_ = song;
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
  if (const Telemetry* t = uiTelemetry()) return t->partial_count;
  std::lock_guard<std::mutex> lock(telemetry_mutex_);
  return telemetry_.partial_count;
}

int Mt32Device::partialState(int slot) const {
  if (slot < 0 || slot >= kPartialSlots) return -1;
  if (!inited_ || mock_ || synth_ == nullptr || !synth_open_) {
    return slot < activePartialCount()
               ? static_cast<int>(MT32Emu::PartialState_SUSTAIN)
               : static_cast<int>(MT32Emu::PartialState_INACTIVE);
  }
  if (const Telemetry* t = uiTelemetry()) {
    return t->partial_state[static_cast<std::size_t>(slot)];
  }
  std::lock_guard<std::mutex> lock(telemetry_mutex_);
  return telemetry_.partial_state[static_cast<std::size_t>(slot)];
}

bool Mt32Device::partActive(int part) const {
  if (part < 0 || part >= kParts) return false;
  if (!inited_ || mock_ || synth_ == nullptr || !synth_open_) {
    const int ch = (part == 8) ? kRhythmChannel : part + 1;
    return !isDormant(ch) && activeNoteCount(ch) > 0;
  }
  if (const Telemetry* t = uiTelemetry()) {
    return t->part_active[static_cast<std::size_t>(part)] != 0;
  }
  std::lock_guard<std::mutex> lock(telemetry_mutex_);
  return telemetry_.part_active[static_cast<std::size_t>(part)] != 0;
}

std::string Mt32Device::lcdText() const {
  if (!inited_ || mock_ || synth_ == nullptr || !synth_open_) {
    return std::string(lcd_.data(), kLcdChars);
  }
  if (const Telemetry* t = uiTelemetry()) return std::string(t->lcd.data());
  std::lock_guard<std::mutex> lock(telemetry_mutex_);
  return std::string(telemetry_.lcd.data());
}

void Mt32Device::beginUiFrame() const {
  // One lock for the whole UI draw pass; the accessors then read the cache.
  if (!inited_ || mock_ || synth_ == nullptr || !synth_open_) return;
  std::lock_guard<std::mutex> lock(telemetry_mutex_);
  ui_telemetry_ = telemetry_;
  ui_frame_open_ = true;
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
  // The render thread is the only thread that touches the live synth, so
  // refresh the UI telemetry shadow from here (under the shadow mutex).
  updateTelemetry();
}

void Mt32Device::updateTelemetry() {
  if (synth_ == nullptr || !synth_open_) return;
  Telemetry t;
  const MT32Emu::Bit32u partials = synth_->getPartialCount();
  std::vector<MT32Emu::PartialState> states(partials > 0 ? partials : 1u);
  synth_->getPartialStates(states.data());
  // "Active" is MUNT's documented LCD-activity predicate, shared with the part
  // LEDs: a partial counts only while it is non-releasing (ATTACK/SUSTAIN),
  // exactly the per-partial basis of Synth::getPartStates(). Counting RELEASE
  // here lit the readout while every part LED was dark (release tail, or a
  // Note-Off mute). The 32-slot grid still renders RELEASE as a phase colour
  // -- that is a phase view, not a sounding voice.
  int count = 0;
  for (MT32Emu::Bit32u i = 0; i < partials; ++i) {
    if (states[i] == MT32Emu::PartialState_ATTACK ||
        states[i] == MT32Emu::PartialState_SUSTAIN) {
      ++count;
    }
    if (i < static_cast<MT32Emu::Bit32u>(kPartialSlots)) {
      t.partial_state[i] = static_cast<int>(states[i]);
    }
  }
  t.partial_count = count;
  const MT32Emu::Bit32u part_states = synth_->getPartStates();
  for (int part = 0; part < kParts; ++part) {
    t.part_active[static_cast<std::size_t>(part)] =
        static_cast<int>((part_states >> part) & 1u);
    const char* name =
        synth_->getPatchName(static_cast<MT32Emu::Bit8u>(part));
    std::snprintf(t.patch_name[static_cast<std::size_t>(part)].data(),
                  t.patch_name[static_cast<std::size_t>(part)].size(), "%s",
                  name != nullptr ? name : "");
    std::uint8_t keys[32] = {0};
    std::uint8_t vels[32] = {0};
    const MT32Emu::Bit32u cnt = synth_->getPlayingNotes(
        static_cast<MT32Emu::Bit8u>(part), keys, vels);
    const int n = static_cast<int>(cnt < 32u ? cnt : 32u);
    t.playing_count[static_cast<std::size_t>(part)] = n;
    for (int i = 0; i < n; ++i) {
      t.playing_keys[static_cast<std::size_t>(part)]
                    [static_cast<std::size_t>(i)] = keys[i];
      t.playing_vel[static_cast<std::size_t>(part)]
                   [static_cast<std::size_t>(i)] = vels[i];
    }
  }
  char buf[kLcdChars + 1];
  std::memset(buf, ' ', kLcdChars);
  buf[kLcdChars] = '\0';
  synth_->getDisplayState(buf);
  std::memcpy(t.lcd.data(), buf, kLcdChars);
  t.lcd[kLcdChars] = '\0';
  std::lock_guard<std::mutex> lock(telemetry_mutex_);
  telemetry_ = t;
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
  renderSynthMono(buf, frames);
}

void Mt32Device::setMute(int ch, bool muted) {
  MidiDevice::setMute(ch, muted);
  // Mute = pre-synth Note-On filter (MidiDevice) + note-offs for held notes.
  // Never CC7=0: the MT-32 captures part volume at note start, so a CC7
  // write aimed at a dormant channel silences it forever.
  if (muted) silenceChannel(ch);
}

void Mt32Device::setSolo(int ch, bool soloed) {
  MidiDevice::setSolo(ch, soloed);
  if (!soloed) return;  // Un-solo: nothing held needs silencing.
  // Enabling any solo makes every non-solo channel inaudible: release the
  // voices they are holding now rather than leaving them ringing.
  for (int other = 0; other < kChannels; ++other) {
    if (!synthAudible(other)) silenceChannel(other);
  }
}

std::string Mt32Device::patchName(int part) const {
  if (part == 8) return "Rhythm Channel";
  if (part >= 0 && part < kParts && synth_ != nullptr && synth_open_ &&
      !mock_) {
    const std::size_t p = static_cast<std::size_t>(part);
    if (const Telemetry* t = uiTelemetry()) {
      return std::string(t->patch_name[p].data());
    }
    std::lock_guard<std::mutex> lock(telemetry_mutex_);
    return std::string(telemetry_.patch_name[p].data());
  }
  const int ch = (part == 8) ? kRhythmChannel : part + 1;
  return mt32TimbreName(program(ch));
}

int Mt32Device::getPlayingNotes(int part, std::uint8_t* keys,
                                std::uint8_t* velocities) const {
  if (part < 0 || part >= kParts) return 0;
  const auto copyFrom = [&](const Telemetry& t) {
    const std::size_t p = static_cast<std::size_t>(part);
    const int count = t.playing_count[p];
    for (int i = 0; i < count; ++i) {
      if (keys != nullptr) keys[i] = t.playing_keys[p][i];
      if (velocities != nullptr) velocities[i] = t.playing_vel[p][i];
    }
    return count;
  };
  if (synth_ != nullptr && synth_open_ && !mock_) {
    if (const Telemetry* t = uiTelemetry()) return copyFrom(*t);
    std::lock_guard<std::mutex> lock(telemetry_mutex_);
    return copyFrom(telemetry_);
  }
  // Mock / fallback: read from MidiDevice note tracking.
  const int ch = (part == 8) ? kRhythmChannel : part + 1;
  int count = 0;
  for (int note = 0; note < kNotesPerChannel && count < 32; ++note) {
    if (isNoteSounding(ch, note)) {
      if (keys != nullptr) keys[count] = static_cast<std::uint8_t>(note);
      if (velocities != nullptr)
        velocities[count] = static_cast<std::uint8_t>(noteVelocity(ch, note));
      ++count;
    }
  }
  return count;
}

DeviceSnapshot Mt32Device::snapshot() const {
  DeviceSnapshot s = makeSnapshot();
  // MT-32 channels are part-mapped (1-8 -> parts 1-8, 9 -> rhythm) or
  // part-less (0, 10-15). Our own register mirror is not authoritative for
  // any of them, so mark every channel engine-sourced: a reset() clears the
  // synth but not the base note tracking, and the tracker must not show the
  // stale mirror.
  for (ChannelState& cs : s.channels) {
    cs.library_voices = true;
    cs.sounding_note = -1;
  }
  const auto setPart = [&s](int part, const std::uint8_t* keys, int count) {
    const int ch = (part == kParts - 1) ? kRhythmChannel : part + 1;
    if (ch < 0 || static_cast<std::size_t>(ch) >= s.channels.size()) return;
    ChannelState& cs = s.channels[static_cast<std::size_t>(ch)];
    cs.library_voices = true;
    // One tracker cell per channel: the first engine-reported note.
    cs.sounding_note = (count > 0 && keys != nullptr) ? keys[0] : -1;
  };
  if (synth_ != nullptr && synth_open_ && !mock_) {
    std::lock_guard<std::mutex> lock(telemetry_mutex_);
    for (int part = 0; part < kParts; ++part) {
      const std::size_t p = static_cast<std::size_t>(part);
      setPart(part, telemetry_.playing_keys[p].data(),
              telemetry_.playing_count[p]);
    }
    return s;
  }
  // Mock / fallback: getPlayingNotes() reads the base note matrix (no lock).
  for (int part = 0; part < kParts; ++part) {
    std::uint8_t keys[32] = {0};
    std::uint8_t vels[32] = {0};
    const int count = getPlayingNotes(part, keys, vels);
    setPart(part, keys, count);
  }
  return s;
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
    // Real synth: isolate this channel by releasing the others' held notes
    // (never CC7=0). See silenceOthersForStem().
    silenceOthersForStem(ch);
    renderSynthMono(bufs[ch], frames);
  }
}

}  // namespace audio_dbg
