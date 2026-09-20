// Stage 5.3: YAML enhancement watcher, revision manager & runtime compiler.
//
// `EnhancementManager` is the C++ parity of
// `dos_port/tools/audio/audition/revisions.py` plus the runtime side of the
// enhancement pipeline (`yaml_lint.py` resolve + `assets/midi/` baseline):
//   - Watch: monitors `dos_port/tools/audio/enhancements/<Song>.yaml` via
//     `std::filesystem::last_write_time`; `pollForChanges()` reports once
//     per on-disk change and refreshes its cache.
//   - Revisions: snapshots under `dos_port/tools/audio/.revisions/<Song>/`
//     named `{id:04d}_{YYYYmmdd_HHMMSS}_{note}.yaml`; `saveSnapshot()` skips
//     the write when the content is byte-identical to the latest revision
//     (same observable behaviour as revisions.py's SHA-256 comparison).
//     `listRevisions()` parses `(id, timestamp)` exactly like
//     revisions.py (`stem.split("_", 2)`, id = parts[0], ts = parts[1]).
//   - Compile: `compileEnhancement()` shells out to
//     `python3 src/enhancement_dump.py` (which imports `yaml_lint.lint`,
//     so lint and the viewer can never disagree) and converts the resolved
//     frame-domain notes into `SimNoteEvent` on/off pairs.
//   - Baseline: `loadMidiFile()` natively parses standard MIDI files from
//     `dos_port/assets/midi/<target>/` (GB ch1/2/3 -> MIDI 1/2/3, noise
//     ch4 -> MIDI 9) into `SimNoteEvent` lists with exact frame timings
//     (1 tick = 1 frame at the files' 60-division / 1s-quarter tempo;
//     general tempos are scaled, not assumed).
//
// No yaml-cpp dependency: YAML is resolved through the Python bridge, MIDI
// through the header-only SMF parser below.
//
// See docs/current_plan_debug_frontend.md §5 (5.3).

#ifndef PKMN_AUDIO_DBG_ENHANCEMENT_MANAGER_H_
#define PKMN_AUDIO_DBG_ENHANCEMENT_MANAGER_H_

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "session_engine.h"

