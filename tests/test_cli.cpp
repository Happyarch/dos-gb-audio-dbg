// Stage 8.3: Acceptance tests for headless CLI + audiolog replay verification.
// Verifies (headless: no GUI, no audio hardware):
//   1. CLI parsing: --headless/--track/--device/--frames/--out/--replay,
//      --no-enh/--enhancement 0|1, --help, = forms, error cases.
//   2. Device name normalization (mt32/gm/opl3/gbapu + GB-APU/gb_apu forms).
//   3. Frame/sample math: per-frame distribution telescopes to the exact total.
//   4. Headless track render (Music_PalletTown, mt32 mock, fixed frames):
//      valid WAV header, exact frame/sample counts, non-zero content, and
//      byte-identical output across two runs (null difference).
//   5. AudioLog replay: a hand-recorded log replays to a valid non-silent WAV;
//      two replays are byte-identical; and replay output matches a direct
//      SessionEngine render of the same event stream (record/replay parity).
// MT32_ROM_DIR=none + SOUNDFONT=none force the deterministic mock synths.
// Exits 0 with ALL PASS, 1 on any failure.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "cli.h"
#include "devices/mt32_device.h"
#include "devices/sound_device.h"
#include "headless.h"
#include "recorder.h"
#include "session_engine.h"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    ++g_checks;                                                         \
    if (!(cond)) {                                                      \
      ++g_failures;                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
    }                                                                   \
  } while (0)

std::uint32_t rdU32(const std::vector<char>& d, std::size_t o) {
  return static_cast<std::uint32_t>(static_cast<std::uint8_t>(d[o])) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(d[o + 1]))
          << 8) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(d[o + 2]))
          << 16) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(d[o + 3]))
          << 24);
}

std::uint16_t rdU16(const std::vector<char>& d, std::size_t o) {
  return static_cast<std::uint16_t>(static_cast<std::uint8_t>(d[o]) |
                                    (static_cast<std::uint8_t>(d[o + 1])
                                     << 8));
}

bool readFile(const std::string& path, std::vector<char>* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  in.seekg(0, std::ios::end);
  const long n = static_cast<long>(in.tellg());
  if (n < 0) return false;
  in.seekg(0, std::ios::beg);
  out->assign(static_cast<std::size_t>(n), 0);
  if (n > 0) in.read(out->data(), n);
  return static_cast<bool>(in);
}

std::string tmpPath(const std::string& name) {
  return std::string("/tmp/opencode/pkmn-audio-stage8-") + name;
}

// Parses argv-style words (argv[0] = prog).
bool parseWords(const std::vector<std::string>& words,
                audio_dbg::CliOptions* opt, std::string* err) {
  return audio_dbg::parseCliArgs(words, opt, err);
}

bool wavHeaderOk(const std::vector<char>& blob, int want_rate,
                 int want_channels) {
  if (blob.size() < 44) return false;
  if (blob[0] != 'R' || blob[1] != 'I' || blob[2] != 'F' || blob[3] != 'F') {
    return false;
  }
  if (blob[8] != 'W' || blob[9] != 'A' || blob[10] != 'V' || blob[11] != 'E') {
    return false;
  }
  if (blob[12] != 'f' || blob[13] != 'm' || blob[14] != 't' ||
      blob[15] != ' ') {
    return false;
  }
  if (rdU16(blob, 20) != 3) return false;  // float32.
  if (rdU16(blob, 22) != static_cast<std::uint16_t>(want_channels)) {
    return false;
  }
  if (rdU32(blob, 24) != static_cast<std::uint32_t>(want_rate)) return false;
  if (blob[36] != 'd' || blob[37] != 'a' || blob[38] != 't' ||
      blob[39] != 'a') {
    return false;
  }
  return true;
}

bool wavHasNonzero(const std::vector<char>& blob) {
  if (blob.size() <= 44) return false;
  const std::size_t nfloats = (blob.size() - 44) / 4;
  for (std::size_t i = 0; i < nfloats; ++i) {
    float v = 0.0f;
    std::memcpy(&v, blob.data() + 44 + i * 4, 4);
    if (v != 0.0f) return true;
  }
  return false;
}

}  // namespace

