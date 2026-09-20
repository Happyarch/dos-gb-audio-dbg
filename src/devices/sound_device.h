// Stage 2.1 + 2.5: Tier-1 universal sound-device base.
//
// `SoundDevice` is the polymorphic root for every synthesizer backend
// (OPL3, GB-APU, MT-32, GM, and future devices). It owns:
//   - per-channel mute/solo/dormant state,
//   - per-channel oscilloscope ring buffers,
//   - master volume/peak tracking,
//   - the 60 Hz frame counter the session engine drives via tick().
//
// Lazy channel wake-up (§2.5): every channel starts DORMANT. The first
// register write / note command addressed to a channel calls
// wakeChannel(ch). Hot paths (render, scope push, UI draw) open with
// `if (isDormant(ch)) return;` — a single bounds-checked load, i.e. the
// 1-cycle fast escape. Dormant channels therefore cost nothing on
// high-channel devices (18-voice OPL3, 16-channel GM).
//
// Oscilloscope feed (W6): the per-channel scope rings are lock-free SPSC.
// The AUDIO thread is the sole producer: each backend's render() (the path
// AudioMixer::renderBlock drives) taps its isolated per-voice/per-channel
// buffers into the ring via pushWaveform(). The UI thread only ever reads
// them, through copyWaveform() — it never calls the synth or advances
// emulation to fill a scope.
//
// See docs/current_plan_debug_frontend.md §2.1 / §2.5.

#ifndef PKMN_AUDIO_DBG_DEVICES_SOUND_DEVICE_H_
#define PKMN_AUDIO_DBG_DEVICES_SOUND_DEVICE_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace audio_dbg {

// Per-channel UI/engine state shared by every device family.
// Tier-2 bases (Midi/Fm/Psg) extend tracking alongside this, never by
// renaming these fields.
struct ChannelState {
  std::string name;      // Display label, e.g. "OPL3 V07", "GB Pulse1", "MT-32 P3".
  float volume = 1.0f;   // Linear 0..1 channel fader.
  float pan = 0.0f;      // -1 (L) .. +1 (R), 0 = centre.
  bool muted = false;    // Per-channel mute toggle.
  bool soloed = false;   // Per-channel solo toggle (any solo => only solos audible).
  bool dormant = true;   // §2.5: true until first command wakes the channel.
  float freq = 0.0f;     // Last known frequency in Hz (0 = unknown/silent).
  bool active = false;   // Currently sounding (note/key on).
  float peak = 0.0f;     // Last observed linear peak, for strip meters.
  int last_note = -1;    // Last MIDI note number, -1 = none.
};

// Point-in-time copy handed to the UI thread. Produced by snapshot().
struct DeviceSnapshot {
  int device_id = 0;
  std::string device_name;
  std::vector<ChannelState> channels;
  float master_volume = 1.0f;
  float master_peak = 0.0f;
  std::uint32_t frame = 0;  // Last tick() frame at snapshot time.
};

// Single simulated driver note event (from the SM83 driver simulation).
struct SimNote {
  int channel = -1;
  int note = 0;          // MIDI note number.
  int velocity = 0;      // 0..127.
  bool active = false;
  std::uint32_t start_frame = 0;
  std::uint32_t length_frames = 0;
};

// Session-engine driver state paired with snapshots in tracker views.
struct SimState {
  std::uint32_t driver_frame = 0;
  std::vector<SimNote> notes;
};

// Fixed-capacity float ring buffer feeding per-channel oscilloscopes.
// Oldest samples are overwritten once full; copyRecent() returns the
// newest `n` samples in chronological order.
//
// Lock-free SPSC: exactly one thread pushes (the audio thread, inside
// SoundDevice::render()), exactly one thread copies (the UI thread, inside
// a tab's draw). The producer publishes a monotonically increasing write
// counter with a release store; the consumer takes a single acquire
// snapshot and derives the readable window from it, so there is no torn
// index pair to reconcile. AudioMixer's master ring keeps its own mutex
// around this class and is unaffected.
//
// Non-copyable, movable (a moved-from ring is empty-equivalent) so it can
// live in std::vector.
class WaveformRing {
 public:
  explicit WaveformRing(std::size_t capacity = 4096) : buf_(capacity, 0.0f) {}

