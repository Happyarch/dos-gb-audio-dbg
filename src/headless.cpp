// Stage 8.1/8.2: Headless renderer + replayer implementation. See headless.h.

#include "headless.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "cli.h"
#include "config.h"
#include "devices/gb_apu_device.h"
#include "devices/gm_device.h"
#include "devices/midi_device.h"
#include "devices/mt32_device.h"
#include "devices/opl3_device.h"
#include "devices/sound_device.h"
#include "enhancement_manager.h"
#include "recorder.h"
#include "song_catalog.h"

namespace audio_dbg {
namespace {

constexpr std::uint8_t kNoteOnOpcode = 0x90;
constexpr std::uint8_t kNoteOffOpcode = 0x80;
constexpr std::uint8_t kProgramChangeOpcode = 0xC0;
constexpr std::uint8_t kControlChangeOpcode = 0xB0;

// Expands mono device PCM to stereo-interleaved (L == R) for the WAV writer.
void monoToStereo(const float* mono, std::size_t n, std::vector<float>* out) {
  out->resize(n * 2);
  for (std::size_t i = 0; i < n; ++i) {
    (*out)[i * 2] = mono[i];
    (*out)[i * 2 + 1] = mono[i];
  }
}

void noteResultSummary(const HeadlessResult& r) {
  std::printf(
      "pkmn-audio-dbg headless: device=%s rate=%u track=%s frames=%u/%u "
      "samples=%llu peak=%.4f nonzero=%llu out=%s\n",
      r.device.c_str(), r.sample_rate, r.track.c_str(), r.frames_rendered,
      r.frames_requested, static_cast<unsigned long long>(r.total_samples),
      static_cast<double>(r.peak),
      static_cast<unsigned long long>(r.nonzero_samples),
      r.out_path.empty() ? "(discarded)" : r.out_path.c_str());
}

}  // namespace

std::unique_ptr<SoundDevice> createHeadlessDevice(
    const std::string& name, std::uint32_t* rateOut, std::string* canonicalOut,
    std::string* err) {
  const std::string canon = normalizeDeviceName(name);
  if (canon.empty()) {
    if (err != nullptr) {
      *err = "unknown --device " + name + " (want mt32, gm, opl3, or gbapu)";
    }
    return nullptr;
  }
  std::unique_ptr<SoundDevice> dev;
  std::uint32_t rate = 0;
  if (canon == "mt32") {
    auto mt = new Mt32Device();
    rate = mt->sampleRate();
    dev.reset(mt);
  } else if (canon == "gm") {
    auto gm = new GmDevice();
    rate = gm->sampleRate();
    dev.reset(gm);
  } else if (canon == "opl3") {
    auto opl = new Opl3Device();
    rate = opl->sampleRate();
    dev.reset(opl);
  } else {
    auto apu = new GbApuDevice();
    rate = static_cast<std::uint32_t>(apu->sampleRate());
    dev.reset(apu);
  }
  if (rateOut != nullptr) *rateOut = rate;
  if (canonicalOut != nullptr) *canonicalOut = canon;
  return dev;
}

std::string deviceMidiTarget(const std::string& canonical) {
  return canonical == "gm" ? "gm" : "mt32";
}

std::size_t frameSampleCount(std::uint32_t rate, std::uint32_t frame) {
  const std::uint64_t r = rate;
  const std::uint64_t f = frame;
  const std::uint64_t hi = ((f + 1) * r) / 60;
  const std::uint64_t lo = (f * r) / 60;
  return static_cast<std::size_t>(hi - lo);
}

std::uint64_t totalSamplesFor(std::uint32_t rate, std::uint32_t frames) {
  return (static_cast<std::uint64_t>(rate) * frames) / 60;
}

void dispatchReplayEvent(SoundDevice* dev, std::uint8_t opcode,
                         const std::uint8_t* payload, std::size_t len) {
  if (dev == nullptr) return;
  // MIDI backends get the concrete note/program/CC API, exactly like
  // SessionEngine::dispatch(): a replayed track must apply its Program
  // Change (0xC0) and Control Change (0xB0) events, not just notes, or the
  // patches/volume/pan the capture carried are silently dropped.
  if (MidiDevice* midi = dynamic_cast<MidiDevice*>(dev)) {
    if (payload != nullptr) {
      if (opcode == kNoteOnOpcode && len >= 3) {
        midi->noteOn(payload[0], payload[1], payload[2]);
        return;
      }
      if (opcode == kNoteOffOpcode && len >= 2) {
        midi->noteOff(payload[0], payload[1]);
        return;
      }
      if (opcode == kProgramChangeOpcode && len >= 2) {
        midi->programChange(payload[0], payload[1]);
        return;
      }
      if (opcode == kControlChangeOpcode && len >= 3) {
        midi->sendControlChange(payload[0], payload[1], payload[2]);
        return;
      }
    }
  }
  dev->handleCommand(opcode, payload, len);
}

int runHeadless(const CliOptions& opt, std::string* err, HeadlessResult* res) {
  if (!opt.replay_path.empty()) return runReplayLog(opt, err, res);
  return runHeadlessTrack(opt, err, res);
}

int runHeadlessTrack(const CliOptions& opt, std::string* err,
                     HeadlessResult* res) {
  auto fail = [&](const std::string& msg) {
    if (err != nullptr) *err = msg;
    return 1;
  };
  if (opt.track.empty()) {
    return fail("--headless needs --track <ID> (or use --replay <log>)");
  }
  std::uint32_t rate = 0;
  std::string canon;
  std::unique_ptr<SoundDevice> dev =
      createHeadlessDevice(opt.device, &rate, &canon, err);
  if (dev == nullptr || rate == 0) return 1;
  if (!dev->init()) {
    return fail("device " + canon + " failed to initialize");
  }

  ConfigManager config_mgr;
  config_mgr.load();
  if (!opt.project.empty()) {
    config_mgr.setCurrentProject(opt.project);
  }
  if (!opt.project_dir.empty()) {
    config_mgr.setActiveProjectRoot(opt.project_dir);
  }
  const std::string root = config_mgr.activeRoot();
  const std::string consts = config_mgr.resolveConstantsPath(opt.constants_path);
  const std::string headers = config_mgr.resolveHeadersDir();
  const std::string overrides = config_mgr.resolveOverridesDir(opt.overrides_dir);
  const std::string enhancements = config_mgr.resolveEnhancementsDir(opt.enhancements_dir);

  SongCatalog catalog;
  catalog.load(root, consts, headers);
  const SongInfo* info = catalog.findTrack(opt.track);
  if (info == nullptr) {
    dev->shutdown();
    return fail("unknown track " + opt.track);
  }
  EnhancementManager enh_mgr;
  enh_mgr.setRepoRoot(root);
  enh_mgr.setEnhanceDir(enhancements);
  enh_mgr.setRevisionsDir(root.empty() ? "" : root + "/dos_port/tools/audio/.revisions");
  const std::string target = deviceMidiTarget(canon);
  std::vector<SimNoteEvent> base =
      enh_mgr.loadSongBaseline(info->header_label, target);
  if (base.empty()) {
    dev->shutdown();
    return fail("track " + info->constant_name + " has no " + target +
                " baseline rendering");
  }
  std::uint32_t track_frames = 0;
  for (const SimNoteEvent& e : base) {
    track_frames = std::max(track_frames, e.frame);
  }
  ++track_frames;  // One past the last event.
  std::uint32_t frames = opt.frames_explicit ? opt.frames : track_frames;
  if (frames == 0) frames = kCliDefaultFrames;

  SessionEngine engine;
  engine.setActiveDevice(dev.get());
  engine.setEvents(base);
  if (!opt.no_enhancement) {
    std::vector<SimNoteEvent> enh =
        enh_mgr.compileEnhancement(info->header_label, target);
    if (!enh.empty()) engine.setEnhancementEvents(enh);
  } else {
    engine.setEnhancementEnabled(false);
  }
  engine.setTotalFrames(frames);
  engine.play();

  WavWriter wav;
  if (!opt.out_path.empty()) {
    if (!wav.open(opt.out_path, static_cast<int>(rate), 2,
                  WavFormat::FLOAT32)) {
      dev->shutdown();
      return fail("cannot open --out " + opt.out_path);
    }
  }

  HeadlessResult r;
  r.frames_requested = frames;
  r.sample_rate = rate;
  r.track = info->constant_name;
  r.device = canon;
  r.out_path = opt.out_path;
  std::vector<float> mono;
  std::vector<float> stereo;
  for (std::uint32_t f = 0; f < frames; ++f) {
    engine.tick();
    const std::size_t n = frameSampleCount(rate, f);
    mono.assign(n, 0.0f);
    if (n > 0) dev->render(mono.data(), n);
    for (std::size_t i = 0; i < n; ++i) {
      const float a = std::fabs(mono[i]);
      if (a > r.peak) r.peak = a;
      if (mono[i] != 0.0f) ++r.nonzero_samples;
    }
    ++r.frames_rendered;
    r.total_samples += n;
    if (wav.isOpen() && n > 0) {
      monoToStereo(mono.data(), n, &stereo);
      if (!wav.writeFrames(stereo.data(), n)) {
        dev->shutdown();
        return fail("failed writing --out " + opt.out_path);
      }
    }
  }
  wav.close();
  dev->shutdown();
  noteResultSummary(r);
  if (res != nullptr) *res = r;
  return 0;
}

int runReplayLog(const CliOptions& opt, std::string* err,
                 HeadlessResult* res) {
  auto fail = [&](const std::string& msg) {
    if (err != nullptr) *err = msg;
    return 1;
  };
  if (opt.replay_path.empty()) return fail("--replay needs a log path");
  std::vector<AudioLogEvent> log;
  if (!AudioLogReader::readAll(opt.replay_path, &log)) {
    return fail("cannot read audiolog " + opt.replay_path);
  }
  // Frame order is the dispatch order (the session engine keeps a sorted
  // stream too); a stable sort here makes hand-written or merged logs replay
  // exactly like a capture written in frame order.
  std::stable_sort(log.begin(), log.end(),
                   [](const AudioLogEvent& a, const AudioLogEvent& b) {
                     return a.frame < b.frame;
                   });
  std::uint32_t log_frames = 0;
  for (const AudioLogEvent& e : log) {
    log_frames = std::max(log_frames, e.frame);
  }
  if (!log.empty()) ++log_frames;  // One past the last record.
  std::uint32_t frames =
      opt.frames_explicit ? opt.frames : log_frames;
  if (frames == 0) frames = kCliDefaultFrames;

  std::uint32_t rate = 0;
  std::string canon;
  std::unique_ptr<SoundDevice> dev =
      createHeadlessDevice(opt.device, &rate, &canon, err);
  if (dev == nullptr || rate == 0) return 1;
  if (!dev->init()) {
    return fail("device " + canon + " failed to initialize");
  }

  WavWriter wav;
  if (!opt.out_path.empty()) {
    if (!wav.open(opt.out_path, static_cast<int>(rate), 2,
                  WavFormat::FLOAT32)) {
      dev->shutdown();
      return fail("cannot open --out " + opt.out_path);
    }
  }

  // Index records by frame (logs are written in frame order).
  std::size_t cursor = 0;
  HeadlessResult r;
  r.frames_requested = frames;
  r.sample_rate = rate;
  r.track = "(replay)";
  r.device = canon;
  r.out_path = opt.out_path;
  std::vector<float> mono;
  std::vector<float> stereo;
  for (std::uint32_t f = 0; f < frames; ++f) {
    while (cursor < log.size() && log[cursor].frame == f) {
      const AudioLogEvent& e = log[cursor];
      dispatchReplayEvent(dev.get(), e.opcode,
                          e.payload.empty() ? nullptr : e.payload.data(),
                          e.payload.size());
      ++cursor;
    }
    // Late records (frame < f from an unsorted log) still apply in order
    // rather than stalling the frame loop.
    while (cursor < log.size() && log[cursor].frame < f) {
      const AudioLogEvent& e = log[cursor];
      dispatchReplayEvent(dev.get(), e.opcode,
                          e.payload.empty() ? nullptr : e.payload.data(),
                          e.payload.size());
      ++cursor;
    }
    dev->tick(f);
    const std::size_t n = frameSampleCount(rate, f);
    mono.assign(n, 0.0f);
    if (n > 0) dev->render(mono.data(), n);
    for (std::size_t i = 0; i < n; ++i) {
      const float a = std::fabs(mono[i]);
      if (a > r.peak) r.peak = a;
      if (mono[i] != 0.0f) ++r.nonzero_samples;
    }
    ++r.frames_rendered;
    r.total_samples += n;
    if (wav.isOpen() && n > 0) {
      monoToStereo(mono.data(), n, &stereo);
      if (!wav.writeFrames(stereo.data(), n)) {
        dev->shutdown();
        return fail("failed writing --out " + opt.out_path);
      }
    }
  }
  wav.close();
  dev->shutdown();
  noteResultSummary(r);
  if (res != nullptr) *res = r;
  return 0;
}

}  // namespace audio_dbg
