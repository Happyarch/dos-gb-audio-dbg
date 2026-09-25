// SPDX-License-Identifier: GPL-2.0-or-later
//
// imfc_synth_core.cpp — IBM Music Feature Card (IMFC) synthesis core for dgad.
//
// Adapts the Yamaha FB-01 sound processing state machine and YM2151 FM core
// from dos_port/tools/dosbox-x/src/hardware/imfc.cpp (GPL-2.0-or-later).

#include "devices/imfc_synth_core.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Include the 240 ROM preset binary definitions from DOSBox-X hardware tree
#include "../../../dosbox-x/src/hardware/imfc_rom.c"

namespace audio_dbg {

namespace {

// YM2151 constants & tables
constexpr int EG_ATT = 4, EG_DEC = 3, EG_SUS = 2, EG_REL = 1, EG_OFF = 0;
constexpr int TL_RES_LEN = 256;
constexpr int TL_TAB_LEN = 13 * 2 * TL_RES_LEN;
constexpr int ENV_QUIET = TL_TAB_LEN >> 3;
constexpr int SIN_BITS = 10;
constexpr int SIN_LEN = 1 << SIN_BITS;
constexpr int SIN_MASK = SIN_LEN - 1;
constexpr int MAX_ATT_INDEX = (1 << (16 + 2)) - 1;
constexpr int RATE_STEPS = 8;

struct YM2151Operator {
  uint32_t phase = 0;
  uint32_t freq = 0;
  int32_t volume = MAX_ATT_INDEX;
  int state = EG_OFF;
  uint32_t tl = 0;
  uint32_t mul = 1;
  uint32_t dt1 = 0;
  uint32_t dt2 = 0;
  uint8_t ar = 0;
  uint8_t d1r = 0;
  uint8_t d2r = 0;
  uint8_t rr = 0;
  uint8_t d1l = 0;
  uint8_t ksr = 0;
  uint32_t kc = 0;
  uint32_t kc_i = 768;
  bool am = false;
  uint8_t eg_rate = 0;
  uint8_t eg_shift = 0;

  void key_on(uint8_t /*source*/, uint32_t /*eg_cnt*/) {
    if (state == EG_OFF || state == EG_REL) {
      phase = 0;
      state = EG_ATT;
    }
  }

