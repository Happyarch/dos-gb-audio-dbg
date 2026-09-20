// Stage 3.1-3.2: AudioMixer implementation. See audio_mixer.h.

#include "audio_mixer.h"

#include <algorithm>
#include <cstring>
#include <new>
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
  // Realtime scratch: sized once here (UI thread) so the audio callback never
  // allocates. Extra headroom over the device block covers any resample ratio
  // up to ~1.125x without touching the allocator.
  scratch_frames_ = static_cast<std::size_t>(buffer_frames_) > kScratchFrames
                        ? static_cast<std::size_t>(buffer_frames_)
                        : kScratchFrames;
  mono_scratch_.assign(scratch_frames_, 0.0f);
  // Device-native input scratch: one mixer block, plus slack so an oversized
  // caller block clamps instead of overflowing. SDL keeps the leftover.
  dev_scratch_.assign(scratch_frames_ + 8, 0.0f);
  if (disabled) {
    open_ = true;
    return true;
  }
  // NOTE: SDL_AUDIODRIVER is set in main.cpp before SDL_Init(); setting it
  // here (after the audio subsystem has already chosen its driver) is dead.
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
  // The SDL audio callback is stopped, so it is now safe to free the live
  // resampler and every state retired by earlier setDevice() calls.
  ResampleState* live = resampler_.exchange(nullptr);
  if (live != nullptr) {
    if (live->stream != nullptr) SDL_FreeAudioStream(live->stream);
    delete live;
  }
  for (ResampleState* rs : retired_resamplers_) {
    if (rs == nullptr) continue;
    if (rs->stream != nullptr) SDL_FreeAudioStream(rs->stream);
    delete rs;
  }
  retired_resamplers_.clear();
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
  if (device_rate <= 0) {
    device_rate = output_rate_ > 0 ? output_rate_ : kDefaultOutputRate;
  }
  device_.store(dev);
  device_rate_.store(device_rate);

  // Build the resampler HERE -- on the UI thread, outside the audio callback
  // -- rather than doing linear interpolation per callback. SDL_AudioStream
  // is band-limited conversion (SDL's own resampler): the anti-alias low-pass
  // the hand-rolled path never had.
  ResampleState* rs = new (std::nothrow) ResampleState();
  if (rs == nullptr) return;
  rs->in_rate = device_rate;
  rs->out_rate = output_rate_ > 0 ? output_rate_ : kDefaultOutputRate;
  if (rs->in_rate != rs->out_rate) {
    rs->stream = SDL_NewAudioStream(AUDIO_F32SYS, 1, rs->in_rate, AUDIO_F32SYS,
                                    1, rs->out_rate);
    if (rs->stream != nullptr) SDL_AudioStreamClear(rs->stream);
  }
  // Publish. The superseded state is retired, NOT freed: a callback already
  // inside resampleInto() may still be using it. Freed in shutdown().
  ResampleState* old = resampler_.exchange(rs);
  if (old != nullptr) retired_resamplers_.push_back(old);
}

