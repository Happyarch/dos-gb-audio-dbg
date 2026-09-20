// Stage 7.6: Schism Tracker style tracker view.
//
// `TrackerView` compares the SM83 sound-driver simulation against the live
// sound-chip register state:
//   - one column per channel (GB: Pulse 1 / Pulse 2 / Wave / Noise;
//     MIDI: up to 16 tracks),
//   - per-cell note name + octave (Schism style "C-4" / "C#4"), velocity
//     and instrument,
//   - divergence highlighting: green when the simulated note matches the
//     chip register state, yellow when one side is silent, red when both
//     sides sound different notes.
//
// The view is standalone (not a `DeviceTab`): it reads a `SimState`
// (driver simulation) plus a `DeviceSnapshot` (chip registers) and renders
// the comparison. All colour/logic helpers are ImGui-free so headless unit
// checks can drive them without a context; draw() early-returns without
// one.
//
// See docs/current_plan_debug_frontend.md §7.6.

#ifndef PKMN_AUDIO_DBG_TABS_TRACKER_VIEW_H_
#define PKMN_AUDIO_DBG_TABS_TRACKER_VIEW_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "devices/sound_device.h"
#include "imgui.h"
#include "session_engine.h"

namespace audio_dbg {

// One tracker cell: the simulated note vs the chip register note for a
// single channel. -1 on either side means silent on that side.
struct TrackerCell {
  int channel = -1;
  int sim_note = -1;     // MIDI note 0..127, -1 = silent.
  int chip_note = -1;    // MIDI note 0..127, -1 = silent.
  int velocity = 0;      // 0..127 (from the sim side).
  int instrument = -1;   // Program/patch, -1 = unknown.
  bool divergent = false;
  bool active = false;  // Either side sounding.
};

class TrackerView {
 public:
  TrackerView() = default;

  // --- ImGui-free note helpers (Schism style) ---
  // Pitch class only ("C", "C#", "D", ...). Clamps out-of-range notes
  // into 0..127 before naming.
  static const char* pitchName(int midi_note);
  // Standard MIDI octave (note / 12 - 1): note 60 -> 4.
  static int pitchOctave(int midi_note);
  // Schism cell text: naturals get a "-" filler ("C-4"), sharps keep the
  // accidental ("C#4"). Returns "---" for silent (-1).
  static std::string noteText(int midi_note);

  // Divergence: any mismatch between the two sides (one silent, or two
  // different notes). Both silent (-1, -1) is not divergent.
  static bool isDivergent(int sim_note, int chip_note);

  // Cell colour (no context needed):
  //   silent          -> dim gray,
  //   matching+active -> vibrant green,
  //   divergent, one side silent -> amber/yellow,
  //   divergent, both sounding   -> red.
  static ImVec4 cellColor(const TrackerCell& cell);
  static const char* divergenceName(const TrackerCell& cell);

  // Builds one cell per channel from live state:
  //   sim side  = first active SimNote addressed to that channel (-1),
  //   chip side = snapshot last_note when the channel is non-dormant and
  //               marked active, else -1.
  // `velocity`/`instrument` come from the sim note (0/-1 when silent).
  static TrackerCell cellForChannel(const SimState& sim,
                                    const DeviceSnapshot& snap, int ch);
  // Up to `channels` cells (negative/zero -> empty).
  static std::vector<TrackerCell> compare(const SimState& sim,
                                          const DeviceSnapshot& snap,
                                          int channels);
  // SimNoteEvent variant: sim side = most recent note-on at or before
  // `frame` on that channel that has no later note-off. Chip side is the
  // same snapshot rule as above.
  static std::vector<TrackerCell> compareEvents(
      const std::vector<SimNoteEvent>& events, std::uint32_t frame,
      const DeviceSnapshot& snap, int channels);

  // --- ImGui skin (headless-safe) ---
  // Renders the comparison table for `channels` columns. Null sim renders
  // the chip side only (every cell silent on the sim side).
  void draw(const SimState* sim, const DeviceSnapshot& snap,
            int channels) const;
};

}  // namespace audio_dbg

#endif  // PKMN_AUDIO_DBG_TABS_TRACKER_VIEW_H_