  void key_off(uint8_t /*source*/) {
    if (state != EG_OFF && state != EG_REL) {
      state = EG_REL;
    }
  }
};

// Attack rate curve table
const uint8_t kEgInc[19 * RATE_STEPS] = {
    0, 1, 0, 1, 0, 1, 0, 1, /*  0 */
    0, 1, 0, 1, 1, 1, 0, 1, /*  1 */
    0, 1, 1, 1, 0, 1, 1, 1, /*  2 */
    0, 1, 1, 1, 1, 1, 1, 1, /*  3 */
    1, 1, 1, 1, 1, 1, 1, 1, /*  4 */
    1, 1, 1, 2, 1, 1, 1, 2, /*  5 */
    1, 2, 1, 2, 1, 2, 1, 2, /*  6 */
    1, 2, 2, 2, 1, 2, 2, 2, /*  7 */
    2, 2, 2, 2, 2, 2, 2, 2, /*  8 */
    2, 2, 2, 4, 2, 2, 2, 4, /*  9 */
    2, 4, 2, 4, 2, 4, 2, 4, /* 10 */
    2, 4, 4, 4, 2, 4, 4, 4, /* 11 */
    4, 4, 4, 4, 4, 4, 4, 4, /* 12 */
    4, 4, 4, 8, 4, 4, 4, 8, /* 13 */
    4, 8, 4, 8, 4, 8, 4, 8, /* 14 */
    4, 8, 8, 8, 4, 8, 8, 8, /* 15 */
    8, 8, 8, 8, 8, 8, 8, 8, /* 16 */
    8, 8, 8, 16, 8, 8, 8, 16, /* 17 */
    8, 16, 8, 16, 8, 16, 8, 16, /* 18 */
};

// Detune tables
const uint32_t kDt2Tab[4] = {0, 384, 500, 608};

const uint8_t kDt1Tab[4 * 32] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2,
    2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 8, 8,
    1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5,
    5, 6, 6, 7, 8, 8, 9, 10, 11, 12, 13, 14, 16, 16, 16, 16,
    2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7,
    8, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 20, 22, 22, 22, 22,
};

// Preset names table for ROM 1..5 harvested from imfc_rom.c
const char* const kImfcPresetNames[5][48] = {
    // ROM 1 (Bank 2)
    {
        "Brass", "Horn", "Trumpet", "LoStrig", "Strings", "Piano", "NewEP", "EGrand",
        "Jazz Gt", "EBass", "WodBass", "EOrgan1", "EOrgan2", "POrgan1", "POrgan2",
        "Flute", "Piccolo", "Oboe", "Clarine", "Glocken", "Vibes", "Xylophn",
        "Koto", "Zither", "Clav", "Harpsic", "Bells", "Harp", "SmadSyn", "Harmoni",
        "SteelDr", "Timpani", "LoStrg2", "Horn Lo", "Whistle", "ZingPlp", "Metal",
        "Heavy", "FunkSyn", "Voices", "Marimba", "EBass 2", "SnareDr", "RD Cymb",
        "Tom Tom", "Mars to", "Storm", "Windbel"
    },
    // ROM 2 (Bank 3)
    {
        "UpPiano", "SPiano", "Piano2", "Piano3", "Piano4", "Piano5", "PhGrand",
        "Grand", "DpGrand", "LPiano1", "LPiano2", "EGrand2", "Honkey1", "Honkey2",
        "Pfbell", "PfVibe", "NewEP2", "NewEP3", "NewEP4", "NewEP5", "EPiano1",
        "EPiano2", "EPiano3", "EPiano4", "EPiano5", "HighTin", "HardTin", "PercPf",
        "WoodPf", "EPStrng", "EPBrass", "Clav2", "Clav3", "Clav4", "FuzzClv",
        "MuteClv", "MuteCl2", "SynClv1", "SynClv2", "SynClv3", "SynClv4", "Harpsi2",
        "Harpsi3", "Harpsi4", "Harpsi5", "Circust", "Celeste", "Squeeze"
    },
    // ROM 3 (Bank 4)
    {
        "Horn2", "Horn3", "Horns", "Flugelh", "Trombon", "Trumpt2", "Brass2",
        "Brass3", "HardBr1", "HardBr2", "HardBr3", "HardBr4", "HuffBrs", "PercBr1",
        "PercBr2", "String1", "String2", "String3", "String4", "SoloVio", "RichSt1",
        "RichSt2", "RichSt3", "RichSt4", "Cello1", "Cello2", "LoStrg3", "LoStrg4",
        "LoStrg5", "Orchest", "5th Str", "Pizzic1", "Pizzic2", "Flute2", "Flute3",
        "Flute4", "Pan Flt", "SlowFlt", "5th Flt", "Oboe2", "Bassoon", "Reed",
        "Harmon2", "Harmon3", "Harmon4", "MonoSax", "Sax 1", "Sax 2"
    },
    // ROM 4 (Bank 5)
    {
        "FnkSyn2", "FnkSyn3", "SynOrgn", "SynFeed", "SynHarm", "SynClar", "SynLead",
        "HuffTak", "SoHeavy", "Hollow", "Schmooh", "MonoSyn", "Cheeky", "SynBell",
        "SynPluk", "EBass3", "RubBass", "SolBass", "PlukBas", "UprtBas", "Fretles",
        "FlapBas", "MonoBas", "SynBas1", "SynBas2", "SynBas3", "SynBas4", "SynBas5",
        "SynBas6", "SynBas7", "Marimb2", "Marimb3", "Xyloph2", "Vibe2", "Vibe3",
        "Glockn2", "TubeBe1", "TubeBe2", "Bells 2", "TempleG", "SteelDr", "ElectDr",
        "Hand Dr", "SynTimp", "Clock", "Heifer", "SnareD2", "SnareD3"
    },
    // ROM 5 (Bank 6)
    {
        "JOrgan1", "JOrgan2", "COrgan1", "COrgan2", "EOrgan3", "EOrgan4", "EOrgan5",
        "EOrgan6", "EOrgan7", "EOrgan8", "SmlPipe", "MidPipe", "BigPipe", "SftPipe",
        "Organ", "Guitar", "Folk Gt", "PluckGt", "BriteGt", "Fuzz Gt", "Zither2",
        "Lute", "Banjo", "SftHarp", "Harp2", "Harp3", "SftKoto", "HitKoto",
        "Sitar1", "Sitar2", "HuffSyn", "Fantasy", "Synvoic", "M.Voice", "VSAR",
        "Racing", "Water", "WildWar", "Ghostie", "Wave", "Space 1", "SpChime",
        "SpTalk", "Winds", "Smash", "Alarm", "Helicop", "SineWav"
    }
};

}  // namespace