void AudioMixer::resampleInto(SoundDevice* dev, std::size_t frames,
                              float* mono) {
  ResampleState* rs = resampler_.load();
  if (dev == nullptr || rs == nullptr || rs->stream == nullptr) {
    if (dev != nullptr) {
      // No stream (rates equal or SDL refused one): render 1:1 so the device
      // -- and therefore the AudioClockShim -- is still pulled every block.
      dev->render(mono, frames);
    } else {
      std::memset(mono, 0, frames * sizeof(float));
    }
    return;
  }
  // Feed SDL the exact amount of device-native input needed to produce
  // `frames` output frames at this ratio, tracked with a fractional carry so
  // the long-run rate is exact (no drift, no backlog creep). SDL_AudioStream
  // does the actual band-limited resampling; we only compute how much input
  // to render. Rendering a fixed `frames` of input per block is wrong
  // whenever in_rate != out_rate (e.g. OPL3's 49716 -> 48000): it starves the
  // stream and the zero-fill below injects a silence glitch every block.
  rs->frac += static_cast<double>(frames) * static_cast<double>(rs->in_rate);
  std::size_t dev_frames =
      static_cast<std::size_t>(rs->frac / static_cast<double>(rs->out_rate));
  rs->frac -= static_cast<double>(dev_frames) * static_cast<double>(rs->out_rate);
  if (!rs->primed) {
    dev_frames += kResamplerPrime;
    rs->primed = true;
  }
  if (dev_frames > dev_scratch_.size()) dev_frames = dev_scratch_.size();
  if (dev_frames == 0) dev_frames = 1;

  // Pull the device at its native rate: this is the AudioClockShim's
  // sample-accounting boundary, so it must run every block.
  dev->render(dev_scratch_.data(), dev_frames);
  const int put_bytes = static_cast<int>(dev_frames * sizeof(float));
  if (SDL_AudioStreamPut(rs->stream, dev_scratch_.data(), put_bytes) != 0) {
    std::memset(mono, 0, frames * sizeof(float));
    return;
  }
  const int want_bytes = static_cast<int>(frames * sizeof(float));
  const int got_bytes = SDL_AudioStreamGet(rs->stream, mono, want_bytes);
  const std::size_t got =
      got_bytes > 0
          ? std::min(static_cast<std::size_t>(got_bytes) / sizeof(float), frames)
          : 0;
  // SDL emits at its own ratio, so a single block usually yields close to
  // `frames` but not exactly; pad the difference with silence. The un-emitted
  // samples stay in the stream and surface on the next Get. This is SDL's
  // conversion lateness, not a hand-rolled feed.
  if (got < frames) {
    std::memset(mono + got, 0, (frames - got) * sizeof(float));
  }
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
  // Lock-free SPSC: the audio callback is the sole producer (see
  // WaveformRing in devices/sound_device.h). No mutex on the realtime path.
  master_ring_.push(mono, frames);
}

std::size_t AudioMixer::copyMasterWaveform(float* dst,
                                           std::size_t n) const {
  if (dst == nullptr || n == 0) return 0;
  // Lock-free SPSC: the UI thread is the sole consumer.
  return master_ring_.copyRecent(dst, n);
}

std::size_t AudioMixer::masterWaveSize() const {
  return master_ring_.size();
}

void AudioMixer::clearMasterWaveform() {
  // Reader-side reset: safe because the UI thread owns the consume side and
  // the producer only ever advances its own write counter monotonically.
  master_ring_.clear();
}

void AudioMixer::renderBlock(float* stereo_out, std::size_t frames) {
  if (stereo_out == nullptr || frames == 0) return;
  SoundDevice* dev = device_.load();
  const float gain = master_gain_.load();
  const bool muted = master_muted_.load();

  // Realtime path: preallocated scratch only. No allocator, no mutex, no
  // erase/insert memmove on this thread.
  float* mono = mono_scratch_.data();
  std::size_t n = frames;
  if (n > scratch_frames_) {
    n = scratch_frames_;
    // Anything past the preallocated window is silence, not an allocation.
    std::memset(stereo_out + n * 2, 0, (frames - n) * 2 * sizeof(float));
  }
  if (n == 0) {  // renderBlock before init(): nothing to render into.
    std::memset(stereo_out, 0, frames * 2 * sizeof(float));
    return;
  }

  // Clock accounting FIRST, BEFORE the master-mute check: the device (and
  // through it the AudioClockShim) must be pulled every block or muting
  // freezes the audio-driven transport clock. Mute silences the mix only.
  ResampleState* rs = resampler_.load();
  if (dev == nullptr) {
    std::memset(mono, 0, n * sizeof(float));
  } else if (rs != nullptr && rs->in_rate != rs->out_rate) {
    resampleInto(dev, n, mono);
  } else {
    dev->render(mono, n);
  }

  // Master gain/mute applied AFTER the device run.
  if (dev != nullptr && !muted) {
    if (gain != 1.0f) {
      for (std::size_t i = 0; i < n; ++i) mono[i] *= gain;
    }
  } else {
    std::memset(mono, 0, n * sizeof(float));
  }

  for (std::size_t i = 0; i < n; ++i) {
    const float val = std::clamp(mono[i], -1.0f, 1.0f);
    stereo_out[i * 2] = val;
    stereo_out[i * 2 + 1] = val;
  }
  pushMasterRing(mono, n);
  Recorder* rec = recorder_.load();
  if (rec != nullptr) rec->pushMaster(stereo_out, n);
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