namespace audio_dbg {

// One stored revision: id from the zero-padded filename prefix, timestamp
// string (YYYYmmdd, same slice revisions.py reports), absolute path.
struct RevisionEntry {
  int id = 0;
  std::string timestamp;
  std::string path;
};

// A complete Roland DT1 SysEx frame, F0..F7. The MT-32 custom-timbre bridge
// and the retained SMF SysEx both speak in these.
using SysExMessages = std::vector<std::vector<std::uint8_t> >;

// Per-song custom-timbre Patch Memory frames: `setup` points patch slots at
// the custom Timbre Memory records, `cleanup` restores the factory occupants.
struct SongTimbreSysex {
  SysExMessages setup;
  SysExMessages cleanup;
};

// A parsed standard MIDI file: the note/program/CC stream plus any embedded
// SysEx frames, retained in encounter order (the SMF reader no longer
// discards them).
struct MidiFileData {
  std::vector<SimNoteEvent> notes;
  SysExMessages sysex;
};

// --- Runtime Python bridge mechanics (header-inline) -----------------------
// `EnhancementManager` and `Mt32Device` share this, and the device-only test
// binaries link mt32_device.cpp without enhancement_manager.cpp, so the
// mechanics MUST stay header-inline to avoid a link-time dependency on the
// manager TU. The wire format is documented in src/enhancement_sysex.py.
namespace sysex_bridge {

// Same walk-up as SongCatalog/EnhancementManager: $PKMN_REPO_ROOT wins, else
// walk up to 12 levels looking for constants/music_constants.asm.
inline std::string findRepoRoot() {
  std::error_code ec;
  if (const char* env = std::getenv("PKMN_REPO_ROOT")) {
    if (std::filesystem::is_regular_file(
            std::filesystem::path(env) / "constants/music_constants.asm", ec)) {
      return env;
    }
  }
  std::filesystem::path dir = std::filesystem::current_path(ec);
  if (ec) return "";
  for (int i = 0; i < 12; ++i) {
    if (std::filesystem::is_regular_file(
            dir / "constants/music_constants.asm", ec)) {
      return dir.string();
    }
    if (!dir.has_parent_path()) break;
    dir = dir.parent_path();
  }
  return "";
}

// Single-quote a shell argument (repo paths contain spaces; embedded single
// quotes use the close-quote / escaped-quote / reopen idiom).
inline std::string shellQuote(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
}

// <repo>/dos_port/tools/dos-gb-audio-dbg/src/enhancement_sysex.py, with the
// legacy viewer path as a fallback. Empty when neither exists.
inline std::string scriptPath(const std::string& repo_root) {
  std::error_code ec;
  std::filesystem::path p =
      std::filesystem::path(repo_root) / "dos_port" / "tools" /
      "dos-gb-audio-dbg" / "src" / "enhancement_sysex.py";
  if (std::filesystem::is_regular_file(p, ec)) return p.string();
  p = std::filesystem::path(repo_root) / "dos_port" / "tools" / "viewer" /
      "src" / "enhancement_sysex.py";
  if (std::filesystem::is_regular_file(p, ec)) return p.string();
  return "";
}

inline bool parseHexByte(const std::string& tok, std::uint8_t* out) {
  if (tok.empty() || tok.size() > 2) return false;
  unsigned v = 0;
  for (char c : tok) {
    int d = -1;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    if (d < 0) return false;
    v = (v << 4) | static_cast<unsigned>(d);
  }
  *out = static_cast<std::uint8_t>(v);
  return true;
}

// Runs `python3 enhancement_sysex.py <mode> [song]` and parses the
// section-delimited SYSEX lines. Either output pointer may be null (that
// section is then ignored). Returns true only on a well-formed run terminated
// by END with every SYSEX frame starting F0 and ending F7.
inline bool run(const std::string& repo_root, const std::string& mode,
                const std::string& song, SysExMessages* timbres,
                SongTimbreSysex* song_sysex) {
  if (timbres != nullptr) timbres->clear();
  if (song_sysex != nullptr) {
    song_sysex->setup.clear();
    song_sysex->cleanup.clear();
  }
  if (repo_root.empty()) return false;
  const std::string script = scriptPath(repo_root);
  if (script.empty()) return false;
  std::string cmd = "python3 " + shellQuote(script) + " " + shellQuote(mode);
  if (!song.empty()) cmd += " " + shellQuote(song);
  cmd += " 2>/dev/null";
  FILE* pipe = ::popen(cmd.c_str(), "r");
  if (pipe == nullptr) return false;

  SysExMessages* section = timbres;
  bool saw_end = false;
  bool bad = false;
  char buf[1024];
  std::string carry;
  const auto handleLine = [&](const std::string& line) {
    std::istringstream ls(line);
    std::string kind;
    ls >> kind;
    if (kind == "TIMBRES") {
      section = timbres;
    } else if (kind == "SETUP") {
      section = song_sysex != nullptr ? &song_sysex->setup : nullptr;
    } else if (kind == "CLEANUP") {
      section = song_sysex != nullptr ? &song_sysex->cleanup : nullptr;
    } else if (kind == "SYSEX") {
      std::vector<std::uint8_t> msg;
      std::string tok;
      bool parsed = true;
      while (ls >> tok) {
        std::uint8_t b = 0;
        if (!parseHexByte(tok, &b)) {
          parsed = false;
          break;
        }
        msg.push_back(b);
      }
      if (parsed && msg.size() >= 2 && msg.front() == 0xF0 &&
          msg.back() == 0xF7 && section != nullptr) {
        section->push_back(std::move(msg));
      } else {
        bad = true;
      }
    } else if (kind == "END") {
      saw_end = true;
    } else if (kind == "ERROR") {
      bad = true;
    }
  };
  while (std::fgets(buf, sizeof(buf), pipe) != nullptr) {
    carry += buf;
    std::size_t nl = 0;
    while ((nl = carry.find('\n')) != std::string::npos) {
      handleLine(carry.substr(0, nl));
      carry.erase(0, nl + 1);
    }
  }
  if (!carry.empty()) handleLine(carry);
  ::pclose(pipe);
  return saw_end && !bad;
}

}  // namespace sysex_bridge

class EnhancementManager {
 public:
  // Auto-discovers the repo root (same walk-up as SongCatalog, probing for
  // constants/music_constants.asm): enhance/revisions dirs default under
  // dos_port/tools/audio/.
  EnhancementManager();
  explicit EnhancementManager(const std::string& repo_root);
  // Fully explicit (tests point these at a temp dir).
  EnhancementManager(const std::string& enhance_dir,
                     const std::string& revisions_dir,
                     const std::string& repo_root);