// --- Internal Implementation of YM2151 & FB-01 Processor ---------------------

struct ImfcSynthCore::Impl {
  uint32_t sample_rate = ImfcSynthCore::kNativeRate;
  uint8_t master_volume = 127;
  bool memory_protect = false;

  // 8 Instruments (FB-01 architecture)
  std::array<ImfcInstrumentConfig, kChannels> instruments;
  std::array<ImfcVoiceDef, kChannels> active_voices;
  std::array<int, kChannels> active_notes_count = {};
  std::array<int, kChannels> last_played_note = {};

  // RAM banks (Bank 0 = RAM A, Bank 1 = RAM B, 48 voices each)
  std::array<std::array<ImfcVoiceDef, 48>, 2> ram_banks;

  // ROM banks (Banks 2..6 = ROM 1..5)
  std::array<const ImfcVoiceDef*, 5> rom_banks;

  // YM2151 Emulation Core State
  std::array<YM2151Operator, 32> oper;
  std::array<int32_t, 8> chanout = {};
  std::array<int32_t, 16> pan = {}; // L/R pan per channel: pan[2*ch], pan[2*ch+1]
  std::array<uint8_t, 8> fb = {};   // Feedback per channel
  std::array<uint8_t, 8> alg = {};  // Algorithm per channel
  std::array<int32_t, 8> m1_prev = {}; // Feedback buffer for OP4/M1

  // YM2151 lookup tables
  int tl_tab[TL_TAB_LEN] = {};
  uint32_t sin_tab[SIN_LEN] = {};
  uint32_t d1l_tab[16] = {};

  // LFO
  uint32_t lfo_phase = 0;
  uint32_t lfo_freq = 0;
  uint8_t lfo_wsel = 2; // Triangle default
  uint8_t pmd = 0;
  uint8_t amd = 0;

  // SysEx Parser State Machine
  int sysex_state = 0;
  std::vector<uint8_t> sysex_buf;

  void initTables() {
    // Sine table
    for (int i = 0; i < SIN_LEN; ++i) {
      double d = std::sin(i * 2.0 * M_PI / SIN_LEN);
      if (d > 0.0) {
        sin_tab[i] = static_cast<uint32_t>(-std::log(d) / std::log(2.0) * 256.0);
      } else if (d < 0.0) {
        sin_tab[i] = static_cast<uint32_t>(-std::log(-d) / std::log(2.0) * 256.0) | 0x8000;
      } else {
        sin_tab[i] = 0x7FFF;
      }
    }

    // TL table (attenuation table)
    for (int i = 0; i < TL_TAB_LEN; ++i) {
      tl_tab[i] = static_cast<int>(std::pow(2.0, (TL_TAB_LEN - 1 - i) * 13.0 / TL_TAB_LEN));
    }

    // D1L table
    for (int i = 0; i < 15; ++i) {
      d1l_tab[i] = i * 32;
    }
    d1l_tab[15] = 31 * 32;
  }

