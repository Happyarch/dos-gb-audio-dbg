// Stage 3.5: Acceptance tests for the audio mixer + multitrack recorder.
// Verifies:
//   1. WAV header generation & file writing (RIFF/WAVE/fmt/data, sizes).
//   2. Multitrack stem null-sum (sum of stems == master within float eps).
//   3. AudioLog write & readback round-trip (frame/opcode/payload order).
//   4. AudioMixer thread-safe mute/gain atomics + headless render (incl.
//      44.1k -> 48k resample, scope ring, recorder feed, thread hammer).
//   5. resampleLinear unit checks (constant + ramp endpoints).
// Headless: mixer runs init(..., disabled=true), no SDL audio hardware.
// Exits 0 with ALL PASS, 1 on any failure.

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "audio_mixer.h"
#include "devices/sound_device.h"
#include "recorder.h"

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

// --- Mock synth ----------------------------------------------------------

class MockSynth : public audio_dbg::SoundDevice {
 public:
  explicit MockSynth(int channels = 4)
      : SoundDevice(9, "MockSynth", channels), mono_value_(0.5f) {}
  bool init() override { return true; }
  void shutdown() override {}
  void reset() override {}
  void render(float* buf, std::size_t frames) override {
    ++render_calls;
    if (buf == nullptr) return;
    for (std::size_t i = 0; i < frames; ++i) buf[i] = mono_value_;
  }
  void renderPerChannel(float** bufs, std::size_t frames) override {
    ++render_ch_calls;
    if (bufs == nullptr) return;
    for (int c = 0; c < channelCount(); ++c) {
      if (bufs[c] == nullptr) continue;
      const float v = static_cast<float>(c + 1) * 0.1f;
      for (std::size_t i = 0; i < frames; ++i) bufs[c][i] = v;
    }
  }
  void setMute(int ch, bool m) override { setMutedState(ch, m); }
  void setSolo(int ch, bool s) override { setSoloedState(ch, s); }
  void tick(std::uint32_t f) override { setFrame(f); }
  void handleCommand(std::uint8_t, const std::uint8_t*, std::size_t) override {}
  audio_dbg::DeviceSnapshot snapshot() const override { return makeSnapshot(); }
  float mono_value_ = 0.5f;
  int render_calls = 0;
  int render_ch_calls = 0;
};

// --- WAV helpers ---------------------------------------------------------

std::uint16_t rdU16(const std::vector<char>& d, std::size_t o) {
  return static_cast<std::uint16_t>(
      static_cast<std::uint8_t>(d[o]) |
      (static_cast<std::uint16_t>(static_cast<std::uint8_t>(d[o + 1])) << 8));
}

std::uint32_t rdU32(const std::vector<char>& d, std::size_t o) {
  return static_cast<std::uint32_t>(static_cast<std::uint8_t>(d[o])) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(d[o + 1]))
          << 8) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(d[o + 2]))
          << 16) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(d[o + 3]))
          << 24);
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
  return std::string("/tmp/opencode/pkmn-audio-stage3-") + name;
}

}  // namespace

