// Stage 3.1-3.2: AudioMixer implementation. See audio_mixer.h.

#include "audio_mixer.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "recorder.h"

namespace audio_dbg {

AudioMixer::AudioMixer() = default;

AudioMixer::~AudioMixer() { shutdown(); }

bool AudioMixer::init(int output_rate, int buffer_frames, bool disabled) {
  shutdown();
  if (output_rate <= 0) output_rate = kDefaultOutputRate;
  if (buffer_frames <= 0) buffer_frames = kDefaultBufferFrames;
  output_rate_ = output_rate;
  buffer_frames_ = buffer_frames;
  device_rate_.store(output_rate);
  disabled_ = disabled;
  if (disabled) {
    open_ = true;
    return true;
  }
  SDL_setenv("SDL_AUDIODRIVER", "pipewire", 1);
  SDL_AudioSpec want;
  SDL_zero(want);
  want.freq = output_rate_;
  want.format = AUDIO_F32SYS;
  want.channels = static_cast<Uint8>(kOutputChannels);
  want.samples = static_cast<Uint16>(buffer_frames_);
  want.callback = &AudioMixer::sdlCallback;
  want.userdata = this;
  SDL_AudioSpec have;
  SDL_zero(have);
  audio_dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
  if (audio_dev_ == 0) {
    open_ = false;
    return false;
  }
  // Accept what SDL gave us; frame size stays as requested.
  if (have.freq > 0) output_rate_ = static_cast<int>(have.freq);
  SDL_PauseAudioDevice(audio_dev_, 0);
  open_ = true;
  return true;
}

void AudioMixer::shutdown() {
  if (audio_dev_ != 0) {
    SDL_CloseAudioDevice(audio_dev_);
    audio_dev_ = 0;
  }
  open_ = false;
  disabled_ = false;
}

void AudioMixer::lock() {
  if (audio_dev_ != 0 && !disabled_) {
    SDL_LockAudioDevice(audio_dev_);
  }
}

void AudioMixer::unlock() {
  if (audio_dev_ != 0 && !disabled_) {
    SDL_UnlockAudioDevice(audio_dev_);
  }
}

void AudioMixer::setDevice(SoundDevice* dev, int device_rate) {
  device_.store(dev);
  if (device_rate <= 0) device_rate = output_rate_;
  device_rate_.store(device_rate);
  std::lock_guard<std::mutex> lock(resample_mutex_);
  resample_phase_ = 0.0;
  resample_fifo_.clear();
  fifo_head_ = 0;
}

void AudioMixer::setDeviceChannelMute(int ch, bool muted) {
  SoundDevice* dev = device_.load();
  if (dev != nullptr) dev->setMute(ch, muted);
}

void AudioMixer::setDeviceChannelSolo(int ch, bool soloed) {
  SoundDevice* dev = device_.load();
  if (dev != nullptr) dev->setSolo(ch, soloed);
}

void AudioMixer::resampleLinear(const float* in, std::size_t in_frames,
                                float* out, std::size_t out_frames) {
  if (out == nullptr || out_frames == 0) return;
  if (in == nullptr || in_frames == 0) {
    for (std::size_t i = 0; i < out_frames; ++i) out[i] = 0.0f;
    return;
  }
  if (in_frames == 1 || out_frames == 1) {
    const float v = in[0];
    for (std::size_t i = 0; i < out_frames; ++i) out[i] = v;
    return;
  }
  // Map output indices onto [0, in_frames - 1] and lerp neighbours.
  const double step =
      static_cast<double>(in_frames - 1) / static_cast<double>(out_frames - 1);
  for (std::size_t i = 0; i < out_frames; ++i) {
    const double pos = static_cast<double>(i) * step;
    std::size_t lo = static_cast<std::size_t>(pos);
    if (lo >= in_frames - 1) lo = in_frames - 2;
    const double frac = pos - static_cast<double>(lo);
    const float a = in[lo];
    const float b = in[lo + 1];
    out[i] = static_cast<float>(a + (b - a) * frac);
  }
}

void AudioMixer::pushMasterRing(const float* mono, std::size_t frames) {
  if (mono == nullptr || frames == 0) return;
  std::lock_guard<std::mutex> lock(ring_mutex_);
  master_ring_.push(mono, frames);
}

std::size_t AudioMixer::copyMasterWaveform(float* dst,
                                           std::size_t n) const {
  if (dst == nullptr || n == 0) return 0;
  std::lock_guard<std::mutex> lock(ring_mutex_);
  return master_ring_.copyRecent(dst, n);
}

std::size_t AudioMixer::masterWaveSize() const {
  std::lock_guard<std::mutex> lock(ring_mutex_);
  return master_ring_.size();
}

void AudioMixer::clearMasterWaveform() {
  std::lock_guard<std::mutex> lock(ring_mutex_);
  master_ring_.clear();
}

void AudioMixer::renderBlock(float* stereo_out, std::size_t frames) {
  if (stereo_out == nullptr || frames == 0) return;
  SoundDevice* dev = device_.load();
  const float gain = master_gain_.load();
  const bool muted = master_muted_.load();
  const int out_rate = output_rate_ > 0 ? output_rate_ : kDefaultOutputRate;
  int dev_rate = device_rate_.load();
  if (dev_rate <= 0) dev_rate = out_rate;

  std::vector<float> mono(frames, 0.0f);
  if (dev != nullptr && !muted) {
    if (dev_rate == out_rate) {
      dev->render(mono.data(), frames);
      if (gain != 1.0f) {
        for (std::size_t i = 0; i < frames; ++i) mono[i] *= gain;
      }
    } else {
      std::lock_guard<std::mutex> lock(resample_mutex_);
      const double ratio =
          static_cast<double>(dev_rate) / static_cast<double>(out_rate);
      if (fifo_head_ > 0) {
        resample_fifo_.erase(resample_fifo_.begin(),
                             resample_fifo_.begin() + fifo_head_);
        fifo_head_ = 0;
      }
      const std::size_t needed =
          static_cast<std::size_t>(
              std::ceil(static_cast<double>(frames) * ratio)) + 4;
      if (resample_fifo_.size() < needed) {
        const std::size_t to_render = needed - resample_fifo_.size();
        std::vector<float> dev_chunk(to_render, 0.0f);
        dev->render(dev_chunk.data(), to_render);
        resample_fifo_.insert(resample_fifo_.end(), dev_chunk.begin(),
                              dev_chunk.end());
      }
      for (std::size_t i = 0; i < frames; ++i) {
        if (fifo_head_ >= resample_fifo_.size()) break;
        const float s0 = resample_fifo_[fifo_head_];
        const float s1 = (fifo_head_ + 1 < resample_fifo_.size())
                             ? resample_fifo_[fifo_head_ + 1]
                             : s0;
        mono[i] =
            (s0 + (s1 - s0) * static_cast<float>(resample_phase_)) * gain;
        resample_phase_ += ratio;
        while (resample_phase_ >= 1.0) {
          resample_phase_ -= 1.0;
          ++fifo_head_;
        }
      }
    }
  }  // else: silence (muted or deviceless).

  for (std::size_t i = 0; i < frames; ++i) {
    const float val = std::clamp(mono[i], -1.0f, 1.0f);
    stereo_out[i * 2] = val;
    stereo_out[i * 2 + 1] = val;
  }
  pushMasterRing(mono.data(), frames);
  Recorder* rec = recorder_.load();
  if (rec != nullptr) rec->pushMaster(stereo_out, frames);
}

void AudioMixer::renderStemsBlock(float* stereo_out, std::size_t frames) {
  if (stereo_out == nullptr || frames == 0) return;
  SoundDevice* dev = device_.load();
  const float gain = master_gain_.load();
  const bool muted = master_muted_.load();
  const int out_rate = output_rate_ > 0 ? output_rate_ : kDefaultOutputRate;
  int dev_rate = device_rate_.load();
  if (dev_rate <= 0) dev_rate = out_rate;

  if (dev == nullptr || dev->channelCount() <= 0) {
    renderBlock(stereo_out, frames);
    return;
  }
  const int channels = dev->channelCount();
  const std::size_t nch = static_cast<std::size_t>(channels);

  std::vector<float> mono(frames, 0.0f);
  // Per-channel output staging (post-resample, post-gain).
  std::vector<std::vector<float> > stem_out(nch, std::vector<float>(frames));
  if (!muted) {
    if (dev_rate == out_rate) {
      std::vector<float*> ptrs(nch);
      for (std::size_t c = 0; c < nch; ++c) ptrs[c] = stem_out[c].data();
      dev->renderPerChannel(ptrs.data(), frames);
      if (gain != 1.0f) {
        for (std::size_t c = 0; c < nch; ++c) {
          for (std::size_t i = 0; i < frames; ++i) stem_out[c][i] *= gain;
        }
      }
    } else {
      const std::size_t dev_frames =
          static_cast<std::size_t>(
              (static_cast<double>(frames) * static_cast<double>(dev_rate) /
               static_cast<double>(out_rate))) +
          2;
      std::vector<std::vector<float> > dev_bufs(nch,
                                                std::vector<float>(dev_frames));
      std::vector<float*> dev_ptrs(nch);
      for (std::size_t c = 0; c < nch; ++c) dev_ptrs[c] = dev_bufs[c].data();
      dev->renderPerChannel(dev_ptrs.data(), dev_frames);
      for (std::size_t c = 0; c < nch; ++c) {
        resampleLinear(dev_bufs[c].data(), dev_frames, stem_out[c].data(),
                       frames);
        if (gain != 1.0f) {
          for (std::size_t i = 0; i < frames; ++i) stem_out[c][i] *= gain;
        }
      }
    }
    // Master = sum of gained stems (same order the recorder sums in).
    for (std::size_t i = 0; i < frames; ++i) {
      float m = 0.0f;
      for (std::size_t c = 0; c < nch; ++c) m += stem_out[c][i];
      mono[i] = m;
    }
  }  // else: silence (stems stay zeroed).

  for (std::size_t i = 0; i < frames; ++i) {
    const float val = std::clamp(mono[i], -1.0f, 1.0f);
    stereo_out[i * 2] = val;
    stereo_out[i * 2 + 1] = val;
  }
  pushMasterRing(mono.data(), frames);
  Recorder* rec = recorder_.load();
  if (rec != nullptr) {
    if (rec->isRecording() && rec->mode() == Recorder::Mode::STEMS &&
        rec->stemChannels() == channels) {
      std::vector<const float*> cptrs(nch);
      for (std::size_t c = 0; c < nch; ++c) cptrs[c] = stem_out[c].data();
      rec->pushStems(cptrs.data(), channels, frames);
    } else {
      rec->pushMaster(stereo_out, frames);
    }
  }
}

void SDLCALL AudioMixer::sdlCallback(void* userdata, Uint8* stream, int len) {
  auto* self = static_cast<AudioMixer*>(userdata);
  if (self == nullptr || stream == nullptr || len <= 0) return;
  const std::size_t total_floats = static_cast<std::size_t>(len) / sizeof(float);
  const std::size_t frames = total_floats / 2;
  if (frames == 0) return;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  float* out = reinterpret_cast<float*>(stream);
  // Master path only inside the realtime callback: the stems variant needs
  // per-channel scratch allocation, so the UI/record path calls
  // renderStemsBlock() explicitly from its own thread.
  self->renderBlock(out, frames);
  // Zero any trailing partial frame (len not a multiple of a stereo frame).
  const std::size_t used = frames * 2;
  for (std::size_t i = used; i < total_floats; ++i) out[i] = 0.0f;
}

}  // namespace audio_dbg