  void initRomBanks() {
    rom_banks[0] = reinterpret_cast<const ImfcVoiceDef*>(m_voiceDefinitionBankRom1Binary);
    rom_banks[1] = reinterpret_cast<const ImfcVoiceDef*>(m_voiceDefinitionBankRom2Binary);
    rom_banks[2] = reinterpret_cast<const ImfcVoiceDef*>(m_voiceDefinitionBankRom3Binary);
    rom_banks[3] = reinterpret_cast<const ImfcVoiceDef*>(m_voiceDefinitionBankRom4Binary);
    rom_banks[4] = reinterpret_cast<const ImfcVoiceDef*>(m_voiceDefinitionBankRom5Binary);
  }

  void loadVoiceIntoInstrument(int inst, int bank, int prog) {
    if (inst < 0 || inst >= kChannels) return;
    prog = std::clamp(prog, 0, 47);

    const ImfcVoiceDef* vdef = nullptr;
    if (bank >= 0 && bank <= 1) {
      vdef = &ram_banks[bank][prog];
    } else if (bank >= 2 && bank <= 6) {
      vdef = &rom_banks[bank - 2][prog];
    }

    if (vdef != nullptr) {
      active_voices[inst] = *vdef;
      instruments[inst].voiceBankNumber = bank;
      instruments[inst].voiceNumber = prog;
      applyVoiceToYm(inst);
    }
  }

  void applyVoiceToYm(int ch) {
    if (ch < 0 || ch >= 8) return;
    const ImfcVoiceDef& v = active_voices[ch];
    fb[ch] = v.getFeedback();
    alg[ch] = v.getAlgorithm();

    // Map operators OP1..OP4 to YM2151 operators
    for (int op_idx = 0; op_idx < 4; ++op_idx) {
      int yop = ch * 4 + op_idx;
      const ImfcOperatorDef& op = v.operators[op_idx];
      oper[yop].mul = std::max<uint32_t>(1, op.getMultiple());
      oper[yop].tl = op.getTotalLevel();
      oper[yop].ar = op.getAttackRate();
      oper[yop].d1r = op.getDecay1Rate();
      oper[yop].d2r = op.getDecay2Rate();
      oper[yop].rr = op.getReleaseRate();
      oper[yop].d1l = op.getSustainLevel();
      oper[yop].dt1 = op.getDetune();
    }
  }

  void setPan(int ch, uint8_t p) {
    if (ch < 0 || ch >= 8) return;
    instruments[ch].pan = p;
    // 0 = Left only, 64 = Center (L+R), 127 = Right only
    if (p < 32) {
      pan[2 * ch] = ~0;
      pan[2 * ch + 1] = 0;
    } else if (p > 96) {
      pan[2 * ch] = 0;
      pan[2 * ch + 1] = ~0;
    } else {
      pan[2 * ch] = ~0;
      pan[2 * ch + 1] = ~0;
    }
  }

  void handleNoteOn(int ch, int note, int vel) {
    if (ch < 0 || ch >= 8) return;
    if (vel == 0) {
      handleNoteOff(ch, note);
      return;
    }

    last_played_note[ch] = note;
    active_notes_count[ch]++;

    // Calculate YM2151 frequency
    // Standard pitch: f0 = 440 * 2^((note - 69)/12)
    double f = 440.0 * std::pow(2.0, (note - 69.0) / 12.0);
    uint32_t step = static_cast<uint32_t>((f * (1ULL << 32)) / sample_rate);

    for (int op_idx = 0; op_idx < 4; ++op_idx) {
      int yop = ch * 4 + op_idx;
      oper[yop].freq = step * oper[yop].mul;
      oper[yop].key_on(0, 0);
    }
  }

  void handleNoteOff(int ch, int /*note*/) {
    if (ch < 0 || ch >= 8) return;
    if (active_notes_count[ch] > 0) {
      active_notes_count[ch]--;
    }
    if (active_notes_count[ch] == 0) {
      for (int op_idx = 0; op_idx < 4; ++op_idx) {
        int yop = ch * 4 + op_idx;
        oper[yop].key_off(0);
      }
    }
  }

  void handleProgramChange(int ch, int prog) {
    if (ch < 0 || ch >= 8) return;
    loadVoiceIntoInstrument(ch, instruments[ch].voiceBankNumber, prog);
  }

