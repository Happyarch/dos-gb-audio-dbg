// Stage 3.3-3.4: Multitrack WAV recorder + raw AudioLog capture.
//
// `WavWriter` writes standard RIFF WAV (float32 or 16-bit PCM, mono/stereo)
// with a patch-on-close header. `Recorder` owns one master writer plus one
// writer per stem; in STEMS mode pushStems() derives the master as the exact
// sum of the pushed stems so the null-sum property (sum(stems) == master)
// holds within float rounding. `AudioLogWriter`/`AudioLogReader` capture and
// replay the raw (frame, opcode, payload) event stream for deterministic
// replay: header magic 'AUDIOLOG' + version 1, then length-prefixed records.
//
// Threading: pushMaster()/pushStems() are called from the audio thread;
// start/stop come from the UI thread. A mutex guards file handles; an atomic
// flag publishes the recording state. File I/O on the audio thread is
// best-effort blocking (Stage 3 scope); drops never touch the live mix.
//
// See docs/current_plan_debug_frontend.md §3.3 / §3.4.

#ifndef PKMN_AUDIO_DBG_RECORDER_H_
#define PKMN_AUDIO_DBG_RECORDER_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace audio_dbg {

enum class WavFormat {
  FLOAT32 = 0,
  PCM16 = 1,
};

class WavWriter {
 public:
  WavWriter();
  ~WavWriter();

  WavWriter(const WavWriter&) = delete;
  WavWriter& operator=(const WavWriter&) = delete;

  bool open(const std::string& path, int sample_rate, int channels,
            WavFormat fmt);
  // Interleaved float input (frames * channels samples); converts to PCM16
  // when opened as such. No-op (false) when closed.
  bool writeFrames(const float* interleaved, std::size_t frames);
  bool close();

  bool isOpen() const { return open_; }
  const std::string& path() const { return path_; }
  int sampleRate() const { return sample_rate_; }
  int channels() const { return channels_; }
  WavFormat format() const { return format_; }
  std::size_t framesWritten() const { return frames_written_; }

  static int bytesPerSample(WavFormat fmt) { return fmt == WavFormat::PCM16 ? 2 : 4; }
  // 44-byte RIFF header for `data_bytes` payload bytes. Pure helper so
  // tests can verify header fields without touching disk.
  static std::vector<std::uint8_t> buildHeader(int sample_rate, int channels,
                                               WavFormat fmt,
                                               std::uint32_t data_bytes);

 private:
  bool writeHeaderPlaceholder();
  void patchHeader();

  std::ofstream out_;
  std::string path_;
  int sample_rate_ = 0;
  int channels_ = 0;
  WavFormat format_ = WavFormat::FLOAT32;
  std::size_t frames_written_ = 0;
  bool open_ = false;
};

// One deterministic event: the session-engine frame, the command opcode,
// and the raw payload bytes.
struct AudioLogEvent {
  std::uint32_t frame = 0;
  std::uint8_t opcode = 0;
  std::vector<std::uint8_t> payload;
};

class AudioLogWriter {
 public:
  static constexpr std::uint16_t kVersion = 1;

  AudioLogWriter();
  ~AudioLogWriter();

  AudioLogWriter(const AudioLogWriter&) = delete;
  AudioLogWriter& operator=(const AudioLogWriter&) = delete;

  bool open(const std::string& path);
  bool writeEvent(std::uint32_t frame, std::uint8_t opcode,
                  const std::uint8_t* payload, std::size_t len);
  bool close();
  bool isOpen() const { return open_; }
  const std::string& path() const { return path_; }

 private:
  std::ofstream out_;
  std::string path_;
  bool open_ = false;
};

class AudioLogReader {
 public:
  AudioLogReader();
  ~AudioLogReader();

  AudioLogReader(const AudioLogReader&) = delete;
  AudioLogReader& operator=(const AudioLogReader&) = delete;

  bool open(const std::string& path);
  // False on EOF or error. `out` is cleared on EOF/error.
  bool readNext(AudioLogEvent* out);
  bool close();
  bool isOpen() const { return open_; }

  static bool readAll(const std::string& path,
                      std::vector<AudioLogEvent>* out);

 private:
  std::ifstream in_;
  bool open_ = false;
};

class Recorder {
 public:
  enum class Mode {
    MASTER_ONLY = 0,
    STEMS = 1,
  };

  Recorder();
  ~Recorder();

  Recorder(const Recorder&) = delete;
  Recorder& operator=(const Recorder&) = delete;

  // Master-only: writes `master_path` (stereo expected, any channel count
  // the writer was opened with is honoured).
  bool startMaster(const std::string& master_path, int sample_rate,
                   WavFormat fmt = WavFormat::FLOAT32);
  // Stems: writes `<prefix>_master.wav` (stereo) plus one mono stem per
  // channel (`<prefix>_ch00.wav`, …). `num_channels` must be > 0.
  bool startStems(const std::string& prefix, int num_channels, int sample_rate,
                  WavFormat fmt = WavFormat::FLOAT32);
  // Master-only path: forwards stereo-interleaved blocks. Ignored unless
  // recording in MASTER_ONLY mode (stems mode writes its master via
  // pushStems so the null-sum holds by construction).
  void pushMaster(const float* stereo_interleaved, std::size_t frames);
  // Stems path: `channel_mono[c]` holds `frames` mono samples each. The
  // master stereo block is computed as L == R == sum_c ch[c] (same order
  // every block) and both master + stems are written together.
  void pushStems(const float* const* channel_mono, int channels,
                 std::size_t frames);
  void stop();

  bool isRecording() const { return recording_.load(); }
  Mode mode() const;
  int stemChannels() const;
  int sampleRate() const;
  WavFormat format() const;
  const std::string& masterPath() const { return master_path_; }
  std::string stemPath(int ch) const;

 private:
  mutable std::mutex mutex_;
  std::atomic<bool> recording_{false};
  Mode mode_ = Mode::MASTER_ONLY;
  int sample_rate_ = 0;
  WavFormat format_ = WavFormat::FLOAT32;
  std::string master_path_;
  std::string prefix_;
  WavWriter master_;
  std::vector<WavWriter> stems_;
};

}  // namespace audio_dbg

#endif  // PKMN_AUDIO_DBG_RECORDER_H_