  void setEnhanceDir(const std::string& dir) { enhance_dir_ = dir; }
  void setRevisionsDir(const std::string& dir) { revisions_dir_ = dir; }
  void setRepoRoot(const std::string& root) { repo_root_ = root; }
  const std::string& enhanceDir() const { return enhance_dir_; }
  const std::string& revisionsDir() const { return revisions_dir_; }
  const std::string& repoRoot() const { return repo_root_; }

  // --- Watcher ---
  // Starts (or re-targets) mtime monitoring of <Song>.yaml. Caches the
  // current state, so the first poll after watchSong() reports no change.
  void watchSong(const std::string& song);
  const std::string& watchedSong() const { return watched_song_; }
  std::string watchedPath() const;
  // True exactly once per on-disk change (content mtime flip, or the file
  // appearing/disappearing); refreshes the cache as a side effect.
  bool pollForChanges();

  // --- Revisions (revisions.py parity) ---
  // Writes {id:04d}_{ts}_{note}.yaml unless `content` is byte-identical to
  // the latest revision (then returns the existing id/path, no new file).
  // Returns (revision id, absolute path).
  std::pair<int, std::string> saveSnapshot(const std::string& song,
                                           const std::string& content,
                                           const std::string& note = "");
  // Sorted by id ascending. Malformed filenames are skipped.
  std::vector<RevisionEntry> listRevisions(const std::string& song) const;
  // (found, content). Missing id -> (false, "").
  std::pair<bool, std::string> getRevisionContent(const std::string& song,
                                                 int rev_id) const;
  // Copies the revision back over enhancements/<Song>.yaml. False when the
  // id does not exist. Refreshes the watch cache when it touches the
  // watched file.
  bool revertToRevision(const std::string& song, int rev_id);

  // (found, content) of the live enhancements/<Song>.yaml.
  std::pair<bool, std::string> loadYamlFile(const std::string& song) const;

  // --- Compiler / loaders -> SimNoteEvent lists (sorted by frame) ---
  // Resolved enhancement notes via the python bridge (yaml_lint.lint).
  // Empty when the file is absent, fails lint, or python3 is unavailable.
  std::vector<SimNoteEvent> compileEnhancement(
      const std::string& song, const std::string& target = "mt32") const;
  // Native SMF type-0/1 parse: note-ons become on/off pairs with exact
  // frame timings. Empty on any parse failure.
  std::vector<SimNoteEvent> loadMidiFile(const std::string& path) const;
  // Same parse, additionally returning the embedded SysEx frames the reader
  // used to skip (retained in encounter order).
  MidiFileData parseMidiFile(const std::string& path) const;
  // assets/midi/<target>/<Song>.mid relative to the repo root.
  std::vector<SimNoteEvent> loadSongBaseline(
      const std::string& song, const std::string& target = "mt32") const;
  std::string midiPathFor(const std::string& song,
                          const std::string& target = "mt32") const;

  // --- MT-32 custom-timbre bridge (delegates to enhancement_sysex.py) ------
  // The Timbre Memory upload blob from tools/audio/mt32/timbres.yaml via
  // gen_mt32_patches.build_messages (system=False -- the host --setup path;
  // the channel-table system write is a full-boot upload MUNT mishandles
  // live). Sent once at Mt32Device::init(). Empty when the bridge/python3 or
  // the repo is unavailable.
  SysExMessages loadTimbreBank() const;
  // Per-song Patch Memory setup + factory-restore cleanup via
  // midi_to_stream.find_song_custom_patches + build_song_sysex, keyed by the
  // pret header label (e.g. "Music_JigglypuffSong"). Both empty when the song
  // uses no custom timbres (the cleanup of the PREVIOUS track is what clears
  // the synth for a no-custom track).
  SongTimbreSysex loadSongTimbreSysex(const std::string& song) const;

 private:
  std::string revisionDirFor(const std::string& song) const;
  std::string yamlPathFor(const std::string& song) const;
  void refreshWatchCache();

  std::string enhance_dir_;
  std::string revisions_dir_;
  std::string repo_root_;
  std::string watched_song_;
  // Cached watcher state: whether the file existed + its last mtime.
  bool watch_have_stamp_ = false;
  bool watch_existed_ = false;
  std::uint64_t watch_mtime_ns_ = 0;
};

}  // namespace audio_dbg

#endif  // PKMN_AUDIO_DBG_ENHANCEMENT_MANAGER_H_
