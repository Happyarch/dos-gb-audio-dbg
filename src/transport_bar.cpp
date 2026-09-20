// Stage 6.1-6.4: TransportBar implementation. See transport_bar.h.

#include "transport_bar.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "devices/midi_device.h"
#include "devices/sound_device.h"
#include "enhancement_manager.h"
#include "imgui.h"
#include "session_engine.h"

namespace audio_dbg {

namespace {

// Selectable speeds (Stage 6.3). Index 2 (1.0x) is the default.
constexpr double kSpeeds[] = {0.25, 0.5, 1.0, 2.0, 4.0};
constexpr int kNumSpeeds = static_cast<int>(sizeof(kSpeeds) / sizeof(kSpeeds[0]));

}  // namespace

TransportBar::TransportBar() = default;

// --- Formatting ------------------------------------------------------------

std::string TransportBar::formatTime(std::uint32_t frame, double fps) {
  if (!(fps > 0.0)) fps = 60.0;
  const double total_seconds = static_cast<double>(frame) / fps;
  const int minutes = static_cast<int>(total_seconds / 60.0);
  const double rem = total_seconds - static_cast<double>(minutes) * 60.0;
  int seconds = static_cast<int>(rem);
  int tenths = static_cast<int>((rem - static_cast<double>(seconds)) * 10.0);
  // Float error can push tenths to 10 (e.g. 0.9999999 * 10 = 9.999... is
  // fine, but 1.9999996 - 1 = 0.9999996 * 10 = 9.999996 -> 9; the guard is
  // for the rare round-up past the tenth boundary).
  if (tenths >= 10) {
    tenths = 9;
  }
  if (seconds >= 60) {
    seconds = 59;
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%02d:%02d.%d", minutes, seconds, tenths);
  return buf;
}

std::string TransportBar::formatReadout(std::uint32_t cur, std::uint32_t total,
                                        double fps) {
  char buf[96];
  std::snprintf(buf, sizeof(buf), "%s / %s (f: %u/%u)",
                formatTime(cur, fps).c_str(), formatTime(total, fps).c_str(),
                cur, total);
  return buf;
}

std::uint32_t TransportBar::clampFrame(std::int64_t frame,
                                       std::uint32_t total) {
  if (frame <= 0 || total == 0) return 0;
  if (frame >= static_cast<std::int64_t>(total)) return total;
  return static_cast<std::uint32_t>(frame);
}

float TransportBar::frameToFraction(std::uint32_t cur, std::uint32_t total) {
  if (total == 0) return 0.0f;
  float f = static_cast<float>(cur) / static_cast<float>(total);
  if (f < 0.0f) return 0.0f;
  if (f > 1.0f) return 1.0f;
  return f;
}

std::uint32_t TransportBar::fractionToFrame(float fraction,
                                            std::uint32_t total) {
  if (total == 0) return 0;
  if (!(fraction > 0.0f)) return 0;
  if (fraction >= 1.0f) return total;
  return static_cast<std::uint32_t>(fraction * static_cast<float>(total) +
                                    0.5f);
}

// --- Speed -----------------------------------------------------------------

int TransportBar::numSpeeds() { return kNumSpeeds; }

double TransportBar::speedAt(int index) {
  if (index < 0 || index >= kNumSpeeds) return 1.0;
  return kSpeeds[index];
}

bool TransportBar::setSpeedIndex(int index) {
  if (index < 0 || index >= kNumSpeeds) return false;
  speed_index_ = index;
  return true;
}

bool TransportBar::setSpeed(double speed) {
  for (int i = 0; i < kNumSpeeds; ++i) {
    if (kSpeeds[i] == speed) {
      speed_index_ = i;
      return true;
    }
  }
  return false;
}

int TransportBar::advance(SessionEngine& engine) {
  if (!engine.isPlaying() || engine.isPaused() || engine.isStopped()) {
    return 0;
  }
  accumulator_ += speed();
  int steps = static_cast<int>(accumulator_);
  accumulator_ -= static_cast<double>(steps);
  int done = 0;
  for (int i = 0; i < steps; ++i) {
    if (!engine.isPlaying() || engine.isPaused() || engine.isStopped()) break;
    engine.tick();
    ++done;
  }
  return done;
}

// --- Transport ops ----------------------------------------------------------

void TransportBar::silence(SessionEngine& engine) {
  SoundDevice* dev = engine.activeDevice();
  if (dev == nullptr) return;
  if (MidiDevice* midi = dynamic_cast<MidiDevice*>(dev)) {
    midi->allNotesOff();
  } else {
    dev->reset();
  }
}

void TransportBar::togglePlayPause(SessionEngine& engine) {
  if (engine.isPlaying() && !engine.isPaused() && !engine.isStopped()) {
    engine.pause();
  } else {
    engine.play();
  }
}

void TransportBar::stop(SessionEngine& engine) {
  silence(engine);
  engine.stop();
  resetAccumulator();
}

void TransportBar::seekToFrame(SessionEngine& engine, std::uint32_t frame) {
  silence(engine);
  engine.seekToFrame(clampFrame(static_cast<std::int64_t>(frame),
                                engine.totalFrames()));
}

void TransportBar::stepBy(SessionEngine& engine, int delta) {
  const std::int64_t target =
      static_cast<std::int64_t>(engine.currentFrame()) + delta;
  seekToFrame(engine, clampFrame(target, engine.totalFrames()));
}

// --- Loop -------------------------------------------------------------------

void TransportBar::setLoopStartToCurrent(SessionEngine& engine) {
  engine.setLoop(engine.currentFrame(), engine.loopEnd());
}

void TransportBar::setLoopEndToCurrent(SessionEngine& engine) {
  engine.setLoop(engine.loopStart(), engine.currentFrame());
}

void TransportBar::clearLoop(SessionEngine& engine) {
  engine.setLoop(0, 0);
  have_saved_loop_ = false;
  saved_loop_start_ = 0;
  saved_loop_end_ = 0;
}

void TransportBar::setLoopEnabled(SessionEngine& engine, bool enabled) {
  if (enabled == engine.loopEnabled()) return;
  if (!enabled) {
    saved_loop_start_ = engine.loopStart();
    saved_loop_end_ = engine.loopEnd();
    have_saved_loop_ = true;
    engine.setLoop(0, 0);
  } else if (have_saved_loop_ && saved_loop_end_ > saved_loop_start_) {
    engine.setLoop(saved_loop_start_, saved_loop_end_);
  } else if (engine.totalFrames() > 0) {
    engine.setLoop(0, engine.totalFrames());
  }
}

void TransportBar::toggleLoop(SessionEngine& engine) {
  setLoopEnabled(engine, !engine.loopEnabled());
}

// --- Enhancement + slot -----------------------------------------------------

void TransportBar::toggleEnhancement(SessionEngine& engine) {
  engine.setEnhancementEnabled(!engine.isEnhancementEnabled());
}

void TransportBar::toggleSlot(SessionEngine& engine) {
  engine.toggleSlotAB();
}

// --- Revisions ---------------------------------------------------------------

bool TransportBar::stepRevision(EnhancementManager* mgr, int direction) {
  if (mgr == nullptr) return false;
  if (direction == 0) return false;
  const std::string song = mgr->watchedSong();
  if (song.empty()) return false;
  const std::vector<RevisionEntry> revs = mgr->listRevisions(song);
  if (revs.empty()) return false;
  const int n = static_cast<int>(revs.size());
  int next = revision_index_;
  if (next < 0 || next >= n) {
    // Untracked: enter at the oldest (forward) or newest (backward) end.
    next = (direction > 0) ? 0 : n - 1;
  } else {
    next += (direction > 0) ? 1 : -1;
  }
  if (next < 0 || next >= n) return false;
  if (!mgr->revertToRevision(song, revs[static_cast<std::size_t>(next)].id)) {
    return false;
  }
  revision_index_ = next;
  return true;
}

// --- Keyboard -----------------------------------------------------------------

bool TransportBar::handleKey(SessionEngine& engine, EnhancementManager* mgr,
                             TransportKey key) {
  switch (key) {
    case TransportKey::PlayPause:
      togglePlayPause(engine);
      return true;
    case TransportKey::Rewind:
      rewind(engine);
      return true;
    case TransportKey::StepBack:
      stepBackward(engine);
      return true;
    case TransportKey::StepForward:
      stepForward(engine);
      return true;
    case TransportKey::ToggleEnhancement:
      toggleEnhancement(engine);
      return true;
    case TransportKey::RevisionPrev:
      return stepRevision(mgr, -1);
    case TransportKey::RevisionNext:
      return stepRevision(mgr, 1);
    case TransportKey::ToggleSlot:
      toggleSlot(engine);
      return true;
  }
  return false;
}

// --- ImGui skin -----------------------------------------------------------------

void TransportBar::render(SessionEngine& engine, EnhancementManager* enh_mgr) {
  ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(
      ImVec2(viewport->Pos.x, viewport->Pos.y + viewport->Size.y - 104));
  ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, 104));
  ImGui::Begin("Transport", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

  // Row 1: Play/Pause, Stop, readout, seek slider.
  const bool playing =
      engine.isPlaying() && !engine.isPaused() && !engine.isStopped();
  if (ImGui::Button(playing ? "Pause##transport" : "Play##transport")) {
    togglePlayPause(engine);
  }
  ImGui::SameLine();
  if (ImGui::Button("Stop##transport")) {
    stop(engine);
  }
  ImGui::SameLine();
  ImGui::Text("%s",
              formatReadout(engine.currentFrame(), engine.totalFrames())
                  .c_str());
  ImGui::SameLine();
  {
    const std::uint32_t total = engine.totalFrames();
    int frame = static_cast<int>(engine.currentFrame());
    ImGui::SetNextItemWidth(-1);
    if (total == 0) {
      ImGui::BeginDisabled();
      ImGui::SliderInt("##seek", &frame, 0, 1, "Seek (no track)");
      ImGui::EndDisabled();
    } else if (ImGui::SliderInt("##seek", &frame, 0,
                                static_cast<int>(total), "Seek")) {
      // Draggable across [0, total_frames]: every edit resynchronizes
      // (silence + move; the next advance() dispatches the new frame).
      seekToFrame(engine, clampFrame(static_cast<std::int64_t>(frame), total));
    }
  }

  // Row 2: loop markers, speed, enhancement + slot toggles.
  ImGui::Text("Loop [A: %u | B: %u]%s", engine.loopStart(), engine.loopEnd(),
              engine.loopEnabled() ? "" : " (off)");
  ImGui::SameLine();
  if (ImGui::SmallButton("Set [A]##loop")) setLoopStartToCurrent(engine);
  ImGui::SameLine();
  if (ImGui::SmallButton("Set [B]##loop")) setLoopEndToCurrent(engine);
  ImGui::SameLine();
  if (ImGui::SmallButton(engine.loopEnabled() ? "Loop: On##loop"
                                              : "Loop: Off##loop")) {
    toggleLoop(engine);
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Clear##loop")) clearLoop(engine);
  ImGui::SameLine();
  {
    static constexpr const char* kLabels[] = {"0.25x", "0.5x", "1.0x", "2.0x",
                                              "4.0x"};
    int idx = speed_index_;
    ImGui::SetNextItemWidth(80);
    if (ImGui::Combo("Speed##transport", &idx, kLabels, kNumSpeeds)) {
      setSpeedIndex(idx);
    }
  }
  ImGui::SameLine();
  if (ImGui::SmallButton(engine.isEnhancementEnabled()
                             ? "[E] Enhancement##enh"
                             : "[G] GB Baseline##enh")) {
    toggleEnhancement(engine);
  }
  ImGui::SameLine();
  if (ImGui::SmallButton(engine.activeSlot() == ComparisonSlot::A
                             ? "Slot A (Working)##slot"
                             : "Slot B (Compare)##slot")) {
    toggleSlot(engine);
  }
  if (enh_mgr != nullptr) {
    ImGui::SameLine();
    if (ImGui::SmallButton("[##revprev")) stepRevision(enh_mgr, -1);
    ImGui::SameLine();
    if (ImGui::SmallButton("]##revnext")) stepRevision(enh_mgr, 1);
    ImGui::SameLine();
    const std::string song = enh_mgr->watchedSong();
    if (!song.empty()) {
      const std::vector<RevisionEntry> revs =
          enh_mgr->listRevisions(song);
      ImGui::TextDisabled("rev %d/%zu %s", revision_index_ + 1, revs.size(),
                          song.c_str());
    } else {
      ImGui::TextDisabled("no watched song");
    }
  }

  ImGui::End();
}

}  // namespace audio_dbg
