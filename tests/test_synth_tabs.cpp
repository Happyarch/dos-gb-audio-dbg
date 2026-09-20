// Stage 7.3/7.4/7.6/7.7: Acceptance tests for the synth tabs + tracker.
//
// Verifies (headless: no GUI, no audio hardware):
//   Opl3Tab:
//     1. voiceLabel() 18-voice bank labelling + bankName().
//     2. Operator parameters via opParams() (MOD/CAR round-trip through
//        the FmDevice setters, feedback/connection, envelope phase).
//     3. findZeroCrossing() stabilization (sine crossing, flat/DC,
//        null/short inputs).
//   GbApuTab:
//     1. Duty fractions via PsgTab::dutyFraction() (incl. masking).
//     2. Wave RAM decoding: 16 raw bytes -> 32 nibbles via writeRegister.
//     3. LFSR mode: 7-bit vs 15-bit, shift/divisor decode, width names,
//        wave volume names, envelopeName() strings.
//   TrackerView:
//     1. pitchName()/pitchOctave()/noteText() anchors (Schism style).
//     2. isDivergent() logic + cellColor()/divergenceName() palette.
//     3. compare() / compareEvents() matching vs divergent cells.
//   All tabs + TrackerView:
//     1. Dormant escape + null-safety in pushWaveformData().
//     2. draw*() headless safety (no ImGui context: must not crash),
//        including detached (null-device) tabs.
// Exits 0 with ALL PASS on success, 1 on any failure.

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "devices/gb_apu_device.h"
#include "devices/opl3_device.h"
#include "tabs/gb_apu_tab.h"
#include "tabs/opl3_tab.h"
#include "tabs/tracker_view.h"

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

bool vec4Near(const ImVec4& v, float r, float g, float b, float a) {
  return std::fabs(v.x - r) < 1e-5f && std::fabs(v.y - g) < 1e-5f &&
         std::fabs(v.z - b) < 1e-5f && std::fabs(v.w - a) < 1e-5f;
}

}  // namespace

