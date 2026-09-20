// Stage 5.2: SessionEngine implementation. See session_engine.h.

#include "session_engine.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "devices/midi_device.h"
#include "devices/sound_device.h"

namespace audio_dbg {
namespace {

// Note packets for non-MIDI backends (OPL3/GB-APU/...): the opcode mirrors
// the MIDI status nibble; payload is (channel, note[, velocity]).
constexpr std::uint8_t kNoteOnOpcode = 0x90;
constexpr std::uint8_t kNoteOffOpcode = 0x80;

bool eventLess(const SimNoteEvent& a, const SimNoteEvent& b) {
  return a.frame < b.frame;
}

}  // namespace

SessionEngine::SessionEngine() = default;

void SessionEngine::play() {
  if (total_frames_ > 0 && current_frame_ >= total_frames_) {
    current_frame_ = 0;
  }
  is_playing_ = true;
  is_paused_ = false;
  is_stopped_ = false;
}

void SessionEngine::pause() {
  if (is_playing_ && !is_stopped_) {
    is_paused_ = true;
  }
}

void SessionEngine::stop() {
  is_playing_ = false;
  is_paused_ = false;
  is_stopped_ = true;
  current_frame_ = 0;
}

void SessionEngine::seekToFrame(std::uint32_t target) {
  if (total_frames_ > 0 && target > total_frames_) {
    target = total_frames_;
  }
  current_frame_ = target;
}

void SessionEngine::setLoop(std::uint32_t start, std::uint32_t end) {
  loop_start_ = start;
  loop_end_ = end;
}

void SessionEngine::setActiveDevice(SoundDevice* dev) {
  if (dev == active_device_) return;
  silenceActiveDevice();  // Outgoing device: no stuck notes.
  active_device_ = dev;   // Frame counter untouched (A/B locking).
}

void SessionEngine::rebuildActive() {
  events_ = slot_events_[slotIndex(active_slot_)];
  if (enhancement_enabled_) {
    events_.insert(events_.end(), enhancement_events_.begin(),
                   enhancement_events_.end());
  }
  std::stable_sort(events_.begin(), events_.end(), eventLess);
}

void SessionEngine::silenceActiveDevice() {
  if (active_device_ == nullptr) return;
  if (MidiDevice* midi = dynamic_cast<MidiDevice*>(active_device_)) {
    midi->allNotesOff();
  }
}

void SessionEngine::setSlotEvents(ComparisonSlot slot,
                                  std::vector<SimNoteEvent> events) {
  slot_events_[slotIndex(slot)] = std::move(events);
  std::stable_sort(slot_events_[slotIndex(slot)].begin(),
                   slot_events_[slotIndex(slot)].end(), eventLess);
  rebuildActive();  // Frame counter untouched.
}

const std::vector<SimNoteEvent>& SessionEngine::slotEvents(
    ComparisonSlot slot) const {
  return slot_events_[slotIndex(slot)];
}

void SessionEngine::setComparisonSlot(ComparisonSlot slot) {
  if (slot == active_slot_) return;
  silenceActiveDevice();
  active_slot_ = slot;
  rebuildActive();  // Frame counter untouched (position locking).
}

void SessionEngine::toggleSlotAB() {
  setComparisonSlot(active_slot_ == ComparisonSlot::A ? ComparisonSlot::B
                                                      : ComparisonSlot::A);
}

void SessionEngine::setEnhancementEvents(std::vector<SimNoteEvent> events) {
  silenceActiveDevice();
  enhancement_events_ = std::move(events);
  std::stable_sort(enhancement_events_.begin(), enhancement_events_.end(),
                   eventLess);
  rebuildActive();  // Frame counter untouched.
}

void SessionEngine::setEnhancementEnabled(bool enabled) {
  if (enabled == enhancement_enabled_) return;
  silenceActiveDevice();
  enhancement_enabled_ = enabled;
  rebuildActive();  // Frame counter untouched.
}

void SessionEngine::setEvents(std::vector<SimNoteEvent> events) {
  slot_events_[slotIndex(active_slot_)] = std::move(events);
  std::stable_sort(slot_events_[slotIndex(active_slot_)].begin(),
                   slot_events_[slotIndex(active_slot_)].end(), eventLess);
  rebuildActive();
}

void SessionEngine::addEvent(const SimNoteEvent& ev) {
  slot_events_[slotIndex(active_slot_)].push_back(ev);
  std::stable_sort(slot_events_[slotIndex(active_slot_)].begin(),
                   slot_events_[slotIndex(active_slot_)].end(), eventLess);
  rebuildActive();
}

void SessionEngine::addNote(std::uint32_t frame, std::uint8_t channel,
                            std::uint8_t note, std::uint8_t velocity,
                            std::uint16_t duration) {
  std::vector<SimNoteEvent>& slot = slot_events_[slotIndex(active_slot_)];
  SimNoteEvent on;
  on.frame = frame;
  on.channel = channel;
  on.note = note;
  on.velocity = velocity < 1 ? 1 : (velocity > 127 ? 127 : velocity);
  on.duration_frames = duration;
  on.is_note_on = true;
  slot.push_back(on);
  if (duration > 0) {
    SimNoteEvent off;
    off.frame = frame + duration;
    off.channel = channel;
    off.note = note;
    off.velocity = 0;
    off.duration_frames = 0;
    off.is_note_on = false;
    slot.push_back(off);
  }
  std::stable_sort(slot.begin(), slot.end(), eventLess);
  rebuildActive();
}

void SessionEngine::clearEvents() {
  slot_events_[slotIndex(active_slot_)].clear();
  rebuildActive();
}

void SessionEngine::dispatch(const SimNoteEvent& ev) {
  if (active_device_ == nullptr) return;
  // MIDI backends get real note dispatch (with pre-synth mute filtering);
  // every other backend gets a note packet via the command channel.
  if (MidiDevice* midi = dynamic_cast<MidiDevice*>(active_device_)) {
    if (ev.is_note_on) {
      midi->noteOn(ev.channel, ev.note, ev.velocity);
    } else {
      midi->noteOff(ev.channel, ev.note);
    }
    return;
  }
  if (ev.is_note_on) {
    const std::uint8_t payload[3] = {ev.channel, ev.note, ev.velocity};
    active_device_->handleCommand(kNoteOnOpcode, payload, sizeof(payload));
  } else {
    const std::uint8_t payload[2] = {ev.channel, ev.note};
    active_device_->handleCommand(kNoteOffOpcode, payload, sizeof(payload));
  }
}

void SessionEngine::tick() {
  if (!is_playing_ || is_paused_ || is_stopped_) return;
  for (const SimNoteEvent& ev : events_) {
    if (ev.frame == current_frame_) {
      dispatch(ev);
    } else if (ev.frame > current_frame_) {
      break;  // Sorted: nothing later can match this frame.
    }
  }
  if (active_device_ != nullptr) {
    active_device_->tick(current_frame_);
  }
  ++current_frame_;
  if (loopEnabled() && current_frame_ >= loop_end_) {
    // Single-step advance overshoots by exactly one, so this lands on
    // loop_start_ (the general form loop_start_ + (cur - loop_end_)
    // collapses to the same value).
    current_frame_ = loop_start_;
  } else if (total_frames_ > 0 && current_frame_ >= total_frames_) {
    current_frame_ = total_frames_;
    is_playing_ = false;
    is_paused_ = false;
    is_stopped_ = true;
  }
}

}  // namespace audio_dbg
