// Stage 3.3-3.4: Recorder implementation. See recorder.h.

#include "recorder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace audio_dbg {

namespace {

void putU16LE(std::vector<std::uint8_t>* v, std::uint16_t x) {
  v->push_back(static_cast<std::uint8_t>(x & 0xFF));
  v->push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
}

void putU32LE(std::vector<std::uint8_t>* v, std::uint32_t x) {
  v->push_back(static_cast<std::uint8_t>(x & 0xFF));
  v->push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
  v->push_back(static_cast<std::uint8_t>((x >> 16) & 0xFF));
  v->push_back(static_cast<std::uint8_t>((x >> 24) & 0xFF));
}

void putBytes(std::vector<std::uint8_t>* v, const char* s, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) v->push_back(static_cast<std::uint8_t>(s[i]));
}

float clamp1(float x) {
  if (x < -1.0f) return -1.0f;
  if (x > 1.0f) return 1.0f;
  return x;
}

constexpr char kAudioLogMagic[8] = {'A', 'U', 'D', 'I', 'O', 'L', 'O', 'G'};

}  // namespace

// --- WavWriter ------------------------------------------------------------

WavWriter::WavWriter() = default;

WavWriter::~WavWriter() { close(); }

std::vector<std::uint8_t> WavWriter::buildHeader(int sample_rate, int channels,
                                                 WavFormat fmt,
                                                 std::uint32_t data_bytes) {
  if (sample_rate <= 0) sample_rate = 48000;
  if (channels <= 0) channels = 1;
  const int bits = (fmt == WavFormat::PCM16) ? 16 : 32;
  const std::uint16_t audio_fmt = (fmt == WavFormat::PCM16) ? 1 : 3;
  const std::uint32_t byte_rate =
      static_cast<std::uint32_t>(sample_rate * channels * bits / 8);
  const std::uint16_t block_align =
      static_cast<std::uint16_t>(channels * bits / 8);
  std::vector<std::uint8_t> h;
  h.reserve(44);
  putBytes(&h, "RIFF", 4);
  putU32LE(&h, 36 + data_bytes);
  putBytes(&h, "WAVE", 4);
  putBytes(&h, "fmt ", 4);
  putU32LE(&h, 16);
  putU16LE(&h, audio_fmt);
  putU16LE(&h, static_cast<std::uint16_t>(channels));
  putU32LE(&h, static_cast<std::uint32_t>(sample_rate));
  putU32LE(&h, byte_rate);
  putU16LE(&h, block_align);
  putU16LE(&h, static_cast<std::uint16_t>(bits));
  putBytes(&h, "data", 4);
  putU32LE(&h, data_bytes);
  return h;
}

bool WavWriter::writeHeaderPlaceholder() {
  const std::vector<std::uint8_t> h =
      buildHeader(sample_rate_, channels_, format_, 0);
  out_.write(reinterpret_cast<const char*>(h.data()),
             static_cast<std::streamsize>(h.size()));
  return static_cast<bool>(out_);
}

void WavWriter::patchHeader() {
  if (path_.empty()) return;
  const std::uint64_t data_bytes =
      static_cast<std::uint64_t>(frames_written_) *
      static_cast<std::uint64_t>(channels_) *
      static_cast<std::uint64_t>(bytesPerSample(format_));
  const std::uint32_t clamped =
      data_bytes > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<std::uint32_t>(data_bytes);
  const std::vector<std::uint8_t> h =
      buildHeader(sample_rate_, channels_, format_, clamped);
  // Re-open read/write and overwrite the first 44 bytes.
  std::fstream patch(path_,
                     std::ios::in | std::ios::out | std::ios::binary);
  if (!patch) return;
  patch.write(reinterpret_cast<const char*>(h.data()),
              static_cast<std::streamsize>(h.size()));
}

bool WavWriter::open(const std::string& path, int sample_rate, int channels,
                     WavFormat fmt) {
  close();
  if (path.empty() || sample_rate <= 0 || channels <= 0) return false;
  if (channels > 64) return false;
  path_ = path;
  sample_rate_ = sample_rate;
  channels_ = channels;
  format_ = fmt;
  frames_written_ = 0;
  out_.open(path, std::ios::binary | std::ios::trunc);
  if (!out_) {
    open_ = false;
    return false;
  }
  if (!writeHeaderPlaceholder()) {
    out_.close();
    open_ = false;
    return false;
  }
  open_ = true;
  return true;
}