int main() {
  using namespace audio_dbg;

  // --- Opl3Tab: 18-voice labels + bank names ------------------------------
  {
    Opl3Tab tab;
    CHECK(Opl3Tab::kVoices == 18);
    CHECK(std::strcmp(tab.voiceLabel(0), "Voice 1 (Primary)") == 0);
    CHECK(std::strcmp(tab.voiceLabel(8), "Voice 9 (Primary)") == 0);
    CHECK(std::strcmp(tab.voiceLabel(9), "Voice 10 (Secondary)") == 0);
    CHECK(std::strcmp(tab.voiceLabel(17), "Voice 18 (Secondary)") == 0);
    CHECK(std::strcmp(tab.voiceLabel(-1), "Invalid") == 0);
    CHECK(std::strcmp(tab.voiceLabel(18), "Invalid") == 0);
    CHECK(std::strcmp(Opl3Tab::bankName(0), "Primary") == 0);
    CHECK(std::strcmp(Opl3Tab::bankName(8), "Primary") == 0);
    CHECK(std::strcmp(Opl3Tab::bankName(9), "Secondary") == 0);
    CHECK(std::strcmp(Opl3Tab::bankName(17), "Secondary") == 0);
    CHECK(std::strcmp(Opl3Tab::bankName(-1), "Invalid") == 0);
    CHECK(std::strcmp(Opl3Tab::bankName(18), "Invalid") == 0);
    // Labels embed the 1-based voice number and the bank.
    for (int v = 0; v < 18; ++v) {
      std::string label(tab.voiceLabel(v));
      CHECK(label.find("Voice " + std::to_string(v + 1)) != std::string::npos);
      const char* bank = Opl3Tab::bankName(v);
      CHECK(label.find(bank) != std::string::npos);
    }
    std::printf("PASS opl3 voice labels\n");
  }

  // --- Opl3Tab: operator parameters + envelope phase ----------------------
  {
    Opl3Device dev;
    CHECK(dev.init());
    Opl3Tab tab;
    tab.setOpl3Device(&dev);
    CHECK(tab.opl3Device() == &dev);

    // Detached probe returns defaults.
    {
      Opl3Tab bare;
      FmOperatorParams d = bare.opParams(0, false);
      CHECK(d.mult == 1);
    }

    FmOperatorParams mod;
    mod.mult = 3;
    mod.tl = 17;
    mod.ar = 12;
    mod.dr = 5;
    mod.sl = 7;
    mod.rr = 9;
    mod.ws = 2;
    FmOperatorParams car;
    car.mult = 1;
    car.tl = 42;
    car.ar = 15;
    car.dr = 3;
    car.sl = 4;
    car.rr = 6;
    car.ws = 5;
    dev.writeOperator(2, false, mod);
    dev.writeOperator(2, true, car);
    dev.writeFeedback(2, 5, 1);
    dev.writeFrequency(2, 512, 4);
    dev.voiceKeyOn(2);

    FmOperatorParams got_mod = tab.opParams(2, false);
    FmOperatorParams got_car = tab.opParams(2, true);
    CHECK(got_mod.mult == 3);
    CHECK(got_mod.tl == 17);
    CHECK(got_mod.ar == 12);
    CHECK(got_mod.dr == 5);
    CHECK(got_mod.sl == 7);
    CHECK(got_mod.rr == 9);
    CHECK(got_car.mult == 1);
    CHECK(got_car.tl == 42);
    CHECK(got_car.ws == 5);
    // Out-of-range probe returns defaults, never crashes.
    FmOperatorParams bad = tab.opParams(99, true);
    CHECK(bad.mult == 1);

    // Voice view reflects key/feedback/connection state.
    FmTab::VoiceView vv = tab.voiceView(2);
    CHECK(!vv.dormant);
    CHECK(vv.keyed_on);
    CHECK(vv.connection == 1);
    CHECK(vv.feedback == 5);
    CHECK(std::strcmp(FmTab::envelopePhaseName(true, 1.0f), "ATTACK") == 0);
    CHECK(std::strcmp(FmTab::envelopePhaseName(false, 0.0f), "RELEASE") == 0);
    // Untouched voice stays dormant.
    CHECK(tab.voiceView(7).dormant);

    // Mute toggling through the tab.
    CHECK(!dev.snapshot().channels[2].muted);
    tab.onMuteClick(2);
    CHECK(dev.snapshot().channels[2].muted);
    tab.onMuteClick(2);
    CHECK(!dev.snapshot().channels[2].muted);
    tab.onMuteClick(-1);
    tab.onMuteClick(99);
    Opl3Tab bare;
    bare.onMuteClick(0);
    dev.shutdown();
    std::printf("PASS opl3 operator params\n");
  }

  // --- Zero-crossing stabilization (both tabs share the contract) ---------
  {
    // Rising sine: first positive-slope zero crossing is interior.
    float sine[64];
    for (int i = 0; i < 64; ++i) {
      sine[i] = std::sin(2.0f * 3.14159265f * static_cast<float>(i) / 64.0f);
    }
    std::size_t zx = Opl3Tab::findZeroCrossing(sine, 64);
    CHECK(zx == 1u);
    CHECK(GbApuTab::findZeroCrossing(sine, 64) == zx);
    // Phase-shifted buffer: sin(2*PI*(i+50)/64) rises through zero at
    // global 64 -> local 14 (in float32 sinf(2*PI) is a tiny positive
    // 1.7e-07, so the exact-zero sample itself is the crossing).
    float shifted[32];
    for (int i = 0; i < 32; ++i) {
      shifted[i] =
          std::sin(2.0f * 3.14159265f * static_cast<float>(i + 50) / 64.0f);
    }
    CHECK(Opl3Tab::findZeroCrossing(shifted, 32) == 14u);
    CHECK(GbApuTab::findZeroCrossing(shifted, 32) == 14u);
    // Flat/DC buffers have no crossing -> 0 (raw start fallback).
    float flat[16] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f,
                      0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
    CHECK(Opl3Tab::findZeroCrossing(flat, 16) == 0u);
    CHECK(GbApuTab::findZeroCrossing(flat, 16) == 0u);
    float neg[8] = {-1.0f, -0.5f, -0.2f, -0.1f, -0.4f, -0.3f, -0.2f, -0.1f};
    CHECK(Opl3Tab::findZeroCrossing(neg, 8) == 0u);
    // Degenerate inputs.
    CHECK(Opl3Tab::findZeroCrossing(nullptr, 64) == 0u);
    CHECK(Opl3Tab::findZeroCrossing(sine, 0) == 0u);
    CHECK(Opl3Tab::findZeroCrossing(sine, 1) == 0u);
    CHECK(GbApuTab::findZeroCrossing(nullptr, 8) == 0u);
    CHECK(GbApuTab::findZeroCrossing(neg, 0) == 0u);
    std::printf("PASS zero-crossing stabilization\n");
  }

  // --- GbApuTab: duty fractions -------------------------------------------
  {
    CHECK(std::fabs(PsgTab::dutyFraction(0) - 0.125f) < 1e-6f);
    CHECK(std::fabs(PsgTab::dutyFraction(1) - 0.25f) < 1e-6f);
    CHECK(std::fabs(PsgTab::dutyFraction(2) - 0.5f) < 1e-6f);
    CHECK(std::fabs(PsgTab::dutyFraction(3) - 0.75f) < 1e-6f);
    // Masking: only the low 2 bits select the duty.
    CHECK(PsgTab::dutyFraction(4) == PsgTab::dutyFraction(0));
    CHECK(PsgTab::dutyFraction(7) == PsgTab::dutyFraction(3));
    CHECK(std::strcmp(GbApuTab::channelLabel(0).c_str(), "Pulse 1") == 0);
    CHECK(std::strcmp(GbApuTab::channelLabel(1).c_str(), "Pulse 2") == 0);
    CHECK(std::strcmp(GbApuTab::channelLabel(2).c_str(), "Wave") == 0);
    CHECK(std::strcmp(GbApuTab::channelLabel(3).c_str(), "Noise") == 0);
    CHECK(GbApuTab::channelLabel(99) == "Invalid");
    // Null draw-list primitives must not crash.
    std::array<std::uint8_t, 32> ram{};
    ram.fill(8);
    PsgTab::drawDutyBar(nullptr, ImVec2(0, 0), 128.0f, 12.0f, 2);
    PsgTab::drawWaveRam(nullptr, ImVec2(0, 0), 160.0f, 32.0f, ram);
    std::printf("PASS gb duty fractions\n");
  }

  // --- GbApuTab: wave RAM decoding + volume + envelope --------------------
  {
    GbApuDevice dev;
    CHECK(dev.init());
    GbApuTab tab;
    tab.setGbApuDevice(&dev);
    CHECK(tab.gbApuDevice() == &dev);

    // Write a known wave pattern through the register path: byte i holds
    // nibbles (2*i, 2*i+1) mod 16.
    for (int i = 0; i < 16; ++i) {
      const std::uint8_t hi = static_cast<std::uint8_t>((2 * i) & 0x0F);
      const std::uint8_t lo = static_cast<std::uint8_t>((2 * i + 1) & 0x0F);
      dev.writeRegister(static_cast<std::uint16_t>(0xFF30 + i),
                        static_cast<std::uint8_t>((hi << 4) | lo));
    }
    GbApuTab::ChannelCard card = tab.channelCard(2);
    CHECK(!card.dormant);
    for (int i = 0; i < 32; ++i) {
      CHECK(card.wave_ram[static_cast<std::size_t>(i)] ==
            static_cast<std::uint8_t>(i & 0x0F));
    }
    // Nibbles stay 4-bit after a 0xFF write.
    dev.writeRegister(0xFF30, 0xFF);
    card = tab.channelCard(2);
    CHECK(card.wave_ram[0] == 0x0F);
    CHECK(card.wave_ram[1] == 0x0F);

    // Wave volume codes + names.
    CHECK(std::strcmp(GbApuTab::waveVolumeName(0), "Mute") == 0);
    CHECK(std::strcmp(GbApuTab::waveVolumeName(1), "100%") == 0);
    CHECK(std::strcmp(GbApuTab::waveVolumeName(2), "50%") == 0);
    CHECK(std::strcmp(GbApuTab::waveVolumeName(3), "25%") == 0);
    dev.writeRegister(0xFF1C, 0x40);  // Bits 6-5 = 10 -> 50%.
    CHECK(tab.channelCard(2).wave_volume_code == 2);
    CHECK(dev.waveVolumeCode() == 2);

    // Envelope names are non-empty and stable for 0..7 (+ clamping).
    for (int e = 0; e < 8; ++e) {
      CHECK(tab.envelopeName(e) != nullptr);
      CHECK(std::strlen(tab.envelopeName(e)) > 0);
    }
    CHECK(tab.envelopeName(-5) == tab.envelopeName(0));
    CHECK(tab.envelopeName(99) == tab.envelopeName(7));

    // Pulse duty + envelope volume decode through registers.
    dev.writeRegister(0xFF11, 0x80);  // Duty 10 -> 50%.
    dev.writeRegister(0xFF12, 0xF3);  // Vol 15, pace 3.
    GbApuTab::ChannelCard p1 = tab.channelCard(0);
    CHECK(p1.duty == 2);
    CHECK(p1.volume == 15);
    CHECK(p1.envelope == 3);
    CHECK(p1.frequency >= 0.0);
    dev.shutdown();
    std::printf("PASS gb wave ram + envelope\n");
  }

  // --- GbApuTab: noise LFSR mode ------------------------------------------
  {
    GbApuDevice dev;
    CHECK(dev.init());
    GbApuTab tab;
    tab.setGbApuDevice(&dev);

    CHECK(std::strcmp(GbApuTab::noiseWidthName(true), "7-bit") == 0);
    CHECK(std::strcmp(GbApuTab::noiseWidthName(false), "15-bit") == 0);

    // NR43 = shift(7-4) | width(3) | divisor(2-0). 0x38 = shift 3, 7-bit.
    dev.writeRegister(0xFF22, 0x38);
    CHECK(dev.noiseSevenBit());
    CHECK(dev.noiseShift() == 3);
    CHECK(dev.noiseDivisor() == 0);
    GbApuTab::ChannelCard nz = tab.channelCard(3);
    CHECK(nz.noise_seven_bit);
    CHECK(nz.noise_shift == 3);
    CHECK(nz.noise_divisor == 0);

    // 0x40 = shift 4, 15-bit, divisor 0.
    dev.writeRegister(0xFF22, 0x40);
    CHECK(!dev.noiseSevenBit());
    CHECK(dev.noiseShift() == 4);
    nz = tab.channelCard(3);
    CHECK(!nz.noise_seven_bit);
    CHECK(std::strcmp(GbApuTab::noiseWidthName(nz.noise_seven_bit),
                      "15-bit") == 0);

    // Sweep register is exposed on pulse 1 only.
    dev.writeRegister(0xFF10, 0x19);
    CHECK(tab.channelCard(0).sweep == 0x19);
    CHECK(dev.sweep(0) == 0x19);
    CHECK(dev.sweep(1) == 0);

    // Mute toggling through the tab.
    CHECK(!dev.snapshot().channels[3].muted);
    tab.onMuteClick(3);
    CHECK(dev.snapshot().channels[3].muted);
    tab.onMuteClick(3);
    CHECK(!dev.snapshot().channels[3].muted);
    tab.onMuteClick(-1);
    tab.onMuteClick(99);
    GbApuTab bare;
    bare.onMuteClick(0);
    dev.shutdown();
    std::printf("PASS gb lfsr mode\n");
  }

  // --- TrackerView: note naming (Schism style) -----------------------------
  {
    CHECK(std::strcmp(TrackerView::pitchName(60), "C") == 0);
    CHECK(std::strcmp(TrackerView::pitchName(61), "C#") == 0);
    CHECK(std::strcmp(TrackerView::pitchName(69), "A") == 0);
    CHECK(TrackerView::pitchOctave(60) == 4);
    CHECK(TrackerView::pitchOctave(69) == 4);
    CHECK(TrackerView::pitchOctave(0) == -1);
    CHECK(TrackerView::noteText(60) == "C-4");
    CHECK(TrackerView::noteText(61) == "C#4");
    CHECK(TrackerView::noteText(69) == "A-4");
    CHECK(TrackerView::noteText(-1) == "---");
    CHECK(TrackerView::noteText(200) == "---");
    std::printf("PASS tracker note names\n");
  }

  // --- TrackerView: divergence logic + colours -----------------------------
  {
    CHECK(!TrackerView::isDivergent(-1, -1));
    CHECK(!TrackerView::isDivergent(60, 60));
    CHECK(TrackerView::isDivergent(60, 62));
    CHECK(TrackerView::isDivergent(60, -1));
    CHECK(TrackerView::isDivergent(-1, 60));

    TrackerCell silent;
    silent.active = false;
    silent.divergent = false;
    CHECK(vec4Near(TrackerView::cellColor(silent), 0.15f, 0.15f, 0.15f, 1.0f));
    CHECK(std::strcmp(TrackerView::divergenceName(silent), "SILENT") == 0);

    TrackerCell match;
    match.active = true;
    match.divergent = false;
    match.sim_note = 60;
    match.chip_note = 60;
    CHECK(vec4Near(TrackerView::cellColor(match), 0.0f, 1.0f, 0.2f, 1.0f));
    CHECK(std::strcmp(TrackerView::divergenceName(match), "MATCH") == 0);

    TrackerCell missing;
    missing.active = true;
    missing.divergent = true;
    missing.sim_note = 60;
    missing.chip_note = -1;
    CHECK(vec4Near(TrackerView::cellColor(missing), 1.0f, 0.85f, 0.0f, 1.0f));
    CHECK(std::strcmp(TrackerView::divergenceName(missing), "MISSING") == 0);

    TrackerCell clash;
    clash.active = true;
    clash.divergent = true;
    clash.sim_note = 60;
    clash.chip_note = 62;
    CHECK(vec4Near(TrackerView::cellColor(clash), 1.0f, 0.2f, 0.1f, 1.0f));
    CHECK(std::strcmp(TrackerView::divergenceName(clash), "DIVERGED") == 0);
    std::printf("PASS tracker divergence colours\n");
  }

  // --- TrackerView: state comparison ---------------------------------------
  {
    // Matching: sim note 60 on ch 0, chip sounding last_note 60.
    SimState sim;
    sim.driver_frame = 10;
    SimNote n;
    n.channel = 0;
    n.note = 60;
    n.velocity = 100;
    n.active = true;
    n.start_frame = 10;
    sim.notes.push_back(n);

    DeviceSnapshot snap;
    snap.device_id = 11;
    snap.device_name = "GB-APU";
    snap.frame = 10;
    snap.channels.resize(4);
    snap.channels[0].dormant = false;
    snap.channels[0].active = true;
    snap.channels[0].last_note = 60;

    std::vector<TrackerCell> cells = TrackerView::compare(sim, snap, 4);
    CHECK(cells.size() == 4u);
    CHECK(!cells[0].divergent);
    CHECK(cells[0].active);
    CHECK(cells[0].sim_note == 60);
    CHECK(cells[0].chip_note == 60);
    CHECK(!cells[1].active);
    CHECK(!cells[1].divergent);

    // Divergent: chip plays 62 while the driver says 60.
    snap.channels[0].last_note = 62;
    cells = TrackerView::compare(sim, snap, 4);
    CHECK(cells[0].divergent);
    CHECK(cells[0].sim_note == 60);
    CHECK(cells[0].chip_note == 62);

    // Chip-only: no sim notes, chip still sounding -> MISSING.
    SimState empty;
    empty.driver_frame = 11;
    cells = TrackerView::compare(empty, snap, 4);
    CHECK(cells[0].divergent);
    CHECK(cells[0].sim_note == -1);
    CHECK(cells[0].chip_note == 62);

    // Zero/negative channel counts yield no cells.
    CHECK(TrackerView::compare(sim, snap, 0).empty());
    CHECK(TrackerView::compare(sim, snap, -3).empty());

    // Event-list variant: note-on at frame 5, note-off at frame 9.
    std::vector<SimNoteEvent> evs;
    SimNoteEvent on;
    on.frame = 5;
    on.channel = 1;
    on.note = 64;
    on.velocity = 90;
    on.is_note_on = true;
    SimNoteEvent off = on;
    off.frame = 9;
    off.is_note_on = false;
    evs.push_back(on);
    evs.push_back(off);
    DeviceSnapshot snap2;
    snap2.channels.resize(4);
    snap2.channels[1].dormant = false;
    snap2.channels[1].active = true;
    snap2.channels[1].last_note = 64;
    std::vector<TrackerCell> ec =
        TrackerView::compareEvents(evs, 7, snap2, 4);
    CHECK(ec.size() == 4u);
    CHECK(!ec[1].divergent);
    CHECK(ec[1].sim_note == 64);
    // After the note-off the sim side falls silent -> divergent.
    ec = TrackerView::compareEvents(evs, 10, snap2, 4);
    CHECK(ec[1].divergent);
    CHECK(ec[1].sim_note == -1);
    std::printf("PASS tracker state comparison\n");
  }

  // --- Dormant escape + null-safety ----------------------------------------
  {
    const float samples[8] = {0.1f, 0.2f, 0.3f, 0.4f,
                              0.5f, 0.6f, 0.7f, 0.8f};
    Opl3Device odev;
    CHECK(odev.init());
    Opl3Tab otab;
    otab.setOpl3Device(&odev);
    otab.pushWaveformData(3, nullptr, 8);
    otab.pushWaveformData(3, samples, 0);
    otab.pushWaveformData(3, nullptr, 0);
    otab.pushWaveformData(-1, samples, 8);
    otab.pushWaveformData(99, samples, 8);
    CHECK(otab.testWaveSize(3) == 0u);
    CHECK(odev.isVoiceDormant(3));
    otab.pushWaveformData(3, samples, 8);
    CHECK(otab.testWaveSize(3) == 0u);
    odev.voiceKeyOn(3);
    otab.pushWaveformData(3, samples, 8);
    CHECK(otab.testWaveSize(3) == 8u);
    odev.shutdown();

    GbApuDevice gdev;
    CHECK(gdev.init());
    GbApuTab gtab;
    gtab.setGbApuDevice(&gdev);
    gtab.pushWaveformData(1, nullptr, 8);
    gtab.pushWaveformData(1, samples, 0);
    gtab.pushWaveformData(1, nullptr, 0);
    gtab.pushWaveformData(-1, samples, 8);
    gtab.pushWaveformData(99, samples, 8);
    CHECK(gtab.testWaveSize(1) == 0u);
    CHECK(gdev.isDormant(1));
    gtab.pushWaveformData(1, samples, 8);
    CHECK(gtab.testWaveSize(1) == 0u);
    gdev.writeRegister(0xFF16, 0x80);
    gtab.pushWaveformData(1, samples, 8);
    CHECK(gtab.testWaveSize(1) == 8u);
    gdev.shutdown();

    Opl3Tab bare_o;
    bare_o.pushWaveformData(0, nullptr, 8);
    bare_o.pushWaveformData(0, samples, 0);
    GbApuTab bare_g;
    bare_g.pushWaveformData(0, nullptr, 8);
    bare_g.pushWaveformData(0, samples, 0);
    std::printf("PASS dormant escape + null-safety\n");
  }

  // --- Headless draw safety (no ImGui context) ------------------------------
  {
    Opl3Device odev;
    CHECK(odev.init());
    odev.voiceKeyOn(0);
    Opl3Tab otab;
    otab.setOpl3Device(&odev);
    DeviceSnapshot os = odev.snapshot();
    otab.drawChannelStrips(os);
    otab.drawDetail(os);
    otab.drawTracker(os, nullptr);
    SimState sim;
    sim.driver_frame = 3;
    otab.drawTracker(os, &sim);
    odev.shutdown();

    GbApuDevice gdev;
    CHECK(gdev.init());
    gdev.writeRegister(0xFF11, 0x80);
    GbApuTab gtab;
    gtab.setGbApuDevice(&gdev);
    DeviceSnapshot gs = gdev.snapshot();
    gtab.drawChannelStrips(gs);
    gtab.drawDetail(gs);
    gtab.drawTracker(gs, nullptr);
    gtab.drawTracker(gs, &sim);
    gdev.shutdown();

    Opl3Tab bare_o;
    GbApuTab bare_g;
    DeviceSnapshot empty;
    bare_o.drawChannelStrips(empty);
    bare_o.drawDetail(empty);
    bare_o.drawTracker(empty, nullptr);
    bare_g.drawChannelStrips(empty);
    bare_g.drawDetail(empty);
    bare_g.drawTracker(empty, nullptr);

    TrackerView tracker;
    tracker.draw(nullptr, empty, 4);
    tracker.draw(&sim, empty, 4);
    tracker.draw(&sim, empty, 0);
    DeviceSnapshot midi_snap;
    midi_snap.channels.resize(16);
    tracker.draw(&sim, midi_snap, 16);
    std::printf("PASS headless draw safety\n");
  }

  if (g_failures == 0) {
    std::printf("ALL PASS (%d checks)\n", g_checks);
    return 0;
  }
  std::printf("%d/%d checks FAILED\n", g_failures, g_checks);
  return 1;
}
