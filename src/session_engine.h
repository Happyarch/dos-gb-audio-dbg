// Stage 5.2: Session engine — deterministic 60 Hz frame-clock scheduler.
//
// `SessionEngine` owns the transport state (play/pause/stop, frame counter,
// loop range) and a timestamped SM83 sound-driver event list. Each tick():
//   1. dispatches every event stamped for the current frame to the active
//      `SoundDevice` (`MidiDevice` backends get noteOn()/noteOff(); every
//      other backend gets a handleCommand() note packet),
//   2. steps the active device via tick(frame),
//   3. advances one frame, wrapping to loop_start at loop_end or auto-
//      stopping at total_frames.
//
// The engine holds no audio state itself: mute/solo, synthesis and rendering
// stay inside the active device, so switching backends never touches the
// transport (Stage 5.4 A/B position locking builds on this).
//
// Event production (Stage 5.3) lives in `EnhancementManager`
// (enhancement_manager.h): compileEnhancement() resolves the YAML layer
// and loadSongBaseline()/loadMidiFile() load the GB base; both feed
// setSlotEvents()/setEnhancementEvents(). Until a caller loads them,
// events are injected with setEvents()/addEvent()/addNote().
//
// See docs/current_plan_debug_frontend.md §5 (5.2).

#ifndef PKMN_AUDIO_DBG_SESSION_ENGINE_H_
#define PKMN_AUDIO_DBG_SESSION_ENGINE_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace audio_dbg {

class SoundDevice;
struct SongInfo;

// A/B comparison slots (Stage 5.4): slot A holds the working copy (GB base
// + live enhancement), slot B the comparison baseline (GB-only base, or a
// pinned revision / compare file). Switching slots never touches the
// frame counter.
enum class ComparisonSlot { A = 0, B = 1 };

enum class SimEventType : std::uint8_t {
  Note = 0,
  ProgramChange = 1,
  ControlChange = 2,
};

// One simulated GB sound-driver voice event, stamped on the 60 Hz grid.
struct SimNoteEvent {
  std::uint32_t frame = 0;       // Frame on which this event fires.
  std::uint8_t channel = 0;      // Driver channel (0-based).
  std::uint8_t note = 60;        // MIDI note number (or program number if ProgramChange, CC controller if ControlChange).
  std::uint8_t velocity = 100;   // 1..127 (or CC value if ControlChange).
  std::uint16_t duration_frames = 0;  // Informational hold length; the
                                      // matching note-off is a separate
                                      // event (see addNote()).
  bool is_note_on = true;        // False = note-off for (channel, note).
  SimEventType type = SimEventType::Note;
};

inline int eventPriority(const SimNoteEvent& ev) {
  if (ev.type == SimEventType::ProgramChange ||
      ev.type == SimEventType::ControlChange) return 0;
  if (!ev.is_note_on) return 1;
  return 2;
}

inline bool eventLess(const SimNoteEvent& a, const SimNoteEvent& b) {
  if (a.frame != b.frame) return a.frame < b.frame;
  return eventPriority(a) < eventPriority(b);
}

class SessionEngine {
 public:
  SessionEngine();

  // --- Transport controls ---
  void play();    // Start/resume. Rewinds to 0 when stopped at the end.
  void pause();   // Freeze the frame counter (device keeps its state).
  void stop();    // Halt and rewind to frame 0.
  void seekToFrame(std::uint32_t target);  // Clamped to total_frames.
  void setLoop(std::uint32_t start, std::uint32_t end);  // Enabled iff end > start.
  void setTotalFrames(std::uint32_t frames) { total_frames_ = frames; }

  // --- Device routing ---
  // Not owned. May be null (ticks still advance the frame counter).
  // Swapping mid-playback preserves the frame counter (A/B locking) and
  // silences the outgoing device (MidiDevice::allNotesOff).
  void setActiveDevice(SoundDevice* dev);
  SoundDevice* activeDevice() const { return active_device_; }
  void syncDeviceState();