bool WavWriter::writeFrames(const float* interleaved, std::size_t frames) {
  if (!open_ || interleaved == nullptr || frames == 0) return false;
  const std::size_t total = frames * static_cast<std::size_t>(channels_);
  if (format_ == WavFormat::PCM16) {
    std::vector<std::int16_t> tmp(total);
    for (std::size_t i = 0; i < total; ++i) {
      const float c = clamp1(interleaved[i]);
      tmp[i] = static_cast<std::int16_t>(std::lround(c * 32767.0f));
    }
    out_.write(reinterpret_cast<const char*>(tmp.data()),
               static_cast<std::streamsize>(total * sizeof(std::int16_t)));
  } else {
    out_.write(reinterpret_cast<const char*>(interleaved),
               static_cast<std::streamsize>(total * sizeof(float)));
  }
  if (!out_) return false;
  frames_written_ += frames;
  return true;
}

bool WavWriter::close() {
  if (!open_) {
    path_.clear();
    frames_written_ = 0;
    return true;
  }
  out_.close();
  patchHeader();
  open_ = false;
  return true;
}

// --- AudioLogWriter -------------------------------------------------------

AudioLogWriter::AudioLogWriter() = default;

AudioLogWriter::~AudioLogWriter() { close(); }

bool AudioLogWriter::open(const std::string& path) {
  close();
  if (path.empty()) return false;
  path_ = path;
  out_.open(path, std::ios::binary | std::ios::trunc);
  if (!out_) return false;
  out_.write(kAudioLogMagic, 8);
  const std::uint8_t ver[4] = {
      static_cast<std::uint8_t>(kVersion & 0xFF),
      static_cast<std::uint8_t>((kVersion >> 8) & 0xFF),
      0,
      0,
  };
  out_.write(reinterpret_cast<const char*>(ver), 4);
  if (!out_) {
    out_.close();
    return false;
  }
  open_ = true;
  return true;
}

bool AudioLogWriter::writeEvent(std::uint32_t frame, std::uint8_t opcode,
                                const std::uint8_t* payload,
                                std::size_t len) {
  if (!open_) return false;
  if (len > 0xFFFF) return false;
  if (len > 0 && payload == nullptr) return false;
  std::uint8_t rec[7];
  rec[0] = static_cast<std::uint8_t>(frame & 0xFF);
  rec[1] = static_cast<std::uint8_t>((frame >> 8) & 0xFF);
  rec[2] = static_cast<std::uint8_t>((frame >> 16) & 0xFF);
  rec[3] = static_cast<std::uint8_t>((frame >> 24) & 0xFF);
  rec[4] = opcode;
  rec[5] = static_cast<std::uint8_t>(len & 0xFF);
  rec[6] = static_cast<std::uint8_t>((len >> 8) & 0xFF);
  out_.write(reinterpret_cast<const char*>(rec), 7);
  if (len > 0) {
    out_.write(reinterpret_cast<const char*>(payload),
               static_cast<std::streamsize>(len));
  }
  return static_cast<bool>(out_);
}

bool AudioLogWriter::close() {
  if (!open_) {
    path_.clear();
    return true;
  }
  out_.close();
  open_ = false;
  return true;
}

// --- AudioLogReader -------------------------------------------------------

AudioLogReader::AudioLogReader() = default;

AudioLogReader::~AudioLogReader() { close(); }

bool AudioLogReader::open(const std::string& path) {
  close();
  if (path.empty()) return false;
  in_.open(path, std::ios::binary);
  if (!in_) return false;
  char magic[8] = {0};
  in_.read(magic, 8);
  if (!in_) {
    in_.close();
    return false;
  }
  for (int i = 0; i < 8; ++i) {
    if (magic[i] != kAudioLogMagic[i]) {
      in_.close();
      return false;
    }
  }
  std::uint8_t hdr[4] = {0};
  in_.read(reinterpret_cast<char*>(hdr), 4);
  if (!in_) {
    in_.close();
    return false;
  }
  const std::uint16_t version =
      static_cast<std::uint16_t>(hdr[0] | (hdr[1] << 8));
  if (version != AudioLogWriter::kVersion) {
    in_.close();
    return false;
  }
  open_ = true;
  return true;
}

bool AudioLogReader::readNext(AudioLogEvent* out) {
  if (!open_ || out == nullptr) return false;
  std::uint8_t rec[7] = {0};
  in_.read(reinterpret_cast<char*>(rec), 7);
  if (in_.gcount() == 0 && in_.eof()) {
    out->frame = 0;
    out->opcode = 0;
    out->payload.clear();
    return false;  // Clean EOF.
  }
  if (!in_) {
    out->payload.clear();
    return false;
  }
  const std::uint32_t frame = static_cast<std::uint32_t>(rec[0]) |
                              (static_cast<std::uint32_t>(rec[1]) << 8) |
                              (static_cast<std::uint32_t>(rec[2]) << 16) |
                              (static_cast<std::uint32_t>(rec[3]) << 24);
  const std::uint16_t len =
      static_cast<std::uint16_t>(rec[5] | (rec[6] << 8));
  std::vector<std::uint8_t> payload(len);
  if (len > 0) {
    in_.read(reinterpret_cast<char*>(payload.data()), len);
    if (!in_) {
      out->payload.clear();
      return false;
    }
  }
  out->frame = frame;
  out->opcode = rec[4];
  out->payload = std::move(payload);
  return true;
}