int main() {
  using namespace audio_dbg;
  std::system("mkdir -p /tmp/opencode");
  // Deterministic mock synths regardless of host ROMs/SoundFonts.
  ::setenv("MT32_ROM_DIR", "none", 1);
  ::setenv("SOUNDFONT", "none", 1);
#ifdef PKMN_TEST_REPO_ROOT
  ::setenv("PKMN_REPO_ROOT", PKMN_TEST_REPO_ROOT, 1);
#endif

  // --- 1. CLI parsing ------------------------------------------------------
  {
    CliOptions o;
    std::string err;
    CHECK(parseWords({"pkmn-audio-dbg", "--headless", "--track",
                      "MUSIC_PALLET_TOWN", "--device", "mt32", "--frames",
                      "120", "--out", "a.wav"},
                     &o, &err));
    CHECK(o.headless);
    CHECK(o.track == "MUSIC_PALLET_TOWN");
    CHECK(o.device == "mt32");
    CHECK(o.frames_explicit && o.frames == 120u);
    CHECK(o.out_path == "a.wav");
    CHECK(!o.show_help && !o.no_enhancement);
    CHECK(o.replay_path.empty());

    // Equals forms + fuzzy track + replay-implies-headless.
    CHECK(parseWords({"p", "--track=routes1", "--device=gm",
                      "--frames=600", "--out=r.wav", "--replay=t.audiolog"},
                     &o, &err));
    CHECK(o.track == "routes1");
    CHECK(o.device == "gm");
    CHECK(o.frames == 600u);
    CHECK(o.out_path == "r.wav");
    CHECK(o.replay_path == "t.audiolog");
    CHECK(o.headless);

    // --no-enh / --enhancement spellings.
    CHECK(parseWords({"p", "--no-enh"}, &o, &err) && o.no_enhancement);
    CHECK(parseWords({"p", "--no-enhancement"}, &o, &err) &&
          o.no_enhancement);
    CHECK(parseWords({"p", "--enhancement", "0"}, &o, &err) &&
          o.no_enhancement);
    CHECK(parseWords({"p", "--enhancement=off"}, &o, &err) &&
          o.no_enhancement);
    CHECK(parseWords({"p", "--enhancement=1"}, &o, &err) &&
          !o.no_enhancement);
    CHECK(parseWords({"p", "--headless", "--track", "pallet"}, &o, &err) &&
          o.headless && o.track == "pallet" && !o.no_enhancement);

    // --help.
    CHECK(parseWords({"p", "--help"}, &o, &err) && o.show_help);
    CHECK(parseWords({"p", "-h"}, &o, &err) && o.show_help);
    CHECK(cliUsage("pkmn-audio-dbg").find("--headless") != std::string::npos);
    CHECK(cliUsage("pkmn-audio-dbg").find("--replay") != std::string::npos);
    CHECK(cliUsage("pkmn-audio-dbg").find("--track") != std::string::npos);

    // Errors: unknown flag, missing value, bad frames, bad device,
    // positional, bad enhancement value.
    CHECK(!parseWords({"p", "--bogus"}, &o, &err) && !err.empty());
    CHECK(!parseWords({"p", "--track"}, &o, &err) && !err.empty());
    CHECK(!parseWords({"p", "--frames", "0"}, &o, &err) && !err.empty());
    CHECK(!parseWords({"p", "--frames=abc"}, &o, &err) && !err.empty());
    CHECK(!parseWords({"p", "--device", "scummvm"}, &o, &err) &&
          !err.empty());
    CHECK(!parseWords({"p", "positional.wav"}, &o, &err) && !err.empty());
    CHECK(!parseWords({"p", "--enhancement", "maybe"}, &o, &err) &&
          !err.empty());
    std::printf("PASS cli parsing\n");
  }

  // --- 2. Device normalization ---------------------------------------------
  {
    CHECK(normalizeDeviceName("mt32") == "mt32");
    CHECK(normalizeDeviceName("MT32") == "mt32");
    CHECK(normalizeDeviceName("gm") == "gm");
    CHECK(normalizeDeviceName("GM") == "gm");
    CHECK(normalizeDeviceName("opl3") == "opl3");
    CHECK(normalizeDeviceName("OPL3") == "opl3");
    CHECK(normalizeDeviceName("gbapu") == "gbapu");
    CHECK(normalizeDeviceName("GB-APU") == "gbapu");
    CHECK(normalizeDeviceName("gb_apu") == "gbapu");
    CHECK(normalizeDeviceName("scummvm").empty());
    CHECK(isKnownDevice("mt32") && isKnownDevice("gm") &&
          isKnownDevice("opl3") && isKnownDevice("gbapu"));
    CHECK(!isKnownDevice("fb01"));
    CHECK(deviceMidiTarget("mt32") == "mt32");
    CHECK(deviceMidiTarget("gm") == "gm");
    CHECK(deviceMidiTarget("opl3") == "mt32");
    CHECK(deviceMidiTarget("gbapu") == "mt32");
    std::uint32_t rate = 0;
    std::string canon, err;
    auto dev = createHeadlessDevice("GB-APU", &rate, &canon, &err);
    // GB-APU now defaults to 48000 Hz (the mixer's output rate) so it takes
    // the 1:1 passthrough path; Blip_Buffer's clock_rate is fixed at 4194304,
    // so pitch is unchanged.
    CHECK(dev != nullptr && canon == "gbapu" && rate == 48000u);
    CHECK(createHeadlessDevice("nope", &rate, &canon, &err) == nullptr);
    std::printf("PASS device normalization\n");
  }

  // --- 3. Frame/sample math -------------------------------------------------
  {
    // Exact-rate device: 44100/60 = 735 samples every frame.
    for (std::uint32_t f = 0; f < 60; ++f) {
      CHECK(frameSampleCount(44100, f) == 735u);
    }
    CHECK(totalSamplesFor(44100, 60) == 44100u);
    // Fractional rates distribute the remainder exactly.
    for (std::uint32_t rate : {32000u, 49716u, 48000u}) {
      std::uint64_t acc = 0;
      for (std::uint32_t f = 0; f < 600; ++f) {
        const std::size_t n = frameSampleCount(rate, f);
        CHECK(n == rate / 60 || n == rate / 60 + 1);
        acc += n;
      }
      CHECK(acc == totalSamplesFor(rate, 600));
    }
    std::printf("PASS frame/sample math\n");
  }

  // --- 4. Headless track render + determinism -------------------------------
  {
    constexpr std::uint32_t kFrames = 240;
    const std::string wa = tmpPath("cli_a.wav");
    const std::string wb = tmpPath("cli_b.wav");
    const std::string wc = tmpPath("cli_gb.wav");
    std::remove(wa.c_str());
    std::remove(wb.c_str());
    std::remove(wc.c_str());

    CliOptions o;
    o.headless = true;
    o.track = "MUSIC_PALLET_TOWN";
    o.device = "mt32";
    o.frames_explicit = true;
    o.frames = kFrames;
    o.out_path = wa;
    std::string err;
    HeadlessResult ra;
    CHECK(runHeadless(o, &err, &ra) == 0);
    CHECK(err.empty());
    CHECK(ra.frames_rendered == kFrames);
    CHECK(ra.sample_rate == Mt32Device::kDefaultRate);
    CHECK(ra.track == "MUSIC_PALLET_TOWN");
    CHECK(ra.device == "mt32");
    CHECK(ra.total_samples == totalSamplesFor(ra.sample_rate, kFrames));

    o.out_path = wb;
    HeadlessResult rb;
    CHECK(runHeadless(o, &err, &rb) == 0);
    CHECK(rb.total_samples == ra.total_samples);

    std::vector<char> ba, bb;
    CHECK(readFile(wa, &ba));
    CHECK(readFile(wb, &bb));
    CHECK(wavHeaderOk(ba, static_cast<int>(ra.sample_rate), 2));
    CHECK(rdU32(ba, 40) ==
          static_cast<std::uint32_t>(ra.total_samples * 2 * 4));
    CHECK(wavHasNonzero(ba));
    CHECK(ba.size() == bb.size());
    CHECK(ba == bb);  // Null difference: bit-deterministic across runs.
    std::printf("INFO headless mt32: %llu samples peak=%.4f nonzero=%llu\n",
                static_cast<unsigned long long>(ra.total_samples),
                static_cast<double>(ra.peak),
                static_cast<unsigned long long>(ra.nonzero_samples));

    // GB-baseline-only render is valid audio too.
    o.no_enhancement = true;
    o.out_path = wc;
    HeadlessResult rc;
    CHECK(runHeadless(o, &err, &rc) == 0);
    std::vector<char> bc;
    CHECK(readFile(wc, &bc));
    CHECK(wavHeaderOk(bc, static_cast<int>(rc.sample_rate), 2));
    CHECK(wavHasNonzero(bc));

    // Unknown track + missing track are hard errors (exit 1).
    o.track = "MUSIC_NO_SUCH_TRACK_XYZ";
    o.out_path = tmpPath("cli_bad.wav");
    HeadlessResult rx;
    CHECK(runHeadless(o, &err, &rx) == 1 && !err.empty());
    CliOptions no_track;
    no_track.headless = true;
    no_track.device = "mt32";
    CHECK(runHeadless(no_track, &err, &rx) == 1 && !err.empty());
    std::printf("PASS headless track render + determinism\n");
  }

  // --- 5. AudioLog record/replay parity --------------------------------------
  {
    constexpr std::uint32_t kRate = Mt32Device::kDefaultRate;
    constexpr std::uint32_t kFrames = 60;
    // Event stream: three overlapping notes on mt32-mapped channels.
    std::vector<SimNoteEvent> evs;
    auto addNote = [&](std::uint32_t f, std::uint8_t ch, std::uint8_t note,
                       std::uint8_t vel, std::uint16_t dur) {
      SimNoteEvent on;
      on.frame = f;
      on.channel = ch;
      on.note = note;
      on.velocity = vel;
      on.duration_frames = dur;
      on.is_note_on = true;
      evs.push_back(on);
      SimNoteEvent off;
      off.frame = f + dur;
      off.channel = ch;
      off.note = note;
      off.velocity = 0;
      off.duration_frames = 0;
      off.is_note_on = false;
      evs.push_back(off);
    };
    addNote(0, 1, 60, 100, 30);
    addNote(10, 2, 64, 90, 20);
    addNote(20, 1, 67, 80, 20);

    // Record the stream as an audiolog.
    const std::string log = tmpPath("cli.audiolog");
    std::remove(log.c_str());
    {
      AudioLogWriter w;
      CHECK(w.open(log));
      for (const SimNoteEvent& e : evs) {
        if (e.is_note_on) {
          const std::uint8_t p[3] = {e.channel, e.note, e.velocity};
          CHECK(w.writeEvent(e.frame, 0x90, p, 3));
        } else {
          const std::uint8_t p[2] = {e.channel, e.note};
          CHECK(w.writeEvent(e.frame, 0x80, p, 2));
        }
      }
      CHECK(w.close());
    }
    std::vector<AudioLogEvent> back;
    CHECK(AudioLogReader::readAll(log, &back));
    CHECK(back.size() == evs.size());

    // Replay twice: valid, non-silent, byte-identical.
    const std::string r1 = tmpPath("replay_a.wav");
    const std::string r2 = tmpPath("replay_b.wav");
    std::remove(r1.c_str());
    std::remove(r2.c_str());
    CliOptions ro;
    ro.headless = true;
    ro.device = "mt32";
    ro.replay_path = log;
    ro.frames_explicit = true;
    ro.frames = kFrames;
    ro.out_path = r1;
    std::string err;
    HeadlessResult rr1;
    CHECK(runReplayLog(ro, &err, &rr1) == 0);
    CHECK(err.empty());
    CHECK(rr1.frames_rendered == kFrames);
    CHECK(rr1.sample_rate == kRate);
    CHECK(rr1.total_samples == totalSamplesFor(kRate, kFrames));
    ro.out_path = r2;
    HeadlessResult rr2;
    CHECK(runReplayLog(ro, &err, &rr2) == 0);
    std::vector<char> ba, bb;
    CHECK(readFile(r1, &ba));
    CHECK(readFile(r2, &bb));
    CHECK(wavHeaderOk(ba, static_cast<int>(kRate), 2));
    CHECK(wavHasNonzero(ba));
    CHECK(ba == bb);

    // Direct SessionEngine render of the same stream must match the replay.
    const std::string direct = tmpPath("direct.wav");
    std::remove(direct.c_str());
    {
      Mt32Device dev;
      CHECK(dev.init());
      SessionEngine eng;
      eng.setActiveDevice(&dev);
      eng.setEvents(evs);
      eng.setTotalFrames(kFrames);
      eng.play();
      WavWriter wav;
      CHECK(wav.open(direct, static_cast<int>(kRate), 2, WavFormat::FLOAT32));
      std::vector<float> mono, stereo;
      for (std::uint32_t f = 0; f < kFrames; ++f) {
        eng.tick();
        const std::size_t n = frameSampleCount(kRate, f);
        mono.assign(n, 0.0f);
        if (n > 0) dev.render(mono.data(), n);
        stereo.resize(n * 2);
        for (std::size_t i = 0; i < n; ++i) {
          stereo[i * 2] = mono[i];
          stereo[i * 2 + 1] = mono[i];
        }
        if (n > 0) CHECK(wav.writeFrames(stereo.data(), n));
      }
      wav.close();
      dev.shutdown();
    }
    std::vector<char> bd;
    CHECK(readFile(direct, &bd));
    CHECK(bd == ba);  // Record/replay parity: null difference.
    std::printf("PASS audiolog record/replay parity\n");
  }

  if (g_failures == 0) {
    std::printf("ALL PASS (%d checks)\n", g_checks);
    return 0;
  }
  std::printf("%d/%d checks FAILED\n", g_failures, g_checks);
  return 1;
}