  void handleControlChange(int ch, int cc, int val) {
    if (ch < 0 || ch >= 8) return;
    switch (cc) {
      case 7:  // Volume
        instruments[ch].outputLevel = val & 0x7F;
        break;
      case 10: // Pan
        setPan(ch, val & 0x7F);
        break;
      case 120: // All Sound Off
      case 123: // All Notes Off
        active_notes_count[ch] = 0;
        for (int op_idx = 0; op_idx < 4; ++op_idx) {
          oper[ch * 4 + op_idx].key_off(0);
        }
        break;
      default:
        break;
    }
  }

  void handleSysEx(const uint8_t* msg, size_t len) {
    if (len < 5 || msg[0] != 0xF0 || msg[len - 1] != 0xF7) return;
    if (msg[1] != 0x43) return; // Yamaha ID

    // Yamaha FB-01 SysEx: F0 43 75 0s <cmd...> F7
    if (msg[2] == 0x75) {
      uint8_t cmd_byte = msg[4];

      // Memory Protect: F0 43 75 0s 10 21 <val> F7
      if (cmd_byte == 0x10 && len >= 8 && msg[5] == 0x21) {
        memory_protect = (msg[6] != 0);
        return;
      }

      // Master Volume: F0 43 75 0s 10 24 <val> F7
      if (cmd_byte == 0x10 && len >= 8 && msg[5] == 0x24) {
        master_volume = msg[6] & 0x7F;
        return;
      }

      // Instrument Parameter Changes: F0 43 75 0s (0x18 | inst) <param> <val> F7
      if ((cmd_byte & 0xF8) == 0x18 && len >= 8) {
        int inst = cmd_byte & 0x07;
        uint8_t param = msg[5];
        uint8_t val = msg[6];
        if (inst < kChannels) {
          if (param == 0x04) { // Bank select
            instruments[inst].voiceBankNumber = val & 0x07;
            loadVoiceIntoInstrument(inst, instruments[inst].voiceBankNumber, instruments[inst].voiceNumber);
          } else if (param == 0x05) { // Voice select
            instruments[inst].voiceNumber = val & 0x3F;
            loadVoiceIntoInstrument(inst, instruments[inst].voiceBankNumber, instruments[inst].voiceNumber);
          } else if (param == 0x08) { // Level
            instruments[inst].outputLevel = val & 0x7F;
          } else if (param == 0x09) { // Pan
            setPan(inst, val & 0x7F);
          }
        }
        return;
      }

      // 1-Voice Upload to Instrument: F0 43 75 0s (0x08 | inst) 00 01 00 <128 nibbles> <chk> F7
      if ((cmd_byte & 0xF8) == 0x08 && len >= 137) {
        int inst = cmd_byte & 0x07;
        const uint8_t* nibbles = &msg[8];
        ImfcVoiceDef vdef;
        uint8_t* dest = reinterpret_cast<uint8_t*>(&vdef);
        for (int i = 0; i < 64; ++i) {
          uint8_t lo = nibbles[2 * i] & 0x0F;
          uint8_t hi = nibbles[2 * i + 1] & 0x0F;
          dest[i] = (hi << 4) | lo;
        }
        active_voices[inst] = vdef;
        applyVoiceToYm(inst);
        return;
      }

      // Store Voice to RAM Bank: F0 43 75 0s (0x28 | inst) 0x40 <slot> F7
      if ((cmd_byte & 0xF8) == 0x28 && len >= 8 && msg[5] == 0x40) {
        int inst = cmd_byte & 0x07;
        int slot = msg[6] & 0x7F;
        if (slot < 48) {
          ram_banks[0][slot] = active_voices[inst]; // Bank 0
        } else if (slot < 96) {
          ram_banks[1][slot - 48] = active_voices[inst]; // Bank 1
        }
        return;
      }
    }
  }