int main() {
  using namespace audio_dbg;
  std::system("mkdir -p /tmp/opencode");

  // --- 1. WAV header generation & file writing ---------------------------
  {
    const std::vector<std::uint8_t> h =
        WavWriter::buildHeader(48000, 2, WavFormat::FLOAT32, 192000);
    CHECK(h.size() == 44u);
    CHECK(h[0] == 'R' && h[1] == 'I' && h[2] == 'F' && h[3] == 'F');
    CHECK(h[8] == 'W' && h[9] == 'A' && h[10] == 'V' && h[11] == 'E');
    CHECK(h[12] == 'f' && h[13] == 'm' && h[14] == 't' && h[15] == ' ');
    // audioFormat float32 == 3, channels 2, rate 48000, bits 32.
    CHECK(h[20] == 3 && h[21] == 0);
    CHECK(h[22] == 2 && h[23] == 0);
    const std::uint32_t rate = static_cast<std::uint32_t>(h[24]) |
                               (static_cast<std::uint32_t>(h[25]) << 8) |
                               (static_cast<std::uint32_t>(h[26]) << 16) |
                               (static_cast<std::uint32_t>(h[27]) << 24);
    CHECK(rate == 48000u);
    CHECK(h[34] == 32 && h[35] == 0);
    CHECK(h[36] == 'd' && h[37] == 'a' && h[38] == 't' && h[39] == 'a');
    const std::uint32_t riff = static_cast<std::uint32_t>(h[4]) |
                               (static_cast<std::uint32_t>(h[5]) << 8) |
                               (static_cast<std::uint32_t>(h[6]) << 16) |
                               (static_cast<std::uint32_t>(h[7]) << 24);
    CHECK(riff == 36u + 192000u);
    // PCM16 header: format 1, 16 bits, mono.
    const std::vector<std::uint8_t> h16 =
        WavWriter::buildHeader(44100, 1, WavFormat::PCM16, 100);
    CHECK(h16[20] == 1 && h16[34] == 16);
    CHECK(WavWriter::bytesPerSample(WavFormat::PCM16) == 2);
    CHECK(WavWriter::bytesPerSample(WavFormat::FLOAT32) == 4);

    // File round-trip: 256 stereo float frames of a ramp.
    const std::string path = tmpPath("hdr.wav");
    std::remove(path.c_str());
    WavWriter w;
    CHECK(w.open(path, 48000, 2, WavFormat::FLOAT32));
    CHECK(w.isOpen());
    std::vector<float> pcm(256 * 2);
    for (std::size_t i = 0; i < 256; ++i) {
      pcm[i * 2] = static_cast<float>(i) / 256.0f;
      pcm[i * 2 + 1] = -static_cast<float>(i) / 256.0f;
    }
    CHECK(w.writeFrames(pcm.data(), 256));
    CHECK(w.framesWritten() == 256u);
    CHECK(w.close());
    std::vector<char> blob;
    CHECK(readFile(path, &blob));
    CHECK(blob.size() == 44u + 256u * 2u * 4u);
    CHECK(blob[0] == 'R' && blob[8] == 'W');
    const std::uint32_t datasz = rdU32(blob, 40);
    CHECK(datasz == 256u * 2u * 4u);
    // First payload float == 0.0 (L of frame 0).
    float first = 0.0f;
    std::memcpy(&first, blob.data() + 44, 4);
    CHECK(first == 0.0f);
    // PCM16 file: clamps + converts.
    const std::string path16 = tmpPath("hdr16.wav");
    std::remove(path16.c_str());
    WavWriter w16;
    CHECK(w16.open(path16, 44100, 1, WavFormat::PCM16));
    const float mono[4] = {0.0f, 1.0f, -1.0f, 0.5f};
    CHECK(w16.writeFrames(mono, 4));
    CHECK(w16.close());
    std::vector<char> blob16;
    CHECK(readFile(path16, &blob16));
    CHECK(blob16.size() == 44u + 4u * 2u);
    CHECK(rdU16(blob16, 20) == 1u);  // PCM format tag.
    const std::int16_t peak =
        static_cast<std::int16_t>(rdU16(blob16, 44 + 2));
    CHECK(peak == 32767);
    std::printf("PASS wav header + file writing\n");
  }

  // --- 2. Multitrack stem null-sum ---------------------------------------
  {
    const std::string prefix = tmpPath("stems");
    std::remove((prefix + "_master.wav").c_str());
    for (int c = 0; c < 4; ++c) {
      char tmp[16];
      std::snprintf(tmp, sizeof(tmp), "_ch%02d.wav", c);
      std::remove((prefix + tmp).c_str());
    }
    Recorder rec;
    CHECK(rec.startStems(prefix, 4, 48000, WavFormat::FLOAT32));
    CHECK(rec.isRecording());
    CHECK(rec.mode() == Recorder::Mode::STEMS);
    CHECK(rec.stemChannels() == 4);
    constexpr std::size_t kFrames = 128;
    std::vector<std::vector<float> > stems(4, std::vector<float>(kFrames));
    for (int c = 0; c < 4; ++c) {
      for (std::size_t i = 0; i < kFrames; ++i) {
        stems[c][i] =
            static_cast<float>(c + 1) * 0.1f + static_cast<float>(i) * 0.001f;
      }
    }
    std::array<const float*, 4> ptrs = {stems[0].data(), stems[1].data(),
                                        stems[2].data(), stems[3].data()};
    rec.pushStems(ptrs.data(), 4, kFrames);
    rec.pushStems(ptrs.data(), 4, kFrames);
    rec.stop();
    CHECK(!rec.isRecording());
    // Read back master + stems, compare sum(stems) == master L == master R.
    std::vector<char> master;
    CHECK(readFile(prefix + "_master.wav", &master));
    CHECK(master.size() == 44u + 2u * kFrames * 2u * 4u);
    std::vector<std::vector<float> > back(4);
    for (int c = 0; c < 4; ++c) {
      char tmp[16];
      std::snprintf(tmp, sizeof(tmp), "_ch%02d.wav", c);
      std::vector<char> sb;
      CHECK(readFile(prefix + tmp, &sb));
      CHECK(sb.size() == 44u + 2u * kFrames * 4u);
      back[c].assign(2 * kFrames, 0.0f);
      std::memcpy(back[c].data(), sb.data() + 44, 2 * kFrames * 4);
    }
    std::vector<float> mback(2 * kFrames * 2, 0.0f);
    std::memcpy(mback.data(), master.data() + 44, 2 * kFrames * 2 * 4);
    float worst = 0.0f;
    for (std::size_t i = 0; i < 2 * kFrames; ++i) {
      float s = 0.0f;
      for (int c = 0; c < 4; ++c) s += back[c][i];
      const float ml = mback[i * 2];
      const float mr = mback[i * 2 + 1];
      worst = std::max(worst, std::fabs(s - ml));
      worst = std::max(worst, std::fabs(s - mr));
      CHECK(std::fabs(ml - mr) < 1e-7f);
    }
    CHECK(worst < 1e-5f);
    // Master-only mode writes a plain stereo file.
    const std::string mpath = tmpPath("master_only.wav");
    std::remove(mpath.c_str());
    Recorder r2;
    CHECK(r2.startMaster(mpath, 48000, WavFormat::FLOAT32));
    std::vector<float> stereo(kFrames * 2, 0.25f);
    r2.pushMaster(stereo.data(), kFrames);
    r2.stop();
    std::vector<char> mb;
    CHECK(readFile(mpath, &mb));
    CHECK(mb.size() == 44u + kFrames * 2u * 4u);
    std::printf("PASS multitrack stem null-sum (worst=%.2e)\n", worst);
  }

  // --- 3. AudioLog write & readback round-trip ---------------------------
  {
    const std::string path = tmpPath("events.audiolog");
    std::remove(path.c_str());
    AudioLogWriter w;
    CHECK(w.open(path));
    const std::uint8_t p0[] = {0x90, 60, 100};
    const std::uint8_t p1[] = {0x05};
    CHECK(w.writeEvent(0, 0x90, p0, sizeof(p0)));
    CHECK(w.writeEvent(7, 0x80, nullptr, 0));  // Empty payload.
    CHECK(w.writeEvent(123456, 0xB0, p1, sizeof(p1)));
    std::vector<std::uint8_t> big(300, 0xAB);
    CHECK(w.writeEvent(999, 0xF0, big.data(), big.size()));
    CHECK(w.close());
    // Magic check on disk.
    std::vector<char> blob;
    CHECK(readFile(path, &blob));
    CHECK(blob.size() > 12u);
    CHECK(blob[0] == 'A' && blob[1] == 'U' && blob[2] == 'D' &&
          blob[3] == 'I' && blob[4] == 'O' && blob[5] == 'L' &&
          blob[6] == 'O' && blob[7] == 'G');
    CHECK(static_cast<std::uint8_t>(blob[8]) == 1);  // version 1.
    std::vector<AudioLogEvent> evs;
    CHECK(AudioLogReader::readAll(path, &evs));
    CHECK(evs.size() == 4u);
    CHECK(evs[0].frame == 0u && evs[0].opcode == 0x90u);
    CHECK(evs[0].payload.size() == 3u && evs[0].payload[1] == 60);
    CHECK(evs[1].frame == 7u && evs[1].payload.empty());
    CHECK(evs[2].frame == 123456u && evs[2].payload.size() == 1u &&
          evs[2].payload[0] == 0x05);
    CHECK(evs[3].frame == 999u && evs[3].payload.size() == 300u &&
          evs[3].payload[0] == 0xABu);
    // Sequential readNext hits EOF cleanly.
    AudioLogReader r;
    CHECK(r.open(path));
    AudioLogEvent one;
    int n = 0;
    while (r.readNext(&one)) ++n;
    CHECK(n == 4);
    CHECK(!r.readNext(&one));
    r.close();
    // Bad magic rejected.
    const std::string bad = tmpPath("bad.audiolog");
    {
      std::ofstream o(bad, std::ios::binary);
      o.write("NOTALOG!", 8);
    }
    AudioLogReader rb;
    CHECK(!rb.open(bad));
    std::printf("PASS audiolog round-trip\n");
  }

  // --- 4. AudioMixer atomics + headless render ---------------------------
  {
    AudioMixer mixer;
    CHECK(mixer.init(48000, 512, true));  // Headless: no SDL hardware.
    CHECK(mixer.isOpen() && mixer.isDisabled());
    MockSynth synth(4);
    mixer.setDevice(&synth, 44100);  // Synth 44.1k -> out 48k resample.
    CHECK(mixer.device() == &synth);
    CHECK(mixer.deviceRate() == 44100);
    mixer.setMasterGain(1.0f);
    mixer.setMasterMuted(false);
    CHECK(mixer.masterGain() == 1.0f && !mixer.masterMuted());

    std::vector<float> stereo(512 * 2, 0.0f);
    mixer.renderBlock(stereo.data(), 512);
    CHECK(synth.render_calls == 1);
    // Constant 0.5 synth -> constant 0.5 stereo (resampled constant).
    CHECK(std::fabs(stereo[0] - 0.5f) < 1e-4f);
    CHECK(std::fabs(stereo[1] - 0.5f) < 1e-4f);
    CHECK(mixer.masterWaveSize() >= 512u);
    std::vector<float> scope(16, 0.0f);
    CHECK(mixer.copyMasterWaveform(scope.data(), 16) == 16u);

    // Gain scales.
    mixer.setMasterGain(0.5f);
    mixer.renderBlock(stereo.data(), 64);
    CHECK(std::fabs(stereo[0] - 0.25f) < 1e-4f);
    // Mute silences (and still feeds ring/recorder without crashing).
    mixer.setMasterMuted(true);
    mixer.renderBlock(stereo.data(), 64);
    CHECK(stereo[0] == 0.0f && stereo[3] == 0.0f);
    mixer.setMasterMuted(false);
    mixer.setMasterGain(1.0f);

    // Device/channel mute/solo forwarding (no crash, state lands).
    mixer.setDeviceChannelMute(1, true);
    CHECK(synth.snapshot().channels[1].muted);
    mixer.setDeviceChannelSolo(2, true);
    CHECK(synth.snapshot().channels[2].soloed);
    mixer.setDeviceChannelMute(1, false);
    mixer.setDeviceChannelSolo(2, false);

    // Stems path: master == sum of per-channel mocks (0.1+0.2+0.3+0.4).
    mixer.setDevice(&synth, 48000);  // No resample for exact check.
    mixer.renderStemsBlock(stereo.data(), 64);
    CHECK(synth.render_ch_calls >= 1);
    CHECK(std::fabs(stereo[0] - 1.0f) < 1e-4f);
    CHECK(std::fabs(stereo[1] - 1.0f) < 1e-4f);

    // Recorder feed: master mode captures mixer output.
    const std::string mpath = tmpPath("mixer_feed.wav");
    std::remove(mpath.c_str());
    Recorder rec;
    CHECK(rec.startMaster(mpath, 48000, WavFormat::FLOAT32));
    mixer.setRecorder(&rec);
    mixer.renderBlock(stereo.data(), 64);
    mixer.setRecorder(nullptr);
    rec.stop();
    std::vector<char> mb;
    CHECK(readFile(mpath, &mb));
    CHECK(mb.size() == 44u + 64u * 2u * 4u);

    // Null device renders silence without crashing.
    mixer.setDevice(nullptr, 48000);
    mixer.renderBlock(stereo.data(), 32);
    CHECK(stereo[0] == 0.0f);
    mixer.renderStemsBlock(stereo.data(), 32);
    CHECK(stereo[0] == 0.0f);
    mixer.setDevice(&synth, 48000);

    // Thread hammer: gain/mute/device swaps racing renderBlock.
    mixer.setMasterGain(1.0f);
    mixer.setMasterMuted(false);
    std::atomic<bool> stop{false};
    std::thread toggler([&]() {
      for (int i = 0; i < 2000; ++i) {
        mixer.setMasterGain((i % 2 == 0) ? 1.0f : 0.25f);
        mixer.setMasterMuted((i % 3) == 0);
        mixer.setDeviceChannelMute(0, (i % 2) == 0);
        if ((i % 500) == 0) mixer.setDevice(&synth, 44100);
      }
      stop.store(true);
    });
    std::vector<float> blk(128 * 2);
    int spins = 0;
    while (!stop.load() && spins < 5000) {
      mixer.renderBlock(blk.data(), 128);
      for (float v : blk) CHECK(std::isfinite(v));
      ++spins;
    }
    toggler.join();
    mixer.shutdown();
    CHECK(!mixer.isOpen());
    std::printf("PASS mixer atomics + headless render\n");
  }

  // --- 5. resampleLinear ---------------------------------------------------
  {
    // Constant in -> constant out across a rate change.
    const float cin[8] = {0.7f, 0.7f, 0.7f, 0.7f,
                          0.7f, 0.7f, 0.7f, 0.7f};
    float cout[16] = {0};
    AudioMixer::resampleLinear(cin, 8, cout, 16);
    for (float v : cout) CHECK(std::fabs(v - 0.7f) < 1e-6f);
    // Ramp endpoints preserved.
    const float ramp[4] = {0.0f, 1.0f, 2.0f, 3.0f};
    float r8[8] = {0};
    AudioMixer::resampleLinear(ramp, 4, r8, 8);
    CHECK(std::fabs(r8[0] - 0.0f) < 1e-6f);
    CHECK(std::fabs(r8[7] - 3.0f) < 1e-6f);
    CHECK(r8[3] > r8[2] && r8[4] > r8[3]);  // Monotone.
    // Degenerate: empty in -> silence; null-safe.
    float z[4] = {9.0f, 9.0f, 9.0f, 9.0f};
    AudioMixer::resampleLinear(nullptr, 0, z, 4);
    for (float v : z) CHECK(v == 0.0f);
    AudioMixer::resampleLinear(ramp, 4, nullptr, 0);  // No crash.
    std::printf("PASS resampleLinear\n");
  }

  if (g_failures == 0) {
    std::printf("ALL PASS (%d checks)\n", g_checks);
    return 0;
  }
  std::printf("%d/%d checks FAILED\n", g_failures, g_checks);
  return 1;
}
