// Stage 5.1: SongCatalog implementation. See song_catalog.h for the contract.

#include "song_catalog.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <string>
#include <utility>
#include <vector>

namespace audio_dbg {
namespace {

namespace fs = std::filesystem;

// Header file names are discovered by scanning audio/headers/*.asm;
// the bank is the trailing digit (musicheadersN / sfxheadersN map to
// AUDIO_1..AUDIO_4).

constexpr const char* kConstantsFile = "constants/music_constants.asm";

std::string findRepoRoot() {
  if (const char* env = std::getenv("PKMN_REPO_ROOT")) {
    const fs::path p = fs::path(env) / kConstantsFile;
    std::error_code ec;
    if (fs::is_regular_file(p, ec)) return env;
  }
  std::error_code ec;
  fs::path dir = fs::current_path(ec);
  if (ec) return "";
  for (int i = 0; i < 12; ++i) {
    if (fs::is_regular_file(dir / kConstantsFile, ec)) {
      return dir.string();
    }
    if (!dir.has_parent_path()) break;
    dir = dir.parent_path();
  }
  return "";
}

std::string toLower(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) out.push_back(static_cast<char>(std::tolower(c)));
  return out;
}

// Strips a leading MUSIC_/SFX_ (constants) or Music_/SFX_ (headers) prefix,
// case-insensitively.
std::string stripPrefix(const std::string& s) {
  const std::string lower = toLower(s);
  if (lower.rfind("music_", 0) == 0) return s.substr(6);
  if (lower.rfind("sfx_", 0) == 0) return s.substr(4);
  if (lower.rfind("music", 0) == 0) return s.substr(5);
  if (lower.rfind("sfx", 0) == 0) return s.substr(3);
  return s;
}

// Edit distance with early exit past `limit`. Both inputs are short
// (track stems), so the full DP table is trivial.
std::size_t levenshtein(const std::string& a, const std::string& b,
                         std::size_t limit) {
  if (a == b) return 0;
  if (a.empty()) return b.size();
  if (b.empty()) return a.size();
  // Row DP: prev[j] = distance for a[:i-1] vs b[:j].
  std::vector<std::size_t> prev(b.size() + 1), cur(b.size() + 1);
  for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = j;
  for (std::size_t i = 1; i <= a.size(); ++i) {
    cur[0] = i;
    std::size_t row_min = cur[0];
    for (std::size_t j = 1; j <= b.size(); ++j) {
      const std::size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
      std::size_t v = prev[j] + 1;
      if (cur[j - 1] + 1 < v) v = cur[j - 1] + 1;
      if (prev[j - 1] + cost < v) v = prev[j - 1] + cost;
      cur[j] = v;
      if (v < row_min) row_min = v;
    }
    if (row_min > limit) return limit + 1;
    prev.swap(cur);
  }
  return prev[b.size()];
}

struct HeaderInfo {
  int bank = 0;
  int channel_count = 0;
  std::string file_path;
  std::vector<std::string> channels;
};

// Parses one header file into `table` (label -> metadata).
void parseOneHeader(const std::string& full_path,
                    const std::string& repo_rel, int bank,
                    std::map<std::string, HeaderInfo>* table) {
  std::ifstream in(full_path);
  if (!in.is_open() || table == nullptr) return;
  const std::regex label_re(R"(^(\w+)::)");
  const std::regex count_re(R"(^\s*channel_count\s+(\d+))");
  const std::regex chan_re(R"(^\s*channel\s+\d+\s*,\s*(\w+))");
  std::string line, current;
  HeaderInfo info;
  auto flush = [&]() {
    if (current.empty()) return;
    info.bank = bank;
    info.file_path = repo_rel;
    (*table)[current] = info;
    current.clear();
    info = HeaderInfo();
  };
  while (std::getline(in, line)) {
    std::smatch m;
    if (std::regex_search(line, m, label_re)) {
      flush();
      current = m[1].str();
      continue;
    }
    if (current.empty()) continue;
    if (std::regex_search(line, m, count_re)) {
      info.channel_count = std::stoi(m[1].str());
    } else if (std::regex_search(line, m, chan_re)) {
      info.channels.push_back(m[1].str());
    }
  }
  flush();
}

}  // namespace

std::string SongCatalog::normalize(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    if (std::isalnum(c)) out.push_back(static_cast<char>(std::tolower(c)));
  }
  return out;
}

SongCatalog::SongCatalog() : repo_root_(findRepoRoot()) { load(); }

SongCatalog::SongCatalog(const std::string& repo_root)
    : repo_root_(repo_root) {
  load();
}