  // 4-op FM rendering step for one channel
  int32_t renderOp(int yop, int32_t mod_phase) {
    YM2151Operator& o = oper[yop];
    if (o.state == EG_OFF) return 0;

    // Advance envelope
    if (o.state == EG_ATT) {
      o.volume -= (o.ar + 1) * 32;
      if (o.volume <= 0) {
        o.volume = 0;
        o.state = EG_DEC;
      }
    } else if (o.state == EG_DEC) {
      o.volume += (o.d1r + 1) * 8;
      if (o.volume >= static_cast<int32_t>(d1l_tab[o.d1l])) {
        o.state = EG_SUS;
      }
    } else if (o.state == EG_SUS) {
      o.volume += (o.d2r + 1) * 4;
      if (o.volume >= MAX_ATT_INDEX) {
        o.volume = MAX_ATT_INDEX;
        o.state = EG_OFF;
      }
    } else if (o.state == EG_REL) {
      o.volume += (o.rr + 1) * 32;
      if (o.volume >= MAX_ATT_INDEX) {
        o.volume = MAX_ATT_INDEX;
        o.state = EG_OFF;
      }
    }

    // Phase generator
    o.phase += o.freq;
    uint32_t p = (o.phase >> 22) + mod_phase;
    uint32_t sin_val = sin_tab[p & SIN_MASK];
    int sign = (sin_val & 0x8000) ? -1 : 1;
    uint32_t att = (sin_val & 0x7FFF) + (o.tl << 5) + (o.volume >> 6);
    if (att >= TL_TAB_LEN) return 0;
    return sign * tl_tab[TL_TAB_LEN - 1 - att];
  }

  void renderSample(float& out_l, float& out_r) {
    int32_t mix_l = 0;
    int32_t mix_r = 0;

    for (int ch = 0; ch < 8; ++ch) {
      int base = ch * 4;
      int a = alg[ch];
      int f = fb[ch];

      // Modulator 1 (OP4) with feedback
      int32_t fb_phase = 0;
      if (f > 0) {
        fb_phase = (m1_prev[ch] >> (8 - f));
      }
      int32_t m1 = renderOp(base + 3, fb_phase);
      m1_prev[ch] = m1;

      int32_t out = 0;
      switch (a) {
        case 0: { // 4 -> 3 -> 2 -> 1
          int32_t m2 = renderOp(base + 2, m1 >> 10);
          int32_t m3 = renderOp(base + 1, m2 >> 10);
          out = renderOp(base + 0, m3 >> 10);
          break;
        }
        case 1: { // (4 + 3) -> 2 -> 1
          int32_t m2 = renderOp(base + 2, 0);
          int32_t m3 = renderOp(base + 1, (m1 + m2) >> 10);
          out = renderOp(base + 0, m3 >> 10);
          break;
        }
        case 2: { // 4 + (3 -> 2) -> 1
          int32_t m2 = renderOp(base + 1, renderOp(base + 2, 0) >> 10);
          out = renderOp(base + 0, (m1 + m2) >> 10);
          break;
        }
        case 3: { // (4 -> 3) + (2 -> 1)
          int32_t c1 = renderOp(base + 0, renderOp(base + 1, 0) >> 10);
          int32_t c2 = renderOp(base + 2, m1 >> 10);
          out = c1 + c2;
          break;
        }
        case 4: { // (4 -> 3 -> 2) + 1
          int32_t c1 = renderOp(base + 0, 0);
          int32_t c2 = renderOp(base + 1, renderOp(base + 2, m1 >> 10) >> 10);
          out = c1 + c2;
          break;
        }
        case 5: { // 4 -> (3 + 2 + 1)
          int32_t c1 = renderOp(base + 0, m1 >> 10);
          int32_t c2 = renderOp(base + 1, m1 >> 10);
          int32_t c3 = renderOp(base + 2, m1 >> 10);
          out = c1 + c2 + c3;
          break;
        }
        case 6: { // (4 -> 3) + 2 + 1
          int32_t c1 = renderOp(base + 0, 0);
          int32_t c2 = renderOp(base + 1, 0);
          int32_t c3 = renderOp(base + 2, m1 >> 10);
          out = c1 + c2 + c3;
          break;
        }
        case 7: { // 4 + 3 + 2 + 1 (all parallel carriers)
          out = renderOp(base + 0, 0) + renderOp(base + 1, 0) +
                renderOp(base + 2, 0) + m1;
          break;
        }
        default:
          break;
      }

      chanout[ch] = out;
      int32_t level = instruments[ch].outputLevel;
      int32_t ch_sample = (out * level) >> 7;

      if (pan[2 * ch]) mix_l += ch_sample;
      if (pan[2 * ch + 1]) mix_r += ch_sample;
    }

    // Apply master volume and scale to float [-1.0, 1.0]
    float scale = (master_volume / 127.0f) * (1.0f / 32768.0f);
    out_l = std::clamp(mix_l * scale, -1.0f, 1.0f);
    out_r = std::clamp(mix_r * scale, -1.0f, 1.0f);
  }
};

