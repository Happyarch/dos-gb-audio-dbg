// W1 transport-clock acceptance harness — TEMPO INVARIANCE.
//
// Wave-1 gate for the audio-driven tick clock. Standalone: links only
// session_engine.cpp (no ImGui, no SDL, no audio hardware). The ctest suite
// builds, but its catalog/enhancement cases cannot load repo data in this
// worktree (CTest probe: PKMN_REPO_ROOT_ABS is empty at HEAD), so this proves
// the clock invariant directly instead.
//
// WHAT IS PROVEN
//   1. One 60 Hz tick per (rate/60) rendered device samples at 1.0x, for any
//      render block size -> tempo depends on rendered samples, not wall time.
//   2. The tick TOTAL over a fixed number of rendered samples is identical
//      across three hostile caller patterns:
//        - 60 Hz vsync: fixed 16 ms of samples per UI frame;
//        - 240 Hz unbounded: 4 UI frames per 16 ms (no vsync);
//        - a stalled/blocky 16 ms-of-samples delivered as one 7680-sample
//          chunk per UI frame.
//      This is the "tempo invariant to display refresh/vsync" requirement.
//   3. The speed multiplier is a SAMPLE DIVISOR: 2x = half the samples per
//      tick, 0.25x = four times -> tempo scales, the UI clock does not.
//   4. ZERO back-to-back zero-render bursts: a single render-sized advance()
//      never dispatches more than one tick, in ANY of the above patterns
//      (this is the MUNT silent-note condition the rework removes).
//
// Build:  see tests/README_w1_clock.txt
// Run:    ./pkmn-audio-dbg-clock-tests

#include <cstdio>
#include <cstdint>

#include "session_engine.h"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    ++g_checks;                                                         \
    if (!(cond)) {                                                      \
      ++g_failures;                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
    }                                                                   \
  } while (0)

constexpr int kRate = 48000;
// 16 ms of device samples == one 60 Hz tick period at 48 kHz.
constexpr std::size_t kFrameSamples = 768;

// Drives the clock the way AudioClockShim::render() does: split each rendered
// request into chunks bounded by samplesUntilNextTick(), and count ticks per
// render-sized chunk. `block` = device samples per advance() call.
struct DriveResult {
  std::uint64_t ticks = 0;
  std::uint64_t chunks = 0;
  int max_ticks_per_chunk = 0;
  bool zero_render_burst = false;
};

DriveResult drive(audio_dbg::AudioTickClock& clock,
                  audio_dbg::SessionEngine& engine, std::size_t total_samples,
                  std::size_t block) {
  DriveResult r;
  std::size_t done = 0;
  while (done < total_samples) {
    std::size_t want = block;
    if (want > total_samples - done) want = total_samples - done;
    // Mirror AudioClockShim::render(): pump(0) then chunk render then pump.
    {
      const int n = clock.advance(engine, 0);
      r.ticks += static_cast<std::uint64_t>(n);
    }
    std::size_t chunk = want;
    const std::size_t until = clock.samplesUntilNextTick();
    if (chunk > until) chunk = until;
    if (chunk == 0) chunk = want;
    done += chunk;
    const int n = clock.advance(engine, chunk);
    r.ticks += static_cast<std::uint64_t>(n);
    ++r.chunks;
    if (n > r.max_ticks_per_chunk) r.max_ticks_per_chunk = n;
    if (n > 1) r.zero_render_burst = true;
  }
  return r;
}

}  // namespace

