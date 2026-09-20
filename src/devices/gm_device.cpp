// Stage 4.4: GmDevice implementation.
// See gm_device.h for the contract.

#include "devices/gm_device.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <fluidsynth.h>

namespace audio_dbg {

namespace {

// Standard General MIDI 128 program names (melodic bank).
const char* const kGmPrograms[128] = {
    "Acoustic Grand Piano", "Bright Acoustic Piano", "Electric Grand Piano",
    "Honky-tonk Piano", "Electric Piano 1", "Electric Piano 2",
    "Harpsichord", "Clavi", "Celesta", "Glockenspiel", "Music Box",
    "Vibraphone", "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
    "Drawbar Organ", "Percussive Organ", "Rock Organ", "Church Organ",
    "Reed Organ", "Accordion", "Harmonica", "Tango Accordion",
    "Acoustic Guitar (nylon)", "Acoustic Guitar (steel)",
    "Electric Guitar (jazz)", "Electric Guitar (clean)",
    "Electric Guitar (muted)", "Overdriven Guitar", "Distortion Guitar",
    "Guitar harmonics", "Acoustic Bass", "Electric Bass (finger)",
    "Electric Bass (pick)", "Fretless Bass", "Slap Bass 1", "Slap Bass 2",
    "Synth Bass 1", "Synth Bass 2", "Violin", "Viola", "Cello",
    "Contrabass", "Tremolo Strings", "Pizzicato Strings", "Orchestral Harp",
    "Timpani", "String Ensemble 1", "String Ensemble 2", "SynthStrings 1",
    "SynthStrings 2", "Choir Aahs", "Voice Oohs", "Synth Voice",
    "Orchestra Hit", "Trumpet", "Trombone", "Tuba", "Muted Trumpet",
    "French Horn", "Brass Section", "SynthBrass 1", "SynthBrass 2",
    "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax", "Oboe",
    "English Horn", "Bassoon", "Clarinet", "Piccolo", "Flute", "Recorder",
    "Pan Flute", "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
    "Lead 1 (square)", "Lead 2 (sawtooth)", "Lead 3 (calliope)",
    "Lead 4 (chiff)", "Lead 5 (charang)", "Lead 6 (voice)",
    "Lead 7 (fifths)", "Lead 8 (bass + lead)", "Pad 1 (new age)",
    "Pad 2 (warm)", "Pad 3 (polysynth)", "Pad 4 (choir)", "Pad 5 (bowed)",
    "Pad 6 (metallic)", "Pad 7 (halo)", "Pad 8 (sweep)", "FX 1 (rain)",
    "FX 2 (soundtrack)", "FX 3 (crystal)", "FX 4 (atmosphere)",
    "FX 5 (brightness)", "FX 6 (goblins)", "FX 7 (echoes)",
    "FX 8 (sci-fi)", "Sitar", "Banjo", "Shamisen", "Koto", "Kalimba",
    "Bag pipe", "Fiddle", "Shanai", "Tinkle Bell", "Agogo", "Steel Drums",
    "Woodblock", "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal",
    "Guitar Fret Noise", "Breath Noise", "Seashore", "Bird Tweet",
    "Telephone Ring", "Helicopter", "Applause", "Gunshot",
};

constexpr double kTwoPi = 6.28318530717958647692;

}  // namespace

GmDevice::GmDevice(int device_id, std::string device_name,
                   std::uint32_t sample_rate)
    : MidiDevice(device_id, std::move(device_name), kChannels),
      sample_rate_(sample_rate == 0 ? kDefaultRate : sample_rate) {
  for (auto& row : phases_) row.fill(0.0);
}

GmDevice::~GmDevice() { shutdown(); }

double GmDevice::midiHz(int note) {
  return 440.0 * std::pow(2.0, (note - 69) / 12.0);
}

bool GmDevice::synthAudible(int ch) const {
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

bool GmDevice::loadSoundFont() {
  // SOUNDFONT=none forces mock mode (headless CI / fallback tests).
  if (const char* force = std::getenv("SOUNDFONT")) {
    if (std::strcmp(force, "none") == 0) {
      sf_id_ = -1;
      return false;
    }
  }
  static const char* const kCandidates[] = {
      "/usr/share/soundfonts/default.sf2",
      "/usr/share/soundfonts/FluidR3_GM.sf2",
      "/usr/share/soundfonts/FluidR3_GS.sf2",
      "/usr/share/sounds/sf2/default.sf2",
      "/usr/share/sounds/sf2/FluidR3_GM.sf2",
      "/usr/share/minuet/soundfonts/GeneralUser-v1.47.sf2",
      "/mnt/sdb1/ROMs/Soundfonts/MT_Retro_Dreams.sf2",
  };
  if (const char* env = std::getenv("SOUNDFONT")) {
    sf_id_ = fluid_synth_sfload(synth_, env, 1);
    if (sf_id_ >= 0) {
      sf_path_ = env;
      return true;
    }
  }
  if (const char* home = std::getenv("HOME")) {
    const std::string hp = std::string(home) + "/.soundfonts/default.sf2";
    sf_id_ = fluid_synth_sfload(synth_, hp.c_str(), 1);
    if (sf_id_ >= 0) {
      sf_path_ = hp;
      return true;
    }
  }
  for (const char* cand : kCandidates) {
    sf_id_ = fluid_synth_sfload(synth_, cand, 1);
    if (sf_id_ >= 0) {
      sf_path_ = cand;
      return true;
    }
  }
  sf_id_ = -1;
  return false;
}

bool GmDevice::init() {
  if (inited_) return true;
  settings_ = new_fluid_settings();
  if (settings_ == nullptr) {
    mock_ = true;
    inited_ = true;
    return true;
  }
  fluid_settings_setnum(settings_, "synth.sample-rate",
                        static_cast<double>(sample_rate_));
  synth_ = new_fluid_synth(settings_);
  if (synth_ == nullptr) {
    mock_ = true;
    inited_ = true;
    return true;
  }
  mock_ = !loadSoundFont();
  inited_ = true;
  return true;
}

void GmDevice::shutdown() {
  if (synth_ != nullptr) {
    delete_fluid_synth(synth_);
    synth_ = nullptr;
  }
  if (settings_ != nullptr) {
    delete_fluid_settings(settings_);
    settings_ = nullptr;
  }
  sf_id_ = -1;
  sf_path_.clear();
  inited_ = false;
  mock_ = true;
}

void GmDevice::reset() {
  for (auto& row : phases_) row.fill(0.0);
  if (!inited_ || mock_ || synth_ == nullptr) return;
  for (int ch = 0; ch < kChannels; ++ch) {
    fluid_synth_all_sounds_off(synth_, ch);
    fluid_synth_all_notes_off(synth_, ch);
  }
}

void GmDevice::dispatchNoteOn(int ch, int note, int velocity) {
  if (!chInRange(ch) || note < 0 || note >= 128) return;
  channel(ch).freq = static_cast<float>(midiHz(note));
  channel(ch).last_note = note;
  if (!inited_ || mock_ || synth_ == nullptr) return;
  if (velocity < 1) velocity = 1;
  if (velocity > 127) velocity = 127;
  fluid_synth_noteon(synth_, ch, note, velocity);
}

void GmDevice::dispatchNoteOff(int ch, int note) {
  if (!chInRange(ch) || note < 0 || note >= 128) return;
  if (activeNoteCount(ch) == 0) channel(ch).active = false;
  if (!inited_ || mock_ || synth_ == nullptr) return;
  fluid_synth_noteoff(synth_, ch, note);
}

void GmDevice::programChange(int ch, int program) {
  dispatchProgramChange(ch, program);
}

void GmDevice::dispatchProgramChange(int ch, int program) {
  if (!chInRange(ch)) return;
  setProgram(ch, program);
  if (!inited_ || mock_ || synth_ == nullptr) return;
  fluid_synth_program_change(synth_, ch, MidiDevice::program(ch));
}

void GmDevice::dispatchControlChange(int ch, int cc, int value) {
  if (!chInRange(ch) || cc < 0 || cc > 127) return;
  setControlChange(ch, cc, value);
  if (!inited_ || mock_ || synth_ == nullptr) return;
  fluid_synth_cc(synth_, ch, cc, controlChange(ch, cc));
}

const char* GmDevice::gmProgramName(int program) {
  if (program < 0) program = 0;
  if (program > 127) program = 127;
  return kGmPrograms[static_cast<std::size_t>(program)];
}

std::string GmDevice::resolveProgramName(int channel, int program) const {
  (void)channel;
  return std::string(gmProgramName(program));
}

void GmDevice::renderMockMono(float* out, std::size_t frames,
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

void GmDevice::renderSynthMono(float* out, std::size_t frames) {
  constexpr std::size_t kChunk = 256;
  float left[kChunk];
  float right[kChunk];
  std::size_t done = 0;
  while (done < frames) {
    const std::size_t want = std::min(kChunk, frames - done);
    fluid_synth_write_float(synth_, static_cast<int>(want), left, 0, 1,
                            right, 0, 1);
    for (std::size_t i = 0; i < want; ++i) {
      out[done + i] = (left[i] + right[i]) * 0.5f;
    }
    done += want;
  }
}

void GmDevice::muteOthersForStem(int solo_ch) {
  for (int ch = 0; ch < kChannels; ++ch) {
    fluid_synth_cc(synth_, ch, 7, (ch == solo_ch) ? controlChange(ch, 7) : 0);
  }
}

void GmDevice::restoreVolumes() {
  // Converge the engine to the live mute matrix: audible channels get
  // their tracked CC7, muted/solo-excluded channels stay at 0, so voices
  // allocated on a muted channel between renders are born silent.
  for (int ch = 0; ch < kChannels; ++ch) {
    fluid_synth_cc(synth_, ch, 7, synthAudible(ch) ? controlChange(ch, 7) : 0);
  }
}

void GmDevice::render(float* buf, std::size_t frames) {
  if (buf == nullptr || frames == 0) return;
  if (!inited_) {
    std::memset(buf, 0, frames * sizeof(float));
    return;
  }
  if (mock_) {
    renderMockMono(buf, frames, -1);
    return;
  }
  // Converge engine volumes to the mute matrix every block (16 CCs,
  // negligible vs synthesis): this flushes pending mute/unmute/CC changes
  // even when no voice is sounding, so voices allocated afterwards are
  // born under the live matrix.
  restoreVolumes();
  renderSynthMono(buf, frames);
}

void GmDevice::renderPerChannel(float** bufs, std::size_t frames) {
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