bool AudioLogReader::close() {
  if (open_) in_.close();
  open_ = false;
  return true;
}

bool AudioLogReader::readAll(const std::string& path,
                             std::vector<AudioLogEvent>* out) {
  if (out == nullptr) return false;
  out->clear();
  AudioLogReader r;
  if (!r.open(path)) return false;
  AudioLogEvent ev;
  while (r.readNext(&ev)) out->push_back(ev);
  r.close();
  return true;
}

// --- Recorder -------------------------------------------------------------

Recorder::Recorder() = default;

Recorder::~Recorder() { stop(); }

Recorder::Mode Recorder::mode() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return mode_;
}

int Recorder::stemChannels() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<int>(stems_.size());
}

int Recorder::sampleRate() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return sample_rate_;
}

WavFormat Recorder::format() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return format_;
}

std::string Recorder::stemPath(int ch) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (ch < 0 || static_cast<std::size_t>(ch) >= stems_.size()) return {};
  return stems_[static_cast<std::size_t>(ch)].path();
}

bool Recorder::startMaster(const std::string& master_path, int sample_rate,
                           WavFormat fmt) {
  if (master_path.empty() || sample_rate <= 0) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  if (recording_.load()) return false;
  stems_.clear();
  prefix_.clear();
  if (!master_.open(master_path, sample_rate, 2, fmt)) return false;
  master_path_ = master_path;
  sample_rate_ = sample_rate;
  format_ = fmt;
  mode_ = Mode::MASTER_ONLY;
  recording_.store(true);
  return true;
}

bool Recorder::startStems(const std::string& prefix, int num_channels,
                          int sample_rate, WavFormat fmt) {
  if (prefix.empty() || num_channels <= 0 || sample_rate <= 0) return false;
  if (num_channels > 64) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  if (recording_.load()) return false;
  const std::string master_path = prefix + "_master.wav";
  if (!master_.open(master_path, sample_rate, 2, fmt)) return false;
  std::vector<WavWriter> stems(static_cast<std::size_t>(num_channels));
  for (int c = 0; c < num_channels; ++c) {
    char name[32];
    std::snprintf(name, sizeof(name), "_ch%02d.wav", c);
    if (!stems[static_cast<std::size_t>(c)].open(prefix + name, sample_rate, 1,
                                                 fmt)) {
      return false;
    }
  }
  stems_ = std::move(stems);
  master_path_ = master_path;
  prefix_ = prefix;
  sample_rate_ = sample_rate;
  format_ = fmt;
  mode_ = Mode::STEMS;
  recording_.store(true);
  return true;
}

void Recorder::pushMaster(const float* stereo_interleaved,
                          std::size_t frames) {
  if (stereo_interleaved == nullptr || frames == 0) return;
  if (!recording_.load()) return;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!recording_.load() || mode_ != Mode::MASTER_ONLY) return;
  if (!master_.isOpen()) return;
  master_.writeFrames(stereo_interleaved, frames);
}

void Recorder::pushStems(const float* const* channel_mono, int channels,
                         std::size_t frames) {
  if (channel_mono == nullptr || channels <= 0 || frames == 0) return;
  if (!recording_.load()) return;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!recording_.load() || mode_ != Mode::STEMS) return;
  if (static_cast<int>(stems_.size()) != channels) return;
  if (!master_.isOpen()) return;
  for (int c = 0; c < channels; ++c) {
    if (channel_mono[c] == nullptr) return;
  }
  // Master = exact sum of stems (same accumulation order every block).
  std::vector<float> master_stereo(frames * 2);
  for (std::size_t i = 0; i < frames; ++i) {
    float m = 0.0f;
    for (int c = 0; c < channels; ++c) m += channel_mono[c][i];
    master_stereo[i * 2] = m;
    master_stereo[i * 2 + 1] = m;
  }
  master_.writeFrames(master_stereo.data(), frames);
  for (int c = 0; c < channels; ++c) {
    stems_[static_cast<std::size_t>(c)].writeFrames(channel_mono[c], frames);
  }
}

void Recorder::stop() {
  std::lock_guard<std::mutex> lock(mutex_);
  master_.close();
  stems_.clear();
  recording_.store(false);
}

}  // namespace audio_dbg