  WaveformRing(const WaveformRing&) = delete;
  WaveformRing& operator=(const WaveformRing&) = delete;
  WaveformRing(WaveformRing&& other) noexcept
      : buf_(std::move(other.buf_)),
        written_(other.written_.load(std::memory_order_relaxed)) {}
  WaveformRing& operator=(WaveformRing&& other) noexcept {
    if (this != &other) {
      buf_ = std::move(other.buf_);
      written_.store(other.written_.load(std::memory_order_relaxed),
                     std::memory_order_relaxed);
    }
    return *this;
  }

  // Producer side (single writer). Appends `n` samples; oldest overwritten.
  void push(const float* samples, std::size_t n) {
    if (buf_.empty() || samples == nullptr || n == 0) return;
    const std::size_t cap = buf_.size();
    // The producer owns `written_`; a relaxed read of its own last value is
    // sufficient. Publish only once, after every slot is written.
    std::size_t w = written_.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < n; ++i) {
      buf_[w % cap] = samples[i];
      ++w;
    }
    written_.store(w, std::memory_order_release);
  }

  // Consumer side (single reader). Copies up to `n` newest samples
  // (oldest-first) into `dst`. Returns the number actually written.
  std::size_t copyRecent(float* dst, std::size_t n) const {
    if (dst == nullptr || n == 0 || buf_.empty()) return 0;
    const std::size_t w = written_.load(std::memory_order_acquire);
    if (w == 0) return 0;
    const std::size_t cap = buf_.size();
    const std::size_t avail = w < cap ? w : cap;
    const std::size_t want = n < avail ? n : avail;
    std::size_t start = (w - want) % cap;
    for (std::size_t i = 0; i < want; ++i) {
      dst[i] = buf_[start];
      start = (start + 1) % cap;
    }
    return want;
  }

  // Reader-side reset (e.g. a tab clearing its view). Not safe to call
  // concurrently with the producer publishing a block.
  void clear() { written_.store(0, std::memory_order_release); }

  std::size_t size() const {
    const std::size_t w = written_.load(std::memory_order_acquire);
    const std::size_t cap = buf_.size();
    return w < cap ? w : cap;
  }
  std::size_t capacity() const { return buf_.size(); }

 private:
  std::vector<float> buf_;
  std::atomic<std::size_t> written_{0};  // Total samples ever pushed.
};

// Universal synthesizer backend interface.
class SoundDevice {
 public:
  SoundDevice(int device_id, std::string device_name, int channels)
      : device_id_(device_id),
        device_name_(std::move(device_name)),
        channels_(channels < 0 ? 0 : static_cast<std::size_t>(channels)),
        rings_(channels_.size()) {
    for (std::size_t i = 0; i < channels_.size(); ++i) {
      channels_[i].name = "CH" + std::to_string(i);
    }
  }

  virtual ~SoundDevice() = default;

  SoundDevice(const SoundDevice&) = delete;
  SoundDevice& operator=(const SoundDevice&) = delete;
  SoundDevice(SoundDevice&&) = default;
  SoundDevice& operator=(SoundDevice&&) = default;

  // --- Lifecycle (pure virtual: every backend implements these) ---
  virtual bool init() = 0;
  virtual void shutdown() = 0;
  virtual void reset() = 0;

  // --- Rendering (pure virtual) ---
  // render() mixes all audible channels into an interleaved stereo-safe
  // mono buffer of `frames` samples. renderPerChannel() fills one mono
  // buffer per channel (array of channelCount() pointers).
  virtual void render(float* buf, std::size_t frames) = 0;
  virtual void renderPerChannel(float** bufs, std::size_t frames) = 0;

  // --- Mute/solo (pure virtual: backends may add pre-synth filtering) ---
  virtual void setMute(int ch, bool muted) = 0;
  virtual void setSolo(int ch, bool soloed) = 0;

  // --- 60 Hz session pacing (pure virtual) ---
  virtual void tick(std::uint32_t frame) = 0;
  virtual void handleCommand(std::uint8_t opcode, const std::uint8_t* payload,
                             std::size_t len) = 0;

  // --- Snapshot for the UI thread (pure virtual) ---
  virtual DeviceSnapshot snapshot() const = 0;