  // --- A/B comparison slots + enhancement overlay (Stage 5.4) ---
  // Each slot owns an event list; the dispatched stream is the active
  // slot's list plus the enhancement overlay when enabled. Every switch
  // below preserves current_frame_ exactly and silences the outgoing
  // device first, so playback continues at the same frame with no
  // stuck notes and no sample/frame drift.
  void setSlotEvents(ComparisonSlot slot, std::vector<SimNoteEvent> events);
  const std::vector<SimNoteEvent>& slotEvents(ComparisonSlot slot) const;
  // Switch the audible slot (silences + rebuilds; frame preserved).
  void setComparisonSlot(ComparisonSlot slot);
  ComparisonSlot activeSlot() const { return active_slot_; }
  void toggleSlotAB();
  // Enhancement overlay merged on top of whichever slot is active (e.g.
  // the compiled YAML layer over the GB base). Replacing the overlay
  // silences first (a held overlay note may lose its note-off).
  void setEnhancementEvents(std::vector<SimNoteEvent> events);
  const std::vector<SimNoteEvent>& enhancementEvents() const {
    return enhancement_events_;
  }
  // Mute/unmute the overlay without touching the slots (silences +
  // rebuilds; frame preserved). No-op when the value is unchanged.
  void setEnhancementEnabled(bool enabled);
  bool isEnhancementEnabled() const { return enhancement_enabled_; }

  // --- Event list (sorted by frame; stable for equal frames) ---
  // These address the ACTIVE slot's stored list (then rebuild the
  // dispatched stream with the overlay); use setSlotEvents() to target
  // a specific slot directly.
  void setEvents(std::vector<SimNoteEvent> events);
  void addEvent(const SimNoteEvent& ev);
  // Convenience: inserts a note-on at `frame` plus its note-off at
  // `frame + duration` (when duration > 0).
  void addNote(std::uint32_t frame, std::uint8_t channel, std::uint8_t note,
               std::uint8_t velocity, std::uint16_t duration);
  void clearEvents();
  // The dispatched stream: active slot + overlay (when enabled).
  const std::vector<SimNoteEvent>& events() const { return events_; }
  std::size_t eventCount() const { return events_.size(); }

  // --- 60 Hz scheduler step (see header comment) ---
  // No-op unless playing and not paused.
  void tick();

  // --- Transport state ---
  std::uint32_t currentFrame() const { return current_frame_; }
  std::uint32_t totalFrames() const { return total_frames_; }
  std::uint32_t loopStart() const { return loop_start_; }
  std::uint32_t loopEnd() const { return loop_end_; }
  bool loopEnabled() const { return loop_end_ > loop_start_; }
  bool isPlaying() const { return is_playing_; }
  bool isPaused() const { return is_paused_; }
  bool isStopped() const { return is_stopped_; }

 private:
  void dispatch(const SimNoteEvent& ev);
  // Rebuilds events_ from the active slot + overlay (when enabled).
  void rebuildActive();
  // Silences the current device before a stream switch: MidiDevice gets
  // allNotesOff(); other backends keep no sounding-note state in the
  // engine (they only see dispatched packets), so there is nothing to do.
  void silenceActiveDevice();
  static std::size_t slotIndex(ComparisonSlot slot) {
    return slot == ComparisonSlot::B ? 1 : 0;
  }

  SoundDevice* active_device_ = nullptr;
  // Dispatched stream (rebuilt from the slots + overlay; tick reads this).
  std::vector<SimNoteEvent> events_;
  // Per-slot stored lists; events_ mirrors the active one (+ overlay).
  std::vector<SimNoteEvent> slot_events_[2];
  ComparisonSlot active_slot_ = ComparisonSlot::A;
  std::vector<SimNoteEvent> enhancement_events_;
  bool enhancement_enabled_ = true;
  std::uint32_t current_frame_ = 0;
  std::uint32_t total_frames_ = 0;
  std::uint32_t loop_start_ = 0;
  std::uint32_t loop_end_ = 0;  // == start (or 0) = loop disabled.
  bool is_playing_ = false;
  bool is_paused_ = false;
  bool is_stopped_ = true;
};

}  // namespace audio_dbg

#endif  // PKMN_AUDIO_DBG_SESSION_ENGINE_H_