void SongCatalog::load() {
  tracks_.clear();
  if (repo_root_.empty()) return;
  std::error_code ec;
  if (!fs::is_regular_file(fs::path(repo_root_) / kConstantsFile, ec)) {
    repo_root_.clear();
    return;
  }
  parseConstants((fs::path(repo_root_) / kConstantsFile).string());
}

void SongCatalog::parseConstants(const std::string& path) {
  // Build the header table first (bank + channel metadata by label) by
  // scanning every audio/headers/*.asm file. Bank = trailing digit before
  // ".asm" (musicheadersN / sfxheadersN map to AUDIO_1..AUDIO_4); a header
  // file without one keeps bank 0.
  std::map<std::string, HeaderInfo> headers;
  std::error_code ec;
  const fs::path header_dir = fs::path(repo_root_) / "audio" / "headers";
  fs::directory_iterator it(header_dir, ec);
  const fs::directory_iterator end;
  std::vector<std::string> files;
  if (!ec) {
    for (; it != end; it.increment(ec)) {
      if (ec) break;
      if (!it->is_regular_file(ec) || ec) continue;
      if (it->path().extension() != ".asm") continue;
      files.push_back(it->path().string());
    }
  }
  std::sort(files.begin(), files.end());
  for (const std::string& full : files) {
    const std::string stem = fs::path(full).stem().string();
    int bank = 0;
    if (!stem.empty() &&
        std::isdigit(static_cast<unsigned char>(stem.back()))) {
      bank = stem.back() - '0';
    }
    const std::string rel =
        fs::relative(fs::path(full), fs::path(repo_root_), ec).string();
    parseOneHeader(full, ec ? stem : rel, bank, &headers);
  }

  std::ifstream in(path);
  if (!in.is_open()) return;
  const std::regex re(R"(^\s*music_const\s+(\w+)\s*,\s*(\w+))");
  std::string line;
  while (std::getline(in, line)) {
    std::smatch m;
    if (!std::regex_search(line, m, re)) continue;
    SongInfo song;
    song.constant_name = m[1].str();
    song.header_label = m[2].str();
    const auto it = headers.find(song.header_label);
    if (it != headers.end()) {
      song.bank = it->second.bank;
      song.channel_count = it->second.channel_count;
      song.file_path = it->second.file_path;
      song.channels = it->second.channels;
    }
    tracks_.push_back(std::move(song));
  }
}

const SongInfo* SongCatalog::findTrack(const std::string& query) const {
  if (query.empty() || tracks_.empty()) return nullptr;

  // 1. Case-sensitive exact: constant, then header.
  for (const SongInfo& s : tracks_) {
    if (s.constant_name == query) return &s;
  }
  for (const SongInfo& s : tracks_) {
    if (s.header_label == query) return &s;
  }

  // 2. Case-insensitive exact.
  const std::string lower_q = toLower(query);
  for (const SongInfo& s : tracks_) {
    if (toLower(s.constant_name) == lower_q) return &s;
  }
  for (const SongInfo& s : tracks_) {
    if (toLower(s.header_label) == lower_q) return &s;
  }

  // 3. Normalized-stem exact (prefix-stripped).
  const std::string norm_q = normalize(query);
  if (!norm_q.empty()) {
    for (const SongInfo& s : tracks_) {
      if (normalize(stripPrefix(s.constant_name)) == norm_q) return &s;
      if (normalize(stripPrefix(s.header_label)) == norm_q) return &s;
    }
    // 4. Substring on the normalized stem (either direction).
    for (const SongInfo& s : tracks_) {
      const std::string stem_c = normalize(stripPrefix(s.constant_name));
      const std::string stem_h = normalize(stripPrefix(s.header_label));
      if (stem_c.find(norm_q) != std::string::npos ||
          stem_h.find(norm_q) != std::string::npos ||
          norm_q.find(stem_c) != std::string::npos) {
        return &s;
      }
    }
    // 5. Levenshtein fallback over stems (e.g. "route1" vs "routes1").
    const SongInfo* best = nullptr;
    std::size_t best_dist = static_cast<std::size_t>(-1);
    const std::size_t limit = 1 + norm_q.size() / 5;
    for (const SongInfo& s : tracks_) {
      const std::string stems[2] = {
          normalize(stripPrefix(s.constant_name)),
          normalize(stripPrefix(s.header_label)),
      };
      for (const std::string& stem : stems) {
        if (stem.empty()) continue;
        if (stem.size() + 4 < norm_q.size() || norm_q.size() + 4 < stem.size()) {
          continue;
        }
        const std::size_t d = levenshtein(norm_q, stem, limit);
        if (d <= limit && d < best_dist) {
          best_dist = d;
          best = &s;
        }
      }
    }
    if (best != nullptr) return best;
  }
  return nullptr;
}

}  // namespace audio_dbg
