// Stage 8.1: Headless CLI option parsing for pkmn-audio-dbg.
//
// `CliOptions` + `parseCliArgs()` cover the batch-rendering flags:
//   --help             print usage and exit 0.
//   --headless         run without SDL window / OpenGL / ImGui.
//   --track <ID>       track constant or fuzzy name (MUSIC_PALLET_TOWN,
//                      PalletTown, routes1, ...; resolved via SongCatalog).
//   --device <NAME>    backend select: mt32, gm, opl3, gbapu (default mt32).
//   --frames <N>       60 Hz frames to render (default: track total or 3600).
//   --out <WAV>        write rendered stereo audio to a WAV file.
//   --no-enh           disable the enhancement layer (GB baseline only).
//   --enhancement 0|1  same switch in explicit form (0/off/false/no disable).
//   --replay <log>     replay a captured .audiolog (implies --headless).
//   --project <name>   select the active project from the config file.
//   --project-dir <p>  override the active project root directory.
//   --overrides <path> explicit overrides directory (wins over config).
//   --enhancements <p> explicit enhancements directory (wins over config).
//   --constants <path> explicit constants file (wins over config).
//
// Pure parsing (no SDL, no audio hardware) so tests link it headless.
// See docs/current_plan_debug_frontend.md §8 (8.1/8.2).

#ifndef PKMN_AUDIO_DBG_CLI_H_
#define PKMN_AUDIO_DBG_CLI_H_

#include <cstdint>
#include <string>
#include <vector>

namespace audio_dbg {

// Default frame budget when no track/log implies a length (60 s at 60 Hz).
constexpr std::uint32_t kCliDefaultFrames = 3600;

struct CliOptions {
  bool show_help = false;
  bool headless = false;
  std::string track;
  std::string device = "mt32";
  bool frames_explicit = false;
  std::uint32_t frames = 0;
  std::string out_path;
  std::string replay_path;
  // True = GB baseline only (enhancement overlay off).
  bool no_enhancement = false;
  // True = disable Sound Blaster DMA PCM for SFX (force FM fallback).
  bool no_dma = false;
  // Multi-project configuration (config engine). Empty = use config file.
  std::string project;        // --project <name>
  std::string project_dir;    // --project-dir <path>
  std::string overrides;      // --overrides <path>
  std::string enhancements;   // --enhancements <path>
  std::string constants;      // --constants <path>
  // Directory-style aliases, kept in sync by parseCliArgs for callers
  // that prefer explicit suffixes.
  std::string project_root;      // alias of project_dir
  std::string overrides_dir;     // alias of overrides
  std::string enhancements_dir;  // alias of enhancements
  std::string constants_path;    // alias of constants
};

// Parses argv (argv[0] = program name, skipped). Returns true on success;
// on false, *err (when non-null) holds the human-readable reason.
bool parseCliArgs(const std::vector<std::string>& args, CliOptions* out,
                  std::string* err);
bool parseCliArgs(int argc, char** argv, CliOptions* out, std::string* err);

// Usage text for --help (prog = argv[0] or binary name).
std::string cliUsage(const char* prog);

// Canonical device key ("mt32", "gm", "opl3", "gbapu") or "" when unknown.
// Accepts case-insensitive spellings with -/_ separators ("GB-APU", "gb_apu").
std::string normalizeDeviceName(const std::string& name);
bool isKnownDevice(const std::string& name);

}  // namespace audio_dbg

#endif  // PKMN_AUDIO_DBG_CLI_H_
