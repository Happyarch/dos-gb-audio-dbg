// Stage 8.1/8.2: Headless batch renderer + .audiolog replayer.
//
// `runHeadless()` is the --headless/--replay entry point shared by main()
// and the cli_verification tests:
//
// Track mode (--headless --track):
//   1. resolves the track via SongCatalog (exact constant or fuzzy name),
//   2. loads the GB baseline via EnhancementManager (device-mapped target:
//      mt32 for mt32/opl3/gbapu, gm for gm) plus the compiled enhancement
//      overlay unless --no-enh/--enhancement 0,
//   3. steps a SessionEngine frame-by-frame (engine.tick()), renders the
//      active device per frame, and writes stereo float32 WAV at the
//      device's native rate (no resampling, hence bit-deterministic).
// Replay mode (--replay <file.audiolog>):
//   reads captured (frame, opcode, payload) records via AudioLogReader and
//   dispatches them frame-by-frame to the target device with
//   dispatchReplayEvent() — the same note-on/off mapping the session
//   engine uses — rendering the same per-frame WAV.
//
// Per-frame sample distribution is exact and stateless: frame i renders
// floor(((i+1)*rate)/60) - floor((i*rate)/60) samples, so any frame count
// totals floor(frames*rate/60) samples. Rendering without --out still steps
// the engine + device (silent smoke render) and reports the summary.
//
// No SDL, no OpenGL, no ImGui anywhere in this module.
// See docs/current_plan_debug_frontend.md §8 (8.1/8.2).

#ifndef PKMN_AUDIO_DBG_HEADLESS_H_
#define PKMN_AUDIO_DBG_HEADLESS_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "cli.h"
#include "session_engine.h"

namespace audio_dbg {

class SoundDevice;

struct HeadlessResult {
  std::uint32_t frames_requested = 0;
  std::uint32_t frames_rendered = 0;
  std::uint32_t sample_rate = 0;
  std::uint64_t total_samples = 0;  // Mono samples rendered (per channel).
  std::string track;                // Resolved constant (track mode).
  std::string device;               // Canonical device key.
  std::string out_path;             // --out value (may be empty).
  float peak = 0.0f;                // Max |sample| observed.
  std::uint64_t nonzero_samples = 0;
};

// Creates + owns the backend for a canonical device key. On success returns
// the device (un-initialized; the caller inits) with *rateOut set to its
// native rate and *canonicalOut to the canonical key. Returns nullptr (with
// *err set) on unknown names. Accepted keys: mt32, gm, opl3, gbapu.
std::unique_ptr<SoundDevice> createHeadlessDevice(
    const std::string& name, std::uint32_t* rateOut, std::string* canonicalOut,
    std::string* err);

// Baseline/enhancement target for a canonical device: "gm" for gm,
// "mt32" for mt32/opl3/gbapu (assets/midi/ ships mt32 + gm renderings).
std::string deviceMidiTarget(const std::string& canonical);

// Exact per-frame sample count for a device rate at 60 Hz (stateless).
std::size_t frameSampleCount(std::uint32_t rate, std::uint32_t frame);
// Exact total mono samples for `frames` frames at `rate`.
std::uint64_t totalSamplesFor(std::uint32_t rate, std::uint32_t frames);

// Single-event dispatch for replay. For MidiDevice the decoded opcodes map
// to the concrete MIDI API the way SessionEngine::tick() applies them:
// 0x90 (note-on, payload ch/note/vel) -> noteOn(), 0x80 (note-off, payload
// ch/note) -> noteOff(), 0xC0 (program change, payload ch/program) ->
// programChange() and 0xB0 (control change, payload ch/controller/value) ->
// sendControlChange(). Every other backend, plus any opcode a MidiDevice
// does not decode, forwards to handleCommand() verbatim.
void dispatchReplayEvent(SoundDevice* dev, std::uint8_t opcode,
                         const std::uint8_t* payload, std::size_t len);

// Full pipelines (dispatch on opt.replay_path.empty()).
// Returns 0 on success (summary printed to stdout), 1 on failure (*err set).
int runHeadless(const CliOptions& opt, std::string* err, HeadlessResult* res);
int runHeadlessTrack(const CliOptions& opt, std::string* err,
                     HeadlessResult* res);
int runReplayLog(const CliOptions& opt, std::string* err, HeadlessResult* res);

}  // namespace audio_dbg

#endif  // PKMN_AUDIO_DBG_HEADLESS_H_