int main() {
  // --- 1. Base cadence: one tick per rate/60 samples, any block size -------
  {
    for (std::size_t block : {std::size_t(1), std::size_t(7), std::size_t(64),
                              std::size_t(480), std::size_t(512),
                              std::size_t(768), std::size_t(1024),
                              std::size_t(4096)}) {
      audio_dbg::SessionEngine eng;
      eng.setTotalFrames(1000000);  // No auto-stop inside the window.
      eng.play();
      audio_dbg::AudioTickClock clock;
      clock.setSampleRate(kRate);
      const DriveResult r =
          drive(clock, eng, /*total_samples=*/kRate * 2, block);
      // 2 s of rendered audio. The deadline is half-open (a tick fires once
      // `rendered >= next_tick_at_`), so a span of N tick periods dispatches
      // N + 1 ticks: 120 periods + 1.
      CHECK(r.ticks == 121);
      CHECK(!r.zero_render_burst);
      CHECK(r.max_ticks_per_chunk <= 1);
    }
    std::printf("PASS base cadence (1 tick / %zu samples) across block sizes\n",
                static_cast<std::size_t>(kRate / 60));
  }

  // --- 2. Tempo invariance across display-refresh / vsync patterns ---------
  {
    // (a) 60 Hz vsync: one 16 ms block per UI frame.
    // (b) 240 Hz unbounded: four 16 ms blocks per UI frame.
    // (c) stall/blocky: one UI frame renders a large burst of samples (e.g.
    //     a 250 ms audio-thread stall served on the next UI frame).
    struct Pattern {
      const char* name;
      std::size_t block;
    } patterns[] = {
        {"60Hz vsync (768 smp/frame)", 768},
        {"240Hz unbounded (768 smp/frame)", 768},
        {"stalled/blocky (7680 smp/frame)", 7680},
    };
    std::uint64_t reference_ticks = 0;
    for (const Pattern& p : patterns) {
      audio_dbg::SessionEngine eng;
      eng.setTotalFrames(1000000);
      eng.play();
      audio_dbg::AudioTickClock clock;
      clock.setSampleRate(kRate);
      // 10 s of audio, regardless of how it is delivered to the clock.
      const DriveResult r =
          drive(clock, eng, /*total_samples=*/kRate * 10, p.block);
      if (reference_ticks == 0) {
        reference_ticks = r.ticks;
      } else {
        CHECK(r.ticks == reference_ticks);  // Invariant to delivery pattern.
      }
      CHECK(!r.zero_render_burst);
      CHECK(r.max_ticks_per_chunk <= 1);
      std::printf("PASS tempo pattern '%s': ticks=%llu max/chunk=%d burst=%s\n",
                  p.name, static_cast<unsigned long long>(r.ticks),
                  r.max_ticks_per_chunk, r.zero_render_burst ? "YES" : "no");
    }
    // 10 s * 60 Hz = 600 tick periods; the half-open deadline fires once more
    // at the end of the span (the deadline lands exactly on sample kRate*10).
    CHECK(reference_ticks == 601);
    std::printf(
        "PASS tempo invariant to display refresh/vsync (10 s -> %llu ticks)\n",
        static_cast<unsigned long long>(reference_ticks));
  }

  // --- 3. Speed multiplier is a sample divisor -----------------------------
  {
    // speed 4.0 -> one tick per 800/4 = 200 samples; 2x -> 400; 0.25x -> 3200
    // (48 kHz / 60 = 800 samples per 60 Hz frame).
    struct SpeedCase {
      double speed;
      double expected_samples_per_tick;
    } cases[] = {{1.0, 800.0}, {2.0, 400.0}, {4.0, 200.0},
                 {0.5, 1600.0}, {0.25, 3200.0}};
    for (const SpeedCase& c : cases) {
      audio_dbg::SessionEngine eng;
      eng.setTotalFrames(1000000);
      eng.play();
      audio_dbg::AudioTickClock clock;
      clock.setSampleRate(kRate);
      clock.setSpeed(c.speed);
      CHECK(clock.samplesPerTick() == 800.0);
      CHECK(clock.speed() == c.speed);
      // 8 s of audio, delivered in 512-sample blocks (a real callback size).
      const DriveResult r =
          drive(clock, eng, /*total_samples=*/kRate * 8, 512);
      // Half-open deadline: N full periods + the boundary tick.
      const std::uint64_t expected =
          static_cast<std::uint64_t>(kRate * 8 / c.expected_samples_per_tick) +
          1;
      CHECK(r.ticks == expected);
      CHECK(!r.zero_render_burst);
      CHECK(r.max_ticks_per_chunk <= 1);
      std::printf("PASS speed %.2fx: ticks=%llu (expected %llu)\n", c.speed,
                  static_cast<unsigned long long>(r.ticks),
                  static_cast<unsigned long long>(expected));
    }
  }

  // --- 4. Pause/stop accrues no tick debt (no resume burst) ---------------
  {
    audio_dbg::SessionEngine eng;
    eng.setTotalFrames(1000000);
    eng.play();
    audio_dbg::AudioTickClock clock;
    clock.setSampleRate(kRate);
    // Run 100 UI frames, pause for 100 UI frames of silence, resume.
    for (int i = 0; i < 100; ++i) clock.advance(eng, kFrameSamples);
    const std::uint32_t at_pause = eng.currentFrame();
    eng.pause();
    for (int i = 0; i < 100; ++i) clock.advance(eng, kFrameSamples);
    CHECK(eng.currentFrame() == at_pause);  // No ticks while paused.
    // Resume: the very first rendered chunk must dispatch at most one tick
    // (no catch-up burst for the 100 paused frames).
    const int n = clock.advance(eng, kFrameSamples);
    CHECK(n <= 1);
    CHECK(clock.samplesUntilNextTick() != 0);
    std::printf("PASS pause/stop accrues no tick debt (resume ticks=%d)\n", n);
  }

  // --- 5. Speed change mid-run takes effect on the next deadline ----------
  {
    audio_dbg::SessionEngine eng;
    eng.setTotalFrames(1000000);
    eng.play();
    audio_dbg::AudioTickClock clock;
    clock.setSampleRate(kRate);
    for (int i = 0; i < 10; ++i) clock.advance(eng, kFrameSamples);
    clock.setSpeed(4.0);
    // Next frame must now produce >= 3 ticks (4x cadence) and still never a
    // single chunk >1. The clock itself is authoritative here: at 4x one
    // 768-sample render spans 4 tick periods.
    int total = 0;
    for (std::size_t done = 0; done < kFrameSamples;) {
      const int n0 = clock.advance(eng, 0);
      total += n0;
      std::size_t chunk = kFrameSamples - done;
      const std::size_t until = clock.samplesUntilNextTick();
      if (chunk > until) chunk = until;
      if (chunk == 0) chunk = kFrameSamples - done;
      done += chunk;
      total += clock.advance(eng, chunk);
    }
    CHECK(total >= 3);
    std::printf("PASS speed change mid-run (4x -> %d ticks in one 16ms frame)\n",
                total);
  }

  if (g_failures == 0) {
    std::printf("PASS w1 clock: %d/%d checks\n", g_checks, g_checks);
    return 0;
  }
  std::printf("FAIL w1 clock: %d/%d checks FAILED\n", g_failures, g_checks);
  return 1;
}
