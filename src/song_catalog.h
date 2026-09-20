// Stage 5.1: Song catalog — pret constants & header parser, fuzzy track finder.
//
// `SongCatalog` parses the real repository data (no hand-kept track list):
//   - `constants/music_constants.asm`: `music_const CONSTANT, HeaderLabel`
//     pairs (track ID order).
//   - `audio/headers/*.asm` (`musicheadersN`, `sfxheadersN`): per-header
//     `channel_count`, `channel` header pointers; the sound bank is the
//     trailing digit of the header file name (AUDIO_1..AUDIO_4).
//
// The repository root is auto-discovered: `$PKMN_REPO_ROOT` wins, otherwise
// the current working directory is walked upward until
// `constants/music_constants.asm` is found (covers both the `viewer/` source
// dir and an out-of-source `build/` dir as CWD).
//
// `findTrack()` resolves in layers: exact constant, exact header,
// case-insensitive exact, normalized-stem exact (case/underscore-insensitive
// with the `MUSIC_`/`Music_` prefix stripped), substring on the normalized
// stem, then a Levenshtein fallback (handles e.g. `route1` vs `ROUTES1`).
//
// See docs/current_plan_debug_frontend.md §5 (5.1).

#ifndef PKMN_AUDIO_DBG_SONG_CATALOG_H_
#define PKMN_AUDIO_DBG_SONG_CATALOG_H_

#include <cstddef>
#include <string>
#include <vector>

namespace audio_dbg {

// One playable/simulatable track: a pret music constant bound to its header.
struct SongInfo {
  std::string constant_name;  // e.g. "MUSIC_PALLET_TOWN".
  std::string header_label;   // e.g. "Music_PalletTown".
  int bank = 0;               // Sound bank 1..4 (0 = header not found).
  int channel_count = 0;      // From the header's `channel_count`.
  std::string file_path;      // Repo-relative header file, e.g.
                              // "audio/headers/musicheaders1.asm".
  std::vector<std::string> channels;  // `channel N, Label` pointers.
};

class SongCatalog {
 public:
  // Auto-discovers the repo root (see above). An empty catalog (no root
  // found) is valid: tracks() is empty and findTrack() returns nullptr.
  SongCatalog();
  explicit SongCatalog(const std::string& repo_root);

  const std::vector<SongInfo>& tracks() const { return tracks_; }
  std::size_t size() const { return tracks_.size(); }
  bool empty() const { return tracks_.empty(); }
  const std::string& repoRoot() const { return repo_root_; }

  // Fuzzy/normalized resolution. Returns nullptr on empty query or no match.
  // The returned pointer stays valid for the catalog's lifetime.
  const SongInfo* findTrack(const std::string& query) const;

  // Normalization helper (lowercase, alnum-only). Public for tests.
  static std::string normalize(const std::string& s);

 private:
  void load();
  void parseConstants(const std::string& path);

  std::string repo_root_;
  std::vector<SongInfo> tracks_;
};

}  // namespace audio_dbg

#endif  // PKMN_AUDIO_DBG_SONG_CATALOG_H_
