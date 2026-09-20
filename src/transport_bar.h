// Stage 6.1-6.4: Transport bar — playback controls, seek scrubber, loop
// markers, speed multipliers, enhancement/slot toggles, keyboard shortcuts.
//
// `TransportBar` owns the transport UI state the `SessionEngine` deliberately
// does NOT own: the playback-speed multiplier (+ fractional accumulator) and
// the loop toggle stash + revision-step cursor. Frame/loop/slot/enhancement
// state itself lives in the engine; this class edits it through the engine's
// public API so the frame counter, event stream and device silencing stay in
// one place (position-locked A/B switching, Stage 5.4).
//
// Headless design: every behaviour below EXCEPT render() is pure C++ with no
// ImGui/SDL dependency, so tests/test_transport.cpp drives the full logic
// (formatting, clamping, speed pacing, loop wrap, shortcuts) without a GUI.
// render() is the thin ImGui skin over the same methods.
//
// See docs/current_plan_debug_frontend.md §6 (6.1-6.4).

#ifndef PKMN_AUDIO_DBG_TRANSPORT_BAR_H_
#define PKMN_AUDIO_DBG_TRANSPORT_BAR_H_

#include <cstdint>
#include <string>

namespace audio_dbg {

class EnhancementManager;
class SessionEngine;

// Keyboard shortcuts (Stage 6.4). main.cpp maps SDL_Keycode -> TransportKey
// and forwards here; tests inject TransportKey directly (no SDL needed).
enum class TransportKey {
  PlayPause,          // Space: play / pause toggle.
  Rewind,             // Home: rewind to frame 0.
  StepBack,           // Left arrow: step backward 1 frame.
  StepForward,        // Right arrow: step forward 1 frame.
  ToggleEnhancement,  // E: enhancement overlay on/off.
  RevisionPrev,       // [: step to the previous (older) revision.
  RevisionNext,       // ]: step to the next (newer) revision.
  ToggleSlot,         // Tab: toggle comparison slot A/B.
};

class TransportBar {
 public:
  TransportBar();

  // --- Time formatting (Stage 6.1) ---
  // Frames at `fps` (default 60 Hz) as "MM:SS.f" (tenths). Minutes are at
  // least two digits and grow past 99 (e.g. frame 5040 -> "01:24.0").
  static std::string formatTime(std::uint32_t frame, double fps = 60.0);
  // "MM:SS.f / MM:SS.f (f: cur/total)" for the transport readout.
  static std::string formatReadout(std::uint32_t cur, std::uint32_t total,
                                   double fps = 60.0);
  // Clamp a (possibly negative) frame into [0, total]. total == 0 clamps
  // everything to 0 (the scrubber has no range yet).
  static std::uint32_t clampFrame(std::int64_t frame, std::uint32_t total);
  // Scrubber fraction helpers (Stage 6.2).
  static float frameToFraction(std::uint32_t cur, std::uint32_t total);
  static std::uint32_t fractionToFrame(float fraction, std::uint32_t total);

  // --- Speed multipliers (Stage 6.3): 0.25x, 0.5x, 1.0x, 2.0x, 4.0x ---
  static int numSpeeds();
  static double speedAt(int index);  // 1.0 for out-of-range index.
  int speedIndex() const { return speed_index_; }
  double speed() const { return speedAt(speed_index_); }
  // False (unchanged) when index is out of range.
  bool setSpeedIndex(int index);
  // Exact-match only; false (unchanged) for any other value.
  bool setSpeed(double speed);
  // Advance the engine by the speed-scaled tick count for ONE 60 Hz UI
  // frame (clock divider: accumulator_ gathers fractional ticks at
  // 0.25x/0.5x, bursts 2/4 ticks at 2.0x/4.0x). Returns the tick() calls
  // made. No-op (returns 0, accumulates nothing) while the engine is not
  // advancing, so pausing accrues no debt that would burst on resume.
  int advance(SessionEngine& engine);
  void resetAccumulator() { accumulator_ = 0.0; }
  double accumulator() const { return accumulator_; }

  // --- Transport ops (Stage 6.1/6.2) ---
  void togglePlayPause(SessionEngine& engine);
  // engine.stop() (halt + rewind to 0) + accumulator reset.
  void stop(SessionEngine& engine);
  // Silence active voices (MidiDevice::allNotesOff, else device reset),
  // then move the frame counter. The next advance()/tick() dispatches the
  // events stamped at the new frame, which is the playing-state resync.
  void seekToFrame(SessionEngine& engine, std::uint32_t frame);
  void rewind(SessionEngine& engine) { seekToFrame(engine, 0); }
  // Signed single-frame stepping with clamping (paused single step works:
  // the frame moves while the engine stays paused).
  void stepBy(SessionEngine& engine, int delta);
  void stepForward(SessionEngine& engine) { stepBy(engine, 1); }
  void stepBackward(SessionEngine& engine) { stepBy(engine, -1); }

  // --- Loop markers (Stage 6.3) ---
  // Set A/B to the current frame. Enabling needs end > start, so setting
  // one marker of a disabled loop stages it; setting both (A <= B) enables.
  void setLoopStartToCurrent(SessionEngine& engine);
  void setLoopEndToCurrent(SessionEngine& engine);
  void clearLoop(SessionEngine& engine);
  // Disable stashes the range; re-enable restores it (or [0, total] when
  // nothing was stashed). No-op when already in the requested state.
  void setLoopEnabled(SessionEngine& engine, bool enabled);
  void toggleLoop(SessionEngine& engine);

  // --- Enhancement + slot toggles (Stage 6.3) ---
  void toggleEnhancement(SessionEngine& engine);
  void toggleSlot(SessionEngine& engine);

  // --- Revision stepping (Stage 6.4: '[' / ']') ---
  // Cursor into the watched song's listRevisions() ordering (ascending id).
  // -1 = untracked (next step jumps to the oldest for +1, newest for -1).
  int revisionIndex() const { return revision_index_; }
  void setRevisionIndex(int index) { revision_index_ = index; }
  // Reverts the live YAML to the neighbouring revision in `direction`
  // (-1 older, +1 newer). False when there is no manager, no watched song,
  // no revisions, or the cursor is already at that end (no move).
  bool stepRevision(EnhancementManager* mgr, int direction);

  // --- Keyboard shortcuts (Stage 6.4) ---
  // True when the key was handled (revision steps report whether a step
  // actually happened, so a bare '[' with no revisions reads unhandled).
  bool handleKey(SessionEngine& engine, EnhancementManager* mgr,
                 TransportKey key);

  // --- ImGui skin (Stage 6.1-6.3) ---
  // Pinned bottom-of-viewport window: Play/Pause, Stop, readout, seek
  // slider, loop row, speed combo, enhancement + slot buttons, revision
  // stepper (only when enh_mgr is non-null). SDL key forwarding lives in
  // main.cpp's event loop (NOT here) so keys are handled exactly once.
  void render(SessionEngine& engine, EnhancementManager* enh_mgr = nullptr);

 private:
  void silence(SessionEngine& engine);

  int speed_index_ = 2;  // 1.0x.
  double accumulator_ = 0.0;
  // Loop toggle stash for setLoopEnabled(false -> true) round-trips.
  std::uint32_t saved_loop_start_ = 0;
  std::uint32_t saved_loop_end_ = 0;
  bool have_saved_loop_ = false;
  int revision_index_ = -1;
};

}  // namespace audio_dbg

#endif  // PKMN_AUDIO_DBG_TRANSPORT_BAR_H_
