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
  // assets/midi/<target>/<Song>.mid relative to the repo root.
  std::vector<SimNoteEvent> loadSongBaseline(
      const std::string& song, const std::string& target = "mt32") const;
  std::string midiPathFor(const std::string& song,
                          const std::string& target = "mt32") const;

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
