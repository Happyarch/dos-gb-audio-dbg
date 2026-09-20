// Stage 3.1-3.2: Audio mixer engine.
//
// `AudioMixer` owns the SDL2 output device (stereo float32, default 48 kHz,
// 512-frame buffer) and pulls PCM from the currently active `SoundDevice`.
// The SDL audio callback runs on its own thread; all cross-thread state is
// atomic (device pointer, device rate, master gain/mute, recorder pointer).
// Per-block rendering is also exposed synchronously via renderBlock() /
// renderStemsBlock() so headless tests and the recorder path exercise the
// exact code the callback runs.
//
// Resampling: synth cores render at their native rate (e.g. 44100 Hz) while
// the output device runs at 48 kHz. Conversion uses a band-limited
// `SDL_AudioStream`, created once per device in setDevice() (UI thread, never
// inside the audio callback), reading/writing preallocated scratch. The old
// per-callback linear-interpolation FIFO is gone. `resampleLinear` survives
// only for the off-realtime stems/recorder path (renderStemsBlock).
// `SoundDevice::render()` produces mono; the mixer expands to stereo
// (L == R) after applying master gain/mute.
//
// Scope + record feeds: every rendered block pushes post-gain mono into
// `master_ring_` (a lock-free SPSC `WaveformRing` for the oscilloscope -- the
// audio callback produces, the UI consumes) and, when a `Recorder` is
// attached and recording, forwards the block to it.
//
// Device/channel mute/solo live on the `SoundDevice` (pre-synth filter for
// MIDI, voice matrix for FM/PSG). The mixer stages only the pointer swap
// atomically and forwards setDeviceChannelMute/Solo; master gain/mute are
// fully atomic floats/bools. Single-byte mute flags are torn-read safe on
// x86 with eventual visibility, which is sufficient for UI toggles.
//
// See docs/current_plan_debug_frontend.md §3.1 / §3.2.

#ifndef PKMN_AUDIO_DBG_AUDIO_MIXER_H_
#define PKMN_AUDIO_DBG_AUDIO_MIXER_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <SDL.h>

#include "devices/sound_device.h"

namespace audio_dbg {

class Recorder;  // src/recorder.h; forward here to avoid an include cycle.

class AudioMixer {
 public:
  static constexpr int kDefaultOutputRate = 48000;
  static constexpr int kDefaultBufferFrames = 512;
  static constexpr int kOutputChannels = 2;

  AudioMixer();
  ~AudioMixer();

  AudioMixer(const AudioMixer&) = delete;
  AudioMixer& operator=(const AudioMixer&) = delete;

  // Opens the SDL2 output device (stereo FLOAT32). When `disabled` is true
  // no SDL audio is touched (headless/tests); renderBlock() still works.
  // Returns true when the mixer is usable (opened or disabled-headless).
  bool init(int output_rate = kDefaultOutputRate,
            int buffer_frames = kDefaultBufferFrames, bool disabled = false);
  void shutdown();

  bool isOpen() const { return open_; }
  bool isDisabled() const { return disabled_; }
  int outputRate() const { return output_rate_; }
  int bufferFrames() const { return buffer_frames_; }

  // Thread synchronization: locks the SDL audio callback device so the
  // main thread can safely update device state or dispatch commands.
  void lock();
  void unlock();

  // Active synth + its native rate. `device_rate <= 0` means "same as
  // output" (no resampling). Null device renders silence. Atomic swap.
  // Builds the SDL_AudioStream resampler for this rate here, on the calling
  // (UI) thread -- never from the audio callback.
  void setDevice(SoundDevice* dev, int device_rate);
  SoundDevice* device() const { return device_.load(); }
  int deviceRate() const { return device_rate_.load(); }

  // Master controls (atomic, UI thread <-> audio callback).
  void setMasterGain(float g) { master_gain_.store(g); }
  float masterGain() const { return master_gain_.load(); }
  void setMasterMuted(bool m) { master_muted_.store(m); }
  bool masterMuted() const { return master_muted_.load(); }

