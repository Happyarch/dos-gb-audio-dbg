// Stage 7.1/7.2/7.5: Acceptance tests for the MIDI concrete tabs.
//
// Verifies (headless: no GUI, no audio hardware):
//   Mt32Tab:
//     1. 9-part strip layout + factory channel mapping (parts 1-8 on
//        0-based channels 1-8, rhythm on 9, channel 0 part-less) via
//        partChannel()/partLabel()/partStrip().
//     2. programName() timbre resolution (anchors + clamp behaviour).
//     3. Partial matrix: partialColor() for states 0..3 matches the
//        spec palette, partialState() bounds, activePartialCount()
//        dormancy + post-note allocation.
//     4. LCD string: non-empty, 20 chars in mock mode, Roland display
//        SysEx updates its prefix.
//   GmTab:
//     1. 16-channel strip layout + drum marker on channel 10 via
//        channelLabel()/channelStrip().
//     2. programName() GM resolution (anchors + clamp behaviour).
//     3. Mute/solo toggling through onMuteClick()/onSoloClick().
//     4. Volume/pan readouts track CC7/CC10.
//   Both tabs:
//     1. Dormant escape in pushWaveformData() (dormant stores nothing,
//        post-wake stores all) + null-safety (nullptr/zero-length).
//     2. draw*() headless safety (no ImGui context: must not crash).
// Exits 0 with ALL PASS on success, 1 on any failure.

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "devices/gm_device.h"
#include "devices/mt32_device.h"
#include "tabs/gm_tab.h"
#include "tabs/midi_tab.h"
#include "tabs/mt32_tab.h"

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

  // Force mock synthesis so partial/LCD/telemetry assertions are
  // deterministic on any machine (no ROMs / SoundFonts required).
  ::setenv("MT32_ROM_DIR", "none", 1);
  ::setenv("SOUNDFONT", "none", 1);

  // --- Mt32Tab: 9-part layout + factory channel mapping ------------------
  {
    CHECK(Mt32Tab::kParts == 9);
    CHECK(Mt32Tab::kPartialSlots == 32);
    for (int part = 0; part < 8; ++part) {
      CHECK(Mt32Tab::partChannel(part) == part + 1);
    }
    CHECK(Mt32Tab::partChannel(8) == 9);
    CHECK(Mt32Tab::partChannel(-1) == -1);
    CHECK(Mt32Tab::partChannel(9) == -1);
    CHECK(Mt32Tab::partLabel(0) == "Part 1 [Ch 2]");
    CHECK(Mt32Tab::partLabel(7) == "Part 8 [Ch 9]");
    CHECK(Mt32Tab::partLabel(8) == "Rhythm [Ch 10]");
    CHECK(Mt32Tab::partLabel(-1) == "Invalid");
    CHECK(Mt32Tab::partLabel(9) == "Invalid");

    Mt32Device dev;
    CHECK(dev.init());
    CHECK(dev.isMock());
    Mt32Tab tab;
    tab.setMt32Device(&dev);
    CHECK(tab.mt32Device() == &dev);

    DeviceSnapshot s = dev.snapshot();
    // Fresh device: every part dormant, nothing sounding.
    for (int part = 0; part < 9; ++part) {
      Mt32Tab::PartStrip ps = tab.partStrip(s, part);
      CHECK(ps.part == part);
      CHECK(ps.channel == Mt32Tab::partChannel(part));
      CHECK(ps.dormant);
      CHECK(!ps.active);
      CHECK(ps.sounding == 0u);
    }
    // Out-of-range part: channel -1, dormant default.
    {
      Mt32Tab::PartStrip ps = tab.partStrip(s, 99);
      CHECK(ps.channel == -1);
    }

    // Note on part 1's channel wakes that part only.
    CHECK(dev.noteOn(1, 60, 100));
    s = dev.snapshot();
    {
      Mt32Tab::PartStrip p0 = tab.partStrip(s, 0);
      CHECK(!p0.dormant);
      CHECK(p0.sounding == 1u);
      CHECK(p0.active);
      Mt32Tab::PartStrip p1 = tab.partStrip(s, 1);
      CHECK(p1.dormant);
      CHECK(!p1.active);
      Mt32Tab::PartStrip pr = tab.partStrip(s, 8);
      CHECK(pr.channel == 9);
      CHECK(pr.dormant);
    }
    // Rhythm part wakes on its own channel.
    CHECK(dev.noteOn(9, 36, 110));
    s = dev.snapshot();
    CHECK(tab.partStrip(s, 8).active);
    CHECK(tab.partStrip(s, 8).sounding == 1u);
    dev.noteOff(1, 60);
    dev.noteOff(9, 36);
    dev.shutdown();
    std::printf("PASS mt32 9-part layout\n");
  }

  // --- Mt32Tab: programName() timbre resolution --------------------------
  // Factory bank order (mt32_presets.py MT32_FACTORY) — NOT the GM list.
  {
    Mt32Tab tab;
    CHECK(std::strcmp(tab.programName(0), "Acou Piano 1") == 0);
    CHECK(std::strcmp(tab.programName(7), "Honkytonk") == 0);
    CHECK(std::strcmp(tab.programName(8), "Elec Org 1") == 0);
    CHECK(std::strcmp(tab.programName(38), "Warm Bell") == 0);
    CHECK(std::strcmp(tab.programName(47), "Square Wave") == 0);
    CHECK(std::strcmp(tab.programName(51), "Pizzicato") == 0);
    CHECK(std::strcmp(tab.programName(57), "Harp 1") == 0);
    CHECK(std::strcmp(tab.programName(80), "Sax 3") == 0);
    CHECK(std::strcmp(tab.programName(105), "Koto") == 0);
    // Clamp behaviour matches Mt32Device::mt32TimbreName.
    CHECK(tab.programName(-5) == tab.programName(0));
    CHECK(tab.programName(200) == tab.programName(127));
    CHECK(std::strlen(tab.programName(3)) > 0);
    std::printf("PASS mt32 program names\n");
  }

  // --- Mt32Tab: partial matrix states + count -----------------------------
  {
    // Spec palette (ImGui-free, no context needed).
    CHECK(vec4Near(Mt32Tab::partialColor(0), 0.15f, 0.15f, 0.15f, 1.0f));
    CHECK(vec4Near(Mt32Tab::partialColor(1), 1.0f, 0.35f, 0.0f, 1.0f));
    CHECK(vec4Near(Mt32Tab::partialColor(2), 0.0f, 1.0f, 0.2f, 1.0f));
    CHECK(vec4Near(Mt32Tab::partialColor(3), 1.0f, 0.85f, 0.0f, 1.0f));
    CHECK(std::strcmp(Mt32Tab::partialStateName(0), "INACTIVE") == 0);
    CHECK(std::strcmp(Mt32Tab::partialStateName(1), "ATTACK") == 0);
    CHECK(std::strcmp(Mt32Tab::partialStateName(2), "SUSTAIN") == 0);
    CHECK(std::strcmp(Mt32Tab::partialStateName(3), "RELEASE") == 0);
    // Null draw-list primitive: must not crash.
    Mt32Tab::drawPartialCell(nullptr, ImVec2(0, 0), 10.0f, 2);
    MidiTab::drawNoteBar(nullptr, ImVec2(0, 0), 10.0f, 4.0f, 100, true);

    Mt32Tab bare;
    CHECK(bare.activePartialCount() == -1);  // No device attached.

    Mt32Device dev;
    CHECK(dev.init());
    Mt32Tab tab;
    tab.setMt32Device(&dev);
    CHECK(tab.activePartialCount() == 0);
    CHECK(dev.partialState(-1) == -1);
    CHECK(dev.partialState(32) == -1);
    for (int slot = 0; slot < 32; ++slot) {
      const int st = dev.partialState(slot);
      CHECK(st >= 0 && st <= 3);
    }
    CHECK(dev.noteOn(1, 60, 100));
    CHECK(tab.activePartialCount() == 1);
    CHECK(dev.partialState(0) == 2);  // Mock: SUSTAIN for live slots.
    dev.noteOff(1, 60);
    dev.reset();
    CHECK(tab.activePartialCount() == 0);
    dev.shutdown();
    std::printf("PASS mt32 partial matrix\n");
  }

  // --- Mt32Tab: LCD string -------------------------------------------------
  {
    Mt32Device dev;
    CHECK(dev.init());
    Mt32Tab tab;
    tab.setMt32Device(&dev);
    CHECK(!tab.lcdText().empty());
    CHECK(tab.lcdText().size() == Mt32Device::kLcdChars);
    const std::uint8_t sysex[] = {
        0xF0, 0x41, 0x10, 0x16, 0x12, 0x20, 0x00, 0x00, 'H', 'E',
        'L',  'L',  'O',  ' ',  'M',  'T',  '-',  '3', '2', ' ',
        ' ',  ' ',  ' ',  ' ',  ' ',  ' ',  ' ',  ' ', 0x00, 0xF7};
    dev.dispatchSysEx(sysex, sizeof(sysex));
    CHECK(tab.lcdText().substr(0, 12) == "HELLO MT-32 ");
    Mt32Tab bare;
    CHECK(bare.lcdText().empty());
    dev.shutdown();
    std::printf("PASS mt32 lcd\n");
  }

  // --- Mt32Tab: mute toggling ----------------------------------------------
  {
    Mt32Device dev;
    CHECK(dev.init());
    Mt32Tab tab;
    tab.setMt32Device(&dev);
    CHECK(!dev.snapshot().channels[1].muted);
    tab.onMuteClick(1);
    CHECK(dev.snapshot().channels[1].muted);
    tab.onMuteClick(1);
    CHECK(!dev.snapshot().channels[1].muted);
    // Part-addressed mute maps onto the part's channel.
    tab.onPartMuteClick(0);
    CHECK(dev.snapshot().channels[1].muted);
    tab.onPartMuteClick(0);
    CHECK(!dev.snapshot().channels[1].muted);
    tab.onPartMuteClick(8);  // Rhythm -> channel 9.
    CHECK(dev.snapshot().channels[9].muted);
    tab.onPartMuteClick(8);
    CHECK(!dev.snapshot().channels[9].muted);
    // Out-of-range clicks are safe no-ops.
    tab.onMuteClick(-1);
    tab.onMuteClick(16);
    tab.onPartMuteClick(-1);
    tab.onPartMuteClick(9);
    Mt32Tab bare;
    bare.onMuteClick(1);  // No device: must not crash.
    bare.onPartMuteClick(0);
    dev.shutdown();
    std::printf("PASS mt32 mute toggling\n");
  }

  // --- GmTab: 16-channel layout + drum marker -------------------------------
  {
    CHECK(GmTab::kChannels == 16);
    CHECK(GmTab::kDrumChannel == 9);
    CHECK(GmTab::channelLabel(0) == "Ch 1");
    CHECK(GmTab::channelLabel(9) == "Ch 10 [Drums]");
    CHECK(GmTab::channelLabel(15) == "Ch 16");
    CHECK(GmTab::channelLabel(-1) == "Invalid");
    CHECK(GmTab::channelLabel(16) == "Invalid");

    GmDevice dev;
    CHECK(dev.init());
    GmTab tab;
    tab.setGmDevice(&dev);
    CHECK(tab.gmDevice() == &dev);

    DeviceSnapshot s = dev.snapshot();
    for (int ch = 0; ch < 16; ++ch) {
      GmTab::ChannelStrip cs = tab.channelStrip(s, ch);
      CHECK(cs.channel == ch);
      CHECK(cs.dormant);
      CHECK(cs.sounding == 0u);
      CHECK(!cs.muted && !cs.soloed);
    }
    {
      GmTab::ChannelStrip cs = tab.channelStrip(s, 99);
      CHECK(cs.channel == 99);
    }

    CHECK(dev.noteOn(0, 69, 100));
    dev.dispatchProgramChange(0, 40);
    s = dev.snapshot();
    {
      GmTab::ChannelStrip cs = tab.channelStrip(s, 0);
      CHECK(!cs.dormant);
      CHECK(cs.sounding == 1u);
      CHECK(cs.program == 40);
      // Default CC7=100 -> vol, CC10=64 -> centre pan.
      CHECK(std::fabs(cs.volume - 100.0f / 127.0f) < 1e-5f);
      CHECK(std::fabs(cs.pan) < 1e-5f);
    }
    dev.dispatchControlChange(0, 7, 64);
    dev.dispatchControlChange(0, 10, 0);
    s = dev.snapshot();
    {
      GmTab::ChannelStrip cs = tab.channelStrip(s, 0);
      CHECK(std::fabs(cs.volume - 64.0f / 127.0f) < 1e-5f);
      CHECK(std::fabs(cs.pan - (0.0f - 64.0f) / 64.0f) < 1e-5f);
    }
    dev.noteOff(0, 69);
    dev.shutdown();
    std::printf("PASS gm 16-channel layout\n");
  }

  // --- GmTab: programName() GM resolution -----------------------------------
  {
    GmTab tab;
    CHECK(std::strcmp(tab.programName(0), "Acoustic Grand Piano") == 0);
    CHECK(std::strcmp(tab.programName(40), "Violin") == 0);
    CHECK(std::strcmp(tab.programName(127), "Gunshot") == 0);
    CHECK(tab.programName(-5) == tab.programName(0));
    CHECK(tab.programName(200) == tab.programName(127));
    std::printf("PASS gm program names\n");
  }

  // --- GmTab: mute/solo toggling --------------------------------------------
  {
    GmDevice dev;
    CHECK(dev.init());
    GmTab tab;
    tab.setGmDevice(&dev);
    CHECK(!dev.snapshot().channels[2].muted);
    tab.onMuteClick(2);
    CHECK(dev.snapshot().channels[2].muted);
    tab.onMuteClick(2);
    CHECK(!dev.snapshot().channels[2].muted);
    CHECK(!dev.snapshot().channels[4].soloed);
    tab.onSoloClick(4);
    CHECK(dev.snapshot().channels[4].soloed);
    tab.onSoloClick(4);
    CHECK(!dev.snapshot().channels[4].soloed);
    // Out-of-range clicks are safe no-ops.
    tab.onMuteClick(-1);
    tab.onMuteClick(16);
    tab.onSoloClick(-1);
    tab.onSoloClick(16);
    GmTab bare;
    bare.onMuteClick(1);
    bare.onSoloClick(1);
    dev.shutdown();
    std::printf("PASS gm mute/solo toggling\n");
  }

  // --- Both tabs: dormant escape + null-safety in pushWaveformData -----------
  {
    const float samples[8] = {0.1f, 0.2f, 0.3f, 0.4f,
                              0.5f, 0.6f, 0.7f, 0.8f};

    Mt32Device mdev;
    CHECK(mdev.init());
    Mt32Tab mtab;
    mtab.setMt32Device(&mdev);
    // Null-safety: none of these may crash or store.
    mtab.pushWaveformData(1, nullptr, 8);
    mtab.pushWaveformData(1, samples, 0);
    mtab.pushWaveformData(1, nullptr, 0);
    mtab.pushWaveformData(-1, samples, 8);
    mtab.pushWaveformData(99, samples, 8);
    CHECK(mtab.testWaveSize(1) == 0u);
    // Dormant escape.
    CHECK(mdev.isDormant(1));
    mtab.pushWaveformData(1, samples, 8);
    CHECK(mtab.testWaveSize(1) == 0u);
    CHECK(mdev.noteOn(1, 60, 100));
    mtab.pushWaveformData(1, samples, 8);
    CHECK(mtab.testWaveSize(1) == 8u);
    mdev.shutdown();

    GmDevice gdev;
    CHECK(gdev.init());
    GmTab gtab;
    gtab.setGmDevice(&gdev);
    gtab.pushWaveformData(2, nullptr, 8);
    gtab.pushWaveformData(2, samples, 0);
    gtab.pushWaveformData(2, nullptr, 0);
    gtab.pushWaveformData(-1, samples, 8);
    gtab.pushWaveformData(99, samples, 8);
    CHECK(gtab.testWaveSize(2) == 0u);
    CHECK(gdev.isDormant(2));
    gtab.pushWaveformData(2, samples, 8);
    CHECK(gtab.testWaveSize(2) == 0u);
    CHECK(gdev.noteOn(2, 64, 110));
    gtab.pushWaveformData(2, samples, 8);
    CHECK(gtab.testWaveSize(2) == 8u);
    gdev.shutdown();

    // Detached tabs: null device still null-safe.
    Mt32Tab bare_mt;
    bare_mt.pushWaveformData(0, nullptr, 8);
    bare_mt.pushWaveformData(0, samples, 0);
    GmTab bare_gm;
    bare_gm.pushWaveformData(0, nullptr, 8);
    bare_gm.pushWaveformData(0, samples, 0);
    std::printf("PASS dormant escape + null-safety\n");
  }

  // --- Both tabs: headless draw safety (no ImGui context) --------------------
  {
    Mt32Device mdev;
    CHECK(mdev.init());
    CHECK(mdev.noteOn(1, 60, 100));
    Mt32Tab mtab;
    mtab.setMt32Device(&mdev);
    DeviceSnapshot ms = mdev.snapshot();
    mtab.drawChannelStrips(ms);  // Early-return without a context.
    mtab.drawDetail(ms);
    mtab.drawTracker(ms, nullptr);
    SimState sim;
    sim.driver_frame = 3;
    mtab.drawTracker(ms, &sim);
    mdev.shutdown();

    GmDevice gdev;
    CHECK(gdev.init());
    CHECK(gdev.noteOn(0, 69, 100));
    GmTab gtab;
    gtab.setGmDevice(&gdev);
    DeviceSnapshot gs = gdev.snapshot();
    gtab.drawChannelStrips(gs);
    gtab.drawDetail(gs);
    gtab.drawTracker(gs, nullptr);
    gtab.drawTracker(gs, &sim);
    gdev.shutdown();

    // Detached tabs draw disabled placeholders, never crash.
    Mt32Tab bare_mt;
    GmTab bare_gm;
    DeviceSnapshot empty;
    bare_mt.drawChannelStrips(empty);
    bare_mt.drawDetail(empty);
    bare_mt.drawTracker(empty, nullptr);
    bare_gm.drawChannelStrips(empty);
    bare_gm.drawDetail(empty);
    bare_gm.drawTracker(empty, nullptr);
    std::printf("PASS headless draw safety\n");
  }

  if (g_failures == 0) {
    std::printf("ALL PASS (%d checks)\n", g_checks);
    return 0;
  }
  std::printf("%d/%d checks FAILED\n", g_failures, g_checks);
  return 1;
}
