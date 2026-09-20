// Stage 8.1/8.2: CliOptions parsing implementation. See cli.h.

#include "cli.h"

#include <cctype>
#include <cstdlib>

namespace audio_dbg {
namespace {

bool startsWith(const std::string& s, const std::string& prefix) {
  return s.size() >= prefix.size() &&
         s.compare(0, prefix.size(), prefix) == 0;
}

// Lowercase + strip '-'/ '_'/ ' ' separators for device matching.
std::string squashed(const std::string& s) {
  std::string out;
  for (char c : s) {
    const auto uc = static_cast<unsigned char>(c);
    if (c == '-' || c == '_' || c == ' ') continue;
    out += static_cast<char>(std::tolower(uc));
  }
  return out;
}

bool parseFrameCount(const std::string& s, std::uint32_t* out) {
  if (s.empty() || out == nullptr) return false;
  for (char c : s) {
    if (std::isdigit(static_cast<unsigned char>(c)) == 0) return false;
  }
  const unsigned long v = std::strtoul(s.c_str(), nullptr, 10);
  if (v == 0 || v > 1000000UL) return false;
  *out = static_cast<std::uint32_t>(v);
  return true;
}

bool parseEnhancementValue(const std::string& s, bool* no_enhancement) {
  const std::string q = squashed(s);
  if (q == "0" || q == "off" || q == "false" || q == "no" || q == "disable" ||
      q == "disabled" || q == "gb" || q == "g") {
    *no_enhancement = true;
    return true;
  }
  if (q == "1" || q == "on" || q == "true" || q == "yes" || q == "enable" ||
      q == "enabled" || q == "e") {
    *no_enhancement = false;
    return true;
  }
  return false;
}

}  // namespace

std::string normalizeDeviceName(const std::string& name) {
  const std::string q = squashed(name);
  if (q == "mt32" || q == "mt-32") return "mt32";
  if (q == "gm" || q == "general" || q == "generalmidi" || q == "midi") {
    return "gm";
  }
  if (q == "opl3") return "opl3";
  if (q == "gbapu" || q == "gbpu" || q == "apu" || q == "gb") return "gbapu";
  return "";
}

bool isKnownDevice(const std::string& name) {
  return !normalizeDeviceName(name).empty();
}

bool parseCliArgs(const std::vector<std::string>& args, CliOptions* out,
                  std::string* err) {
  if (out == nullptr) {
    if (err != nullptr) *err = "null options out-parameter";
    return false;
  }
  CliOptions opt;
  auto fail = [&](const std::string& msg) {
    if (err != nullptr) *err = msg;
    return false;
  };
  // args[0] is the program name; flags start at args[1].
  for (std::size_t i = 1; i < args.size(); ++i) {
    const std::string& a = args[i];
    auto needValue = [&](const char* flag, std::string* dst) -> bool {
      std::string v;
      const std::string eq = std::string(flag) + "=";
      if (startsWith(a, eq)) {
        v = a.substr(eq.size());
      } else if (a == flag) {
        if (i + 1 >= args.size()) {
          if (err != nullptr) {
            *err = std::string("flag ") + flag + " needs a value";
          }
          return false;
        }
        v = args[++i];
      } else {
        return true;  // Not this flag; keep scanning (unreachable here).
      }
      if (v.empty()) {
        if (err != nullptr) {
          *err = std::string("flag ") + flag + " needs a non-empty value";
        }
        return false;
      }
      *dst = v;
      return true;
    };
    if (a == "--help" || a == "-h") {
      opt.show_help = true;
    } else if (a == "--headless") {
      opt.headless = true;
    } else if (a == "--track" || startsWith(a, "--track=")) {
      if (!needValue("--track", &opt.track)) return false;
    } else if (a == "--device" || startsWith(a, "--device=")) {
      std::string v;
      if (!needValue("--device", &v)) return false;
      const std::string canon = normalizeDeviceName(v);
      if (canon.empty()) {
        return fail("unknown --device " + v +
                    " (want mt32, gm, opl3, or gbapu)");
      }
      opt.device = canon;
    } else if (a == "--frames" || startsWith(a, "--frames=")) {
      std::string v;
      if (!needValue("--frames", &v)) return false;
      std::uint32_t n = 0;
      if (!parseFrameCount(v, &n)) {
        return fail("bad --frames value " + v + " (want 1..1000000)");
      }
      opt.frames = n;
      opt.frames_explicit = true;
    } else if (a == "--out" || startsWith(a, "--out=")) {
      if (!needValue("--out", &opt.out_path)) return false;
    } else if (a == "--replay" || startsWith(a, "--replay=")) {
      if (!needValue("--replay", &opt.replay_path)) return false;
      opt.headless = true;  // Replay is a batch render: no window.
    } else if (a == "--no-enh" || a == "--no-enhancement") {
      opt.no_enhancement = true;
    } else if (a == "--enhancement" || startsWith(a, "--enhancement=")) {
      std::string v;
      if (!needValue("--enhancement", &v)) return false;
      if (!parseEnhancementValue(v, &opt.no_enhancement)) {
        return fail("bad --enhancement value " + v + " (want 0 or 1)");
      }
    } else if (startsWith(a, "--")) {
      return fail("unknown flag " + a + " (see --help)");
    } else {
      return fail("unexpected positional argument " + a +
                  " (see --help)");
    }
  }
  *out = opt;
  return true;
}

bool parseCliArgs(int argc, char** argv, CliOptions* out, std::string* err) {
  std::vector<std::string> args;
  for (int i = 0; i < argc; ++i) {
    args.push_back(argv[i] != nullptr ? argv[i] : "");
  }
  return parseCliArgs(args, out, err);
}

std::string cliUsage(const char* prog) {
  const std::string p = (prog != nullptr && prog[0] != 0) ? prog
                                                         : "pkmn-audio-dbg";
  std::string s;
  s += "Usage:\n";
  s += "  " + p + " [options]                interactive GUI (default)\n";
  s += "  " + p +
       " --headless --track <ID> [--device <NAME>] [--frames <N>]\n";
  s += "      [--out <WAV>] [--no-enh]\n";
  s += "  " + p +
       " --replay <file.audiolog> [--device <NAME>] [--frames <N>]\n";
  s += "      [--out <WAV>]\n";
  s += "\nOptions:\n";
  s += "  --help               print this usage and exit.\n";
  s += "  --headless           batch render without SDL window / OpenGL /\n";
  s += "                       ImGui. Required with --track.\n";
  s += "  --track <ID>         track constant or fuzzy name (MUSIC_PALLET_TOWN,\n";
  s += "                       PalletTown, routes1). Resolved via SongCatalog.\n";
  s += "  --device <NAME>      backend: mt32, gm, opl3, gbapu (default mt32).\n";
  s += "  --frames <N>         60 Hz frames to render. Default: the track\n";
  s += "                       length (or the log length for --replay), else\n";
  s += "                       3600 frames (60 s).\n";
  s += "  --out <WAV>          write rendered stereo audio to a WAV file\n";
  s += "                       (native device rate, float32). Without --out\n";
  s += "                       the audio is rendered and discarded.\n";
  s += "  --no-enh             disable the enhancement layer (GB baseline only).\n";
  s += "  --enhancement 0|1    explicit form of the same switch.\n";
  s += "  --replay <log>       replay a captured .audiolog frame-by-frame on\n";
  s += "                       the selected device (implies --headless).\n";
  s += "\nExamples:\n";
  s += "  " + p + " --headless --track MUSIC_PALLET_TOWN --out pal.wav\n";
  s += "  " + p + " --headless --track routes1 --device gm --frames 600\n";
  s += "                       --out r1.wav --no-enh\n";
  s += "  " + p + " --replay take.audiolog --device mt32 --out take.wav\n";
  s += "\nThe GUI remains the default: run with no flags for the interactive\n";
  s += "debugger. pkmn-audio-dbg replaces dos_port/tools/audio/audition.py.\n";
  return s;
}

}  // namespace audio_dbg