  // Device/channel mute/solo conveniences: forward through the atomic
  // device pointer. Safe to call from the UI thread while audio runs.
  void setDeviceChannelMute(int ch, bool muted);
  void setDeviceChannelSolo(int ch, bool soloed);

  // Recorder hook (atomic). When attached and recording, renderBlock()
  // forwards master stereo blocks; renderStemsBlock() forwards stems.
  void setRecorder(Recorder* rec) { recorder_.store(rec); }
  Recorder* recorder() const { return recorder_.load(); }

  // Renders `frames` stereo-interleaved float samples into `stereo_out`
  // (size >= frames * 2). Silence when muted or deviceless. Pushes mono
  // master into the scope ring and forwards to the recorder (master path).
  void renderBlock(float* stereo_out, std::size_t frames);

  // Stems variant: renders per-channel mono at the device rate, resamples
  // each channel, applies master gain/mute, sums to stereo master
  // (L == R == sum of gained stems), pushes the ring, and forwards to the
  // recorder (stems path when armed, else master path). Falls back to
  // renderBlock() when there is no device or it has no channels.
  void renderStemsBlock(float* stereo_out, std::size_t frames);

  // Linear-interpolation resampler: `in_frames` may differ from
  // `out_frames`. Handles empty/degenerate inputs as silence. Pure helper
  // (no mixer state) so tests can verify conversion directly.
  static void resampleLinear(const float* in, std::size_t in_frames,
                             float* out, std::size_t out_frames);

  // Oscilloscope tap: newest-first copy of post-gain mono master.
  std::size_t copyMasterWaveform(float* dst, std::size_t n) const;
  std::size_t masterWaveSize() const;
  void clearMasterWaveform();

 private:
  // One resampler configuration: a band-limited SDL_AudioStream configured
  // for one in_rate -> out_rate pair. Published atomically by setDevice() and
  // consumed lock-free by renderBlock(). SDL owns the ratio: the callback
  // Puts a block's worth of native-rate input and Gets what is available, so
  // no fractional carry or priming state lives here. A state is never mutated
  // or freed by setDevice() once published -- the callback that already
  // loaded it stays valid; retired states are freed in shutdown().
  struct ResampleState {
    SDL_AudioStream* stream = nullptr;  // null => passthrough (rates equal)
    int in_rate = 0;
    int out_rate = 0;
  };

  static void SDLCALL sdlCallback(void* userdata, Uint8* stream, int len);
  void pushMasterRing(const float* mono, std::size_t frames);
  // Pulls `frames` output frames through the published resampler into
  // `mono`; zero-fills if no stream is available. Realtime-safe: uses only
  // preallocated scratch and the lock-free published state.
  void resampleInto(SoundDevice* dev, std::size_t frames, float* mono);

  std::atomic<SoundDevice*> device_{nullptr};
  std::atomic<int> device_rate_{kDefaultOutputRate};
  std::atomic<float> master_gain_{1.0f};
  std::atomic<bool> master_muted_{false};
  std::atomic<Recorder*> recorder_{nullptr};

  SDL_AudioDeviceID audio_dev_ = 0;
  bool open_ = false;
  bool disabled_ = false;
  int output_rate_ = kDefaultOutputRate;
  int buffer_frames_ = kDefaultBufferFrames;

  // Lock-free SPSC scope ring: the audio callback produces, the UI consumes
  // (see WaveformRing in devices/sound_device.h). No mutex.
  WaveformRing master_ring_{65536};

  // Resampler states (see ResampleState). resampler_ is the live one;
  // retired_resamplers_ holds superseded states until shutdown().
  std::atomic<ResampleState*> resampler_{nullptr};
  std::vector<ResampleState*> retired_resamplers_;

  // Realtime scratch, allocated in init() and never resized by the callback.
  std::size_t scratch_frames_ = 0;
  std::vector<float> mono_scratch_;
  std::vector<float> dev_scratch_;

  // Floor for the realtime scratch (frames); a larger request is clamped and
  // the tail zero-filled rather than allocating on the audio thread.
  static constexpr std::size_t kScratchFrames = 8192;
};

}  // namespace audio_dbg

#endif  // PKMN_AUDIO_DBG_AUDIO_MIXER_H_