// --- ImfcSynthCore Public API ------------------------------------------------

ImfcSynthCore::ImfcSynthCore() : impl_(new Impl()) {}

ImfcSynthCore::~ImfcSynthCore() {
  delete impl_;
  impl_ = nullptr;
}

bool ImfcSynthCore::init(std::uint32_t sample_rate) {
  if (impl_ == nullptr) return false;
  impl_->sample_rate = sample_rate == 0 ? kNativeRate : sample_rate;
  impl_->initTables();
  impl_->initRomBanks();
  reset();
  return true;
}

void ImfcSynthCore::reset() {
  if (impl_ == nullptr) return;
  impl_->master_volume = 127;
  impl_->memory_protect = false;

  for (int i = 0; i < kChannels; ++i) {
    impl_->instruments[i] = ImfcInstrumentConfig();
    impl_->instruments[i].midiChannel = i;
    impl_->instruments[i].voiceBankNumber = 2; // ROM 1 default
    impl_->instruments[i].voiceNumber = 0;     // Brass
    impl_->active_notes_count[i] = 0;
    impl_->last_played_note[i] = 0;
    impl_->setPan(i, 64);
    impl_->loadVoiceIntoInstrument(i, 2, 0);
  }

  // Clear operators
  for (auto& o : impl_->oper) {
    o = YM2151Operator();
  }
}

void ImfcSynthCore::sendMidiByte(std::uint8_t byte) {
  if (impl_ == nullptr) return;

  if (byte == 0xF0) {
    impl_->sysex_buf.clear();
    impl_->sysex_buf.push_back(0xF0);
    impl_->sysex_state = 1;
    return;
  }

  if (impl_->sysex_state == 1) {
    impl_->sysex_buf.push_back(byte);
    if (byte == 0xF7) {
      impl_->handleSysEx(impl_->sysex_buf.data(), impl_->sysex_buf.size());
      impl_->sysex_buf.clear();
      impl_->sysex_state = 0;
    }
    return;
  }

  // Handle standard MIDI status bytes
  static uint8_t running_status = 0;
  static uint8_t midi_data[2] = {};
  static int data_bytes_needed = 0;
  static int data_bytes_received = 0;

  if (byte >= 0x80 && byte < 0xF0) {
    running_status = byte;
    data_bytes_received = 0;
    uint8_t cmd = byte & 0xF0;
    data_bytes_needed = (cmd == 0xC0 || cmd == 0xD0) ? 1 : 2;
    return;
  }

  if (byte < 0x80 && running_status != 0) {
    midi_data[data_bytes_received++] = byte;
    if (data_bytes_received == data_bytes_needed) {
      int ch = running_status & 0x0F;
      uint8_t cmd = running_status & 0xF0;
      if (cmd == 0x90) {
        impl_->handleNoteOn(ch, midi_data[0], midi_data[1]);
      } else if (cmd == 0x80) {
        impl_->handleNoteOff(ch, midi_data[0]);
      } else if (cmd == 0xC0) {
        impl_->handleProgramChange(ch, midi_data[0]);
      } else if (cmd == 0xB0) {
        impl_->handleControlChange(ch, midi_data[0], midi_data[1]);
      }
      data_bytes_received = 0;
    }
  }
}

void ImfcSynthCore::sendMidi(const std::uint8_t* msg, std::size_t len) {
  if (msg == nullptr) return;
  for (std::size_t i = 0; i < len; ++i) {
    sendMidiByte(msg[i]);
  }
}