  // --- Channel count / identity (concrete) ---
  int channelCount() const {
    return static_cast<int>(channels_.size());
  }
  int deviceId() const { return device_id_; }
  const std::string& deviceName() const { return device_name_; }

  // --- §2.5 lazy wake-up: 1-cycle fast escape ---------------------------
  // Dormant by default; out-of-range channels read dormant (safe skip).
  bool isDormant(int ch) const {
    if (ch < 0 || static_cast<std::size_t>(ch) >= channels_.size()) return true;
    return channels_[static_cast<std::size_t>(ch)].dormant;
  }

  void wakeChannel(int ch) {
    if (ch < 0 || static_cast<std::size_t>(ch) >= channels_.size()) return;
    ChannelState& st = channels_[static_cast<std::size_t>(ch)];
    st.dormant = false;
    st.active = true;
  }

  // --- Master volume/peak (concrete) ---
  void setMasterVolume(float v) {
    master_volume_ = v < 0.0f ? 0.0f : (v > 2.0f ? 2.0f : v);
  }
  float masterVolume() const { return master_volume_; }
  float masterPeak() const { return master_peak_; }

  // --- Oscilloscope ring buffers (concrete) ---
  // pushWaveform() is the AUDIO-thread producer side, called from each
  // backend's render(); copyWaveform() is the UI-thread reader side, called
  // from a tab's draw. Both are lock-free SPSC (see WaveformRing).
  // Dormant channels take the fast escape: nothing is stored.
  void pushWaveform(int ch, const float* samples, std::size_t n) {
    if (isDormant(ch) || samples == nullptr || n == 0) return;
    rings_[static_cast<std::size_t>(ch)].push(samples, n);
  }

  // UI-thread read: up to `n` newest samples for `ch`, oldest-first.
  std::size_t copyWaveform(int ch, float* dst, std::size_t n) const {
    if (ch < 0 || static_cast<std::size_t>(ch) >= rings_.size()) return 0;
    return rings_[static_cast<std::size_t>(ch)].copyRecent(dst, n);
  }

  std::size_t waveSize(int ch) const {
    if (ch < 0 || static_cast<std::size_t>(ch) >= rings_.size()) return 0;
    return rings_[static_cast<std::size_t>(ch)].size();
  }

 protected:
  bool channelValid(int ch) const {
    return ch >= 0 && static_cast<std::size_t>(ch) < channels_.size();
  }

  ChannelState& channel(int ch) { return channels_[static_cast<std::size_t>(ch)]; }
  const ChannelState& channel(int ch) const {
    return channels_[static_cast<std::size_t>(ch)];
  }

  // Helpers for derived setMute()/setSolo() overrides (one-liners).
  void setMutedState(int ch, bool muted) {
    if (channelValid(ch)) channels_[static_cast<std::size_t>(ch)].muted = muted;
  }
  void setSoloedState(int ch, bool soloed) {
    if (channelValid(ch)) channels_[static_cast<std::size_t>(ch)].soloed = soloed;
  }
  bool anySoloed() const {
    for (const ChannelState& st : channels_) {
      if (st.soloed) return true;
    }
    return false;
  }

  // Fills the Tier-1 portion of a snapshot; derived snapshot()
  // implementations return this directly (Tier-2 adds its own panels
  // from live backend state, not by extending this struct).
  DeviceSnapshot makeSnapshot() const {
    DeviceSnapshot s;
    s.device_id = device_id_;
    s.device_name = device_name_;
    s.channels = channels_;
    s.master_volume = master_volume_;
    s.master_peak = master_peak_;
    s.frame = frame_;
    return s;
  }

  void setFrame(std::uint32_t f) { frame_ = f; }
  std::uint32_t frame() const { return frame_; }
  void noteMasterPeak(float p) { master_peak_ = p; }

  void resizeChannels(int n) {
    std::size_t want = n < 0 ? 0 : static_cast<std::size_t>(n);
    channels_.resize(want);
    rings_.resize(want);
  }

 private:
  int device_id_ = 0;
  std::string device_name_;
  std::vector<ChannelState> channels_;
  std::vector<WaveformRing> rings_;
  float master_volume_ = 1.0f;
  float master_peak_ = 0.0f;
  std::uint32_t frame_ = 0;
};

}  // namespace audio_dbg

#endif  // PKMN_AUDIO_DBG_DEVICES_SOUND_DEVICE_H_
