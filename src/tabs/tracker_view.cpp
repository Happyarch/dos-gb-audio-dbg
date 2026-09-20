// Stage 7.6: TrackerView implementation.
// draw() early-returns without an ImGui context. See tracker_view.h.

#include "tabs/tracker_view.h"

namespace audio_dbg {

namespace {

constexpr const char* kPitchNames[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B",
};

int clampNote(int n) {
  if (n < 0) return 0;
  if (n > 127) return 127;
  return n;
}

}  // namespace

const char* TrackerView::pitchName(int midi_note) {
  return kPitchNames[clampNote(midi_note) % 12];
}

int TrackerView::pitchOctave(int midi_note) {
  return clampNote(midi_note) / 12 - 1;
}

std::string TrackerView::noteText(int midi_note) {
  if (midi_note < 0 || midi_note > 127) return "---";
  const char* p = pitchName(midi_note);
  const int oct = pitchOctave(midi_note);
  std::string out(p);
  if (out.size() < 2 || out[1] != '#') out += '-';
  out += std::to_string(oct);
  return out;
}

bool TrackerView::isDivergent(int sim_note, int chip_note) {
  if (sim_note < 0 && chip_note < 0) return false;
  return sim_note != chip_note;
}

ImVec4 TrackerView::cellColor(const TrackerCell& cell) {
  if (!cell.active) return ImVec4(0.15f, 0.15f, 0.15f, 1.0f);  // Silent gray.
  if (!cell.divergent)
    return ImVec4(0.0f, 1.0f, 0.2f, 1.0f);  // Matching green.
  if (cell.sim_note < 0 || cell.chip_note < 0)
    return ImVec4(1.0f, 0.85f, 0.0f, 1.0f);  // One side silent: yellow.
  return ImVec4(1.0f, 0.2f, 0.1f, 1.0f);  // Both sounding, differ: red.
}

const char* TrackerView::divergenceName(const TrackerCell& cell) {
  if (!cell.active) return "SILENT";
  if (!cell.divergent) return "MATCH";
  if (cell.sim_note < 0 || cell.chip_note < 0) return "MISSING";
  return "DIVERGED";
}

TrackerCell TrackerView::cellForChannel(const SimState& sim,
                                        const DeviceSnapshot& snap, int ch) {
  TrackerCell c;
  c.channel = ch;
  int sim_note = -1;
  int sim_vel = 0;
  for (const SimNote& n : sim.notes) {
    if (n.channel == ch && n.active) {
      sim_note = n.note;
      sim_vel = n.velocity;
      break;
    }
  }
  int chip_note = -1;
  if (ch >= 0 && static_cast<std::size_t>(ch) < snap.channels.size()) {
    const ChannelState& s = snap.channels[static_cast<std::size_t>(ch)];
    if (s.library_voices) {
      // Engine-reported voice state (MT-32 getPartStates()/getPlayingNotes()).
      // Never the register-level mirror, which survives reset().
      chip_note = s.sounding_note;
    } else if (!s.dormant && s.active && s.last_note >= 0) {
      chip_note = s.last_note;
    }
  }
  c.sim_note = sim_note;
  c.chip_note = chip_note;
  c.velocity = sim_vel;
  c.instrument = -1;
  c.divergent = isDivergent(sim_note, chip_note);
  c.active = (sim_note >= 0 || chip_note >= 0);
  return c;
}

std::vector<TrackerCell> TrackerView::compare(const SimState& sim,
                                              const DeviceSnapshot& snap,
                                              int channels) {
  std::vector<TrackerCell> out;
  if (channels <= 0) return out;
  out.reserve(static_cast<std::size_t>(channels));
  for (int ch = 0; ch < channels; ++ch) {
    out.push_back(cellForChannel(sim, snap, ch));
  }
  return out;
}

std::vector<TrackerCell> TrackerView::compareEvents(
    const std::vector<SimNoteEvent>& events, std::uint32_t frame,
    const DeviceSnapshot& snap, int channels) {
  std::vector<TrackerCell> out;
  if (channels <= 0) return out;
  out.reserve(static_cast<std::size_t>(channels));
  for (int ch = 0; ch < channels; ++ch) {
    int sounding = -1;
    int vel = 0;
    for (const SimNoteEvent& e : events) {
      if (e.frame > frame) break;
      if (e.channel != static_cast<std::uint8_t>(ch)) continue;
      if (e.is_note_on) {
        sounding = e.note;
        vel = e.velocity;
      } else if (sounding == e.note) {
        sounding = -1;
        vel = 0;
      }
    }
    TrackerCell c;
    c.channel = ch;
    c.sim_note = sounding;
    c.velocity = vel;
    c.instrument = -1;
    int chip_note = -1;
    if (ch >= 0 && static_cast<std::size_t>(ch) < snap.channels.size()) {
      const ChannelState& s = snap.channels[static_cast<std::size_t>(ch)];
      if (s.library_voices) {
        // Engine-reported voice state, same rule as cellForChannel().
        chip_note = s.sounding_note;
      } else if (!s.dormant && s.active && s.last_note >= 0) {
        chip_note = s.last_note;
      }
    }
    c.chip_note = chip_note;
    c.divergent = isDivergent(sounding, chip_note);
    c.active = (sounding >= 0 || chip_note >= 0);
    out.push_back(c);
  }
  return out;
}

void TrackerView::draw(const SimState* sim, const DeviceSnapshot& snap,
                       int channels) const {
  if (ImGui::GetCurrentContext() == nullptr) return;
  if (channels <= 0) {
    ImGui::TextDisabled("Tracker: no channels.");
    return;
  }
  SimState empty;
  empty.driver_frame = snap.frame;
  const SimState& ref = (sim != nullptr) ? *sim : empty;
  const std::vector<TrackerCell> cells = compare(ref, snap, channels);
  if (ImGui::BeginTable("##tracker", channels + 1,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("CH");
    for (int ch = 0; ch < channels; ++ch) {
      ImGui::TableNextColumn();
      ImGui::Text("CH%d", ch);
    }
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("Sim/Chip");
    for (const TrackerCell& c : cells) {
      ImGui::TableNextColumn();
      const ImVec4 col = cellColor(c);
      ImGui::TextColored(col, "%s", noteText(c.sim_note).c_str());
      ImGui::SameLine();
      ImGui::TextDisabled("|");
      ImGui::SameLine();
      ImGui::Text("%s", noteText(c.chip_note).c_str());
      ImGui::TextDisabled("v=%d %s", c.velocity, divergenceName(c));
    }
    ImGui::EndTable();
  }
  (void)snap;
}

}  // namespace audio_dbg