void ImfcSynthCore::renderSamples(float* out_l, float* out_r, std::size_t frames) {
  if (impl_ == nullptr || out_l == nullptr || out_r == nullptr) return;
  for (std::size_t i = 0; i < frames; ++i) {
    impl_->renderSample(out_l[i], out_r[i]);
  }
}

void ImfcSynthCore::renderChannelSamples(int ch, float* out_l, float* out_r, std::size_t frames) {
  if (impl_ == nullptr || out_l == nullptr || out_r == nullptr || ch < 0 || ch >= kChannels) return;
  for (std::size_t i = 0; i < frames; ++i) {
    float l, r;
    impl_->renderSample(l, r);
    float ch_val = impl_->chanout[ch] * (1.0f / 32768.0f);
    out_l[i] = ch_val;
    out_r[i] = ch_val;
  }
}

bool ImfcSynthCore::isInstrumentSounding(int inst) const {
  if (impl_ == nullptr || inst < 0 || inst >= kChannels) return false;
  return impl_->active_notes_count[inst] > 0;
}

int ImfcSynthCore::activeNotes(int inst) const {
  if (impl_ == nullptr || inst < 0 || inst >= kChannels) return 0;
  return impl_->active_notes_count[inst];
}

const ImfcInstrumentConfig& ImfcSynthCore::instrumentConfig(int inst) const {
  static const ImfcInstrumentConfig kDefault;
  if (impl_ == nullptr || inst < 0 || inst >= kChannels) return kDefault;
  return impl_->instruments[inst];
}

const ImfcVoiceDef& ImfcSynthCore::activeVoice(int inst) const {
  static const ImfcVoiceDef kDefault;
  if (impl_ == nullptr || inst < 0 || inst >= kChannels) return kDefault;
  return impl_->active_voices[inst];
}

const ImfcVoiceDef& ImfcSynthCore::customVoice(int bank, int slot) const {
  static const ImfcVoiceDef kDefault;
  if (impl_ == nullptr || bank < 0 || bank > 1 || slot < 0 || slot >= 48) return kDefault;
  return impl_->ram_banks[bank][slot];
}

const char* ImfcSynthCore::presetName(int bank, int prog) const {
  if (prog < 0 || prog >= 48) return "Unknown";
  if (bank >= 2 && bank <= 6) {
    return kImfcPresetNames[bank - 2][prog];
  }
  if (bank >= 0 && bank <= 1 && impl_ != nullptr) {
    return impl_->ram_banks[bank][prog].name;
  }
  return "Custom";
}

uint8_t ImfcSynthCore::masterVolume() const {
  return impl_ ? impl_->master_volume : 127;
}

void ImfcSynthCore::setMasterVolume(uint8_t vol) {
  if (impl_) impl_->master_volume = vol & 0x7F;
}

bool ImfcSynthCore::memoryProtect() const {
  return impl_ ? impl_->memory_protect : false;
}

void ImfcSynthCore::setMemoryProtect(bool protect) {
  if (impl_) impl_->memory_protect = protect;
}

void ImfcSynthCore::setInstrumentBank(int inst, int bank) {
  if (impl_ && inst >= 0 && inst < kChannels) {
    impl_->loadVoiceIntoInstrument(inst, bank, impl_->instruments[inst].voiceNumber);
  }
}

void ImfcSynthCore::setInstrumentProgram(int inst, int prog) {
  if (impl_ && inst >= 0 && inst < kChannels) {
    impl_->loadVoiceIntoInstrument(inst, impl_->instruments[inst].voiceBankNumber, prog);
  }
}

void ImfcSynthCore::setInstrumentLevel(int inst, int level) {
  if (impl_ && inst >= 0 && inst < kChannels) {
    impl_->instruments[inst].outputLevel = level & 0x7F;
  }
}

void ImfcSynthCore::setInstrumentPan(int inst, int pan) {
  if (impl_ && inst >= 0 && inst < kChannels) {
    impl_->setPan(inst, pan & 0x7F);
  }
}

}  // namespace audio_dbg
