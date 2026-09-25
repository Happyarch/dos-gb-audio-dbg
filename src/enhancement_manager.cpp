// Stage 5.3: EnhancementManager implementation. See enhancement_manager.h.

#include "enhancement_manager.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace audio_dbg {
namespace {

namespace fs = std::filesystem;

std::string readWholeFile(const std::string& path, bool* ok) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    if (ok != nullptr) *ok = false;
    return "";
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  if (ok != nullptr) *ok = true;
  return ss.str();
}

std::string sanitizeNote(const std::string& note) {
  std::string out;
  for (char c : note) {
    const auto uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc) != 0 || c == '-' || c == '_') out += c;
  }
  if (out.size() > 32) out.resize(32);
  return out;
}

std::string nowStamp() {
  std::time_t t = std::time(nullptr);
  std::tm tm_buf{};
#if defined(_WIN32)
  localtime_s(&tm_buf, &t);
#else
  localtime_r(&t, &tm_buf);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm_buf);
  return buf;
}

std::string zeroPad4(int id) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04d", id);
  return buf;
}

// Parses "<id>_<ts>[_<note>].yaml" stems like revisions.py:
// id = parts[0] (digits), ts = parts[1]. Returns false when malformed.
bool parseRevisionStem(const std::string& stem, int* id, std::string* ts) {
  const std::size_t u1 = stem.find('_');
  if (u1 == std::string::npos || u1 == 0) return false;
  const std::string id_part = stem.substr(0, u1);
  for (char c : id_part) {
    if (std::isdigit(static_cast<unsigned char>(c)) == 0) return false;
  }
  const std::string rest = stem.substr(u1 + 1);
  const std::size_t u2 = rest.find('_');
  const std::string ts_part = (u2 == std::string::npos) ? rest : rest.substr(0, u2);
  if (ts_part.empty()) return false;
  if (id != nullptr) *id = std::stoi(id_part);
  if (ts != nullptr) *ts = ts_part;
  return true;
}

// --- Minimal SMF reader (stdlib only) ------------------------------------

class SmfReader {
 public:
  SmfReader(const std::uint8_t* data, std::size_t len)
      : p_(data), n_(len), pos_(0), ok_(data != nullptr) {}

  bool ok() const { return ok_; }
  std::uint8_t u8() {
    if (pos_ >= n_) {
      ok_ = false;
      return 0;
    }
    return p_[pos_++];
  }
  std::uint16_t u16be() {
    const std::uint8_t hi = u8();
    const std::uint8_t lo = u8();
    return static_cast<std::uint16_t>((hi << 8) | lo);
  }
  std::uint32_t u32be() {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v = (v << 8) | u8();
    return v;
  }
  std::uint32_t vlq() {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      const std::uint8_t b = u8();
      if (!ok_) return 0;
      v = (v << 7) | (b & 0x7F);
      if ((b & 0x80) == 0) return v;
    }
    ok_ = false;  // Overlong VLQ.
    return 0;
  }
  void skip(std::size_t k) {
    if (pos_ + k > n_) {
      ok_ = false;
      pos_ = n_;
      return;
    }
    pos_ += k;
  }
  std::size_t pos() const { return pos_; }
  void seek(std::size_t pos) {
    pos_ = pos <= n_ ? pos : n_;
    ok_ = (p_ != nullptr);
  }

 private:
  const std::uint8_t* p_;
  std::size_t n_;
  std::size_t pos_;
  bool ok_;
};

// Extracts tempo events (0xFF 0x51 0x03) from a single SMF track.
bool extractTempos(SmfReader* r, std::size_t track_end,
                   std::vector<std::pair<std::uint32_t, std::uint32_t> >* tempos) {
  std::uint32_t tick = 0;
  std::uint8_t running = 0;
  while (r->pos() < track_end && r->ok()) {
    tick += r->vlq();
    if (!r->ok()) return false;
    std::uint8_t status = r->u8();
    if (!r->ok()) return false;
    int data_byte = -1;
    if (status < 0x80) {
      if (running == 0) return false;
      data_byte = status;
      status = running;
    }
    if (status == 0xFF) {
      const std::uint8_t mtype = r->u8();
      const std::uint32_t len = r->vlq();
      if (!r->ok()) return false;
      if (mtype == 0x51 && len == 3) {
        const std::uint8_t b0 = r->u8();
        const std::uint8_t b1 = r->u8();
        const std::uint8_t b2 = r->u8();
        const std::uint32_t tempo =
            (static_cast<std::uint32_t>(b0) << 16) |
            (static_cast<std::uint32_t>(b1) << 8) | b2;
        if (tempo > 0 && tempos != nullptr) tempos->push_back({tick, tempo});
      } else {
        r->skip(len);
      }
      running = 0;
    } else if (status == 0xF0 || status == 0xF7) {
      const std::uint32_t len = r->vlq();
      if (!r->ok()) return false;
      r->skip(len);
      running = 0;
    } else {
      const std::uint8_t hi = status & 0xF0;
      running = status;
      if (hi == 0xC0 || hi == 0xD0) {
        if (data_byte < 0) r->u8();
      } else {
        if (data_byte < 0) r->u8();
        r->u8();
      }
    }
  }
  return r->ok();
}

// Parses one track's events with per-track Note-Off to Note-On matching.
// Embedded SysEx frames (status 0xF0/0xF7) are retained verbatim in
// encounter order when `sysex_out` is non-null; otherwise they are skipped.
template <typename F>
bool parseSmfTrack(SmfReader* r, std::size_t track_end, F&& tickToFrame,
                   std::vector<SimNoteEvent>* out, SysExMessages* sysex_out,
                   std::uint32_t* loop_start_tick, std::uint32_t* loop_end_tick,
                   std::string* track_name_out = nullptr) {
  std::uint32_t tick = 0;
  std::uint8_t running = 0;
  std::vector<std::pair<std::uint32_t, int> > pending[16][128];

  while (r->pos() < track_end && r->ok()) {
    tick += r->vlq();
    if (!r->ok()) return false;
    std::uint8_t status = r->u8();
    if (!r->ok()) return false;
    int data_byte = -1;
    if (status < 0x80) {
      if (running == 0) return false;
      data_byte = status;
      status = running;
    }
    if (status == 0xFF) {
      const std::uint8_t mtype = r->u8();
      const std::uint32_t len = r->vlq();
      if (!r->ok()) return false;
      if (mtype == 0x06 && (len == 9 || len == 7)) {
        // Marker text meta event: "loopStart"/"loopEnd" carry the song's
        // loop point (written by gb_to_midi.py, read by midi_to_stream.py).
        char text[9] = {0};
        for (std::uint32_t i = 0; i < len; ++i) {
          text[i] = static_cast<char>(r->u8());
          if (!r->ok()) return false;
        }
        if (len == 9 && loop_start_tick != nullptr &&
            std::memcmp(text, "loopStart", 9) == 0) {
          *loop_start_tick = tick;
        } else if (len == 7 && loop_end_tick != nullptr &&
                   std::memcmp(text, "loopEnd", 7) == 0) {
          *loop_end_tick = tick;
        }
      } else if (mtype == 0x03 && track_name_out != nullptr &&
                 track_name_out->empty()) {
        // Track-name meta event (gb_to_midi.py writes "GB chN" for base
        // tracks and "enh <name> tierN" for merged enhancement tracks).
        // Captured so baseline loaders can drop the baked enhancement
        // layer; the live bridge is its single source at runtime.
        std::string name;
        name.reserve(len < 128 ? len : 128);
        for (std::uint32_t i = 0; i < len; ++i) {
          const std::uint8_t c = r->u8();
          if (!r->ok()) return false;
          if (name.size() < 128) name.push_back(static_cast<char>(c));
        }
        *track_name_out = std::move(name);
      } else {
        r->skip(len);
      }
      running = 0;
    } else if (status == 0xF0 || status == 0xF7) {
      const std::uint32_t len = r->vlq();
      if (!r->ok()) return false;
      if (sysex_out != nullptr) {
        // Retain the frame payload; the caller decides what to do with it.
        std::vector<std::uint8_t> frame;
        frame.reserve(len);
        for (std::uint32_t i = 0; i < len; ++i) {
          frame.push_back(r->u8());
          if (!r->ok()) return false;
        }
        if (!frame.empty()) sysex_out->push_back(std::move(frame));
      } else {
        r->skip(len);
      }
      running = 0;
    } else {
      const std::uint8_t hi = status & 0xF0;
      const int ch = status & 0x0F;
      running = status;
      if (hi == 0xC0 || hi == 0xD0) {
        const std::uint8_t d1 = data_byte >= 0
                                    ? static_cast<std::uint8_t>(data_byte)
                                    : r->u8();
        if (!r->ok()) return false;
        if (hi == 0xC0 && out != nullptr) {
          SimNoteEvent pcev;
          pcev.frame = tickToFrame(tick);
          pcev.channel = static_cast<std::uint8_t>(ch);
          pcev.note = d1;
          pcev.velocity = 0;
          pcev.duration_frames = 0;
          pcev.is_note_on = true;
          pcev.type = SimEventType::ProgramChange;
          out->push_back(pcev);
        }
      } else {
        const std::uint8_t d1 = data_byte >= 0
                                    ? static_cast<std::uint8_t>(data_byte)
                                    : r->u8();
        const std::uint8_t d2 = r->u8();
        if (!r->ok()) return false;
        if (hi == 0x90 && d2 > 0) {
          if (ch < 16 && d1 < 128) {
            pending[ch][d1].push_back({tick, d2});
          }
        } else if (hi == 0x90 || hi == 0x80) {
          if (ch < 16 && d1 < 128 && !pending[ch][d1].empty()) {
            const auto on_ev = pending[ch][d1].front();
            pending[ch][d1].erase(pending[ch][d1].begin());
            const std::uint32_t on_frame = tickToFrame(on_ev.first);
            std::uint32_t off_frame = tickToFrame(tick);
            if (off_frame <= on_frame) off_frame = on_frame + 1;
            const std::uint32_t dur = off_frame - on_frame;
            if (out != nullptr) {
              SimNoteEvent on;
              on.frame = on_frame;
              on.channel = static_cast<std::uint8_t>(ch);
              on.note = d1;
              on.velocity = static_cast<std::uint8_t>(on_ev.second < 1 ? 1 : (on_ev.second > 127 ? 127 : on_ev.second));
              on.duration_frames = static_cast<std::uint16_t>(dur > 0xFFFF ? 0xFFFF : dur);
              on.is_note_on = true;
              on.type = SimEventType::Note;
              out->push_back(on);

              SimNoteEvent off;
              off.frame = off_frame;
              off.channel = static_cast<std::uint8_t>(ch);
              off.note = d1;
              off.velocity = 0;
              off.duration_frames = 0;
              off.is_note_on = false;
              off.type = SimEventType::Note;
              out->push_back(off);
            }
          }
        } else if (hi == 0xB0) {
          if (out != nullptr && (d1 == 7 || d1 == 10)) {
            SimNoteEvent ccev;
            ccev.frame = tickToFrame(tick);
            ccev.channel = static_cast<std::uint8_t>(ch);
            ccev.note = d1;
            ccev.velocity = d2;
            ccev.duration_frames = 0;
            ccev.is_note_on = true;
            ccev.type = SimEventType::ControlChange;
            out->push_back(ccev);
          }
        }
      }
    }
  }

  // Close any unclosed notes in this track at the end of the track.
  const std::uint32_t track_end_frame = tickToFrame(tick) + 1;
  for (int c = 0; c < 16; ++c) {
    for (int k = 0; k < 128; ++k) {
      for (const auto& on_ev : pending[c][k]) {
        const std::uint32_t on_frame = tickToFrame(on_ev.first);
        const std::uint32_t off_frame =
            track_end_frame > on_frame ? track_end_frame : on_frame + 1;
        const std::uint32_t dur = off_frame - on_frame;
        if (out != nullptr) {
          SimNoteEvent on;
          on.frame = on_frame;
          on.channel = static_cast<std::uint8_t>(c);
          on.note = static_cast<std::uint8_t>(k);
          on.velocity = static_cast<std::uint8_t>(on_ev.second < 1 ? 1 : (on_ev.second > 127 ? 127 : on_ev.second));
          on.duration_frames = static_cast<std::uint16_t>(dur > 0xFFFF ? 0xFFFF : dur);
          on.is_note_on = true;
          on.type = SimEventType::Note;
          out->push_back(on);

          SimNoteEvent off;
          off.frame = off_frame;
          off.channel = static_cast<std::uint8_t>(c);
          off.note = static_cast<std::uint8_t>(k);
          off.velocity = 0;
          off.duration_frames = 0;
          off.is_note_on = false;
          off.type = SimEventType::Note;
          out->push_back(off);
        }
      }
    }
  }
  return r->ok();
}

}  // namespace

EnhancementManager::EnhancementManager()
    : EnhancementManager(sysex_bridge::findRepoRoot()) {}

EnhancementManager::EnhancementManager(const std::string& repo_root)
    : repo_root_(repo_root) {
  if (!repo_root_.empty()) {
    enhance_dir_ = (fs::path(repo_root_) / "dos_port" / "tools" / "audio" /
                    "enhancements")
                       .string();
    revisions_dir_ = (fs::path(repo_root_) / "dos_port" / "tools" / "audio" /
                      ".revisions")
                         .string();
    overrides_dir_ = (fs::path(repo_root_) / "dos_port" / "tools" / "audio" /
                      "overrides")
                         .string();
  }
}

EnhancementManager::EnhancementManager(const std::string& enhance_dir,
                                       const std::string& revisions_dir,
                                       const std::string& repo_root)
    : enhance_dir_(enhance_dir),
      revisions_dir_(revisions_dir),
      repo_root_(repo_root) {}

std::string EnhancementManager::revisionDirFor(const std::string& song) const {
  return (fs::path(revisions_dir_) / song).string();
}

std::string EnhancementManager::yamlPathFor(const std::string& song) const {
  return (fs::path(enhance_dir_) / (song + ".yaml")).string();
}

void EnhancementManager::watchSong(const std::string& song) {
  watched_song_ = song;
  refreshWatchCache();
  refreshOverrideWatchCache();
}

std::string EnhancementManager::watchedPath() const {
  if (watched_song_.empty() || enhance_dir_.empty()) return "";
  return yamlPathFor(watched_song_);
}

void EnhancementManager::refreshWatchCache() {
  watch_have_stamp_ = true;
  watch_existed_ = false;
  watch_mtime_ns_ = 0;
  if (watched_song_.empty() || enhance_dir_.empty()) return;
  std::error_code ec;
  fs::path p = yamlPathFor(watched_song_);
  watch_existed_ = fs::is_regular_file(p, ec);
  if (watch_existed_) {
    auto t = fs::last_write_time(p, ec);
    if (!ec) {
      watch_mtime_ns_ =
          static_cast<std::uint64_t>(t.time_since_epoch().count());
    }
  }
}

bool EnhancementManager::pollForChanges() {
  if (watched_song_.empty()) return false;
  std::error_code ec;
  fs::path p = yamlPathFor(watched_song_);
  const bool existed = fs::is_regular_file(p, ec);
  std::uint64_t mtime_ns = 0;
  if (existed) {
    auto t = fs::last_write_time(p, ec);
    if (!ec) {
      mtime_ns = static_cast<std::uint64_t>(t.time_since_epoch().count());
    }
  }
  bool changed = false;
  if (!watch_have_stamp_) {
    changed = existed;  // First poll after construction: report a file.
  } else if (existed != watch_existed_) {
    changed = true;
  } else if (existed && mtime_ns != watch_mtime_ns_) {
    changed = true;
  }
  watch_have_stamp_ = true;
  watch_existed_ = existed;
  watch_mtime_ns_ = mtime_ns;
  return changed;
}

std::string EnhancementManager::overridesPathFor(
    const std::string& song) const {
  return (fs::path(overrides_dir_) / (song + ".yaml")).string();
}

std::string EnhancementManager::watchedOverridesPath() const {
  if (watched_song_.empty() || overrides_dir_.empty()) return "";
  return overridesPathFor(watched_song_);
}

void EnhancementManager::refreshOverrideWatchCache() {
  ov_have_stamp_ = true;
  ov_existed_ = false;
  ov_mtime_ns_ = 0;
  if (watched_song_.empty() || overrides_dir_.empty()) return;
  std::error_code ec;
  fs::path p = overridesPathFor(watched_song_);
  ov_existed_ = fs::is_regular_file(p, ec);
  if (ov_existed_) {
    auto t = fs::last_write_time(p, ec);
    if (!ec) {
      ov_mtime_ns_ =
          static_cast<std::uint64_t>(t.time_since_epoch().count());
    }
  }
}

bool EnhancementManager::pollOverrideChanges() {
  // No overrides dir configured (or no watched song): nothing to track.
  // Unlike pollForChanges, an unstamped first poll reports NO change —
  // watchSong() stamps both caches, and a re-render on every track switch
  // would hitch the UI for no reason (the fresh loadTrackBaseline already
  // plays the current .mid).
  if (watched_song_.empty() || overrides_dir_.empty() || !ov_have_stamp_) {
    return false;
  }
  std::error_code ec;
  fs::path p = overridesPathFor(watched_song_);
  const bool existed = fs::is_regular_file(p, ec);
  std::uint64_t mtime_ns = 0;
  if (existed) {
    auto t = fs::last_write_time(p, ec);
    if (!ec) {
      mtime_ns = static_cast<std::uint64_t>(t.time_since_epoch().count());
    }
  }
  bool changed = false;
  if (existed != ov_existed_) {
    changed = true;
  } else if (existed && mtime_ns != ov_mtime_ns_) {
    changed = true;
  }
  ov_existed_ = existed;
  ov_mtime_ns_ = mtime_ns;
  return changed;
}

std::pair<int, std::string> EnhancementManager::saveSnapshot(
    const std::string& song, const std::string& content,
    const std::string& note) {
  std::error_code ec;
  fs::create_directories(revisionDirFor(song), ec);
  std::vector<RevisionEntry> revs = listRevisions(song);
  if (!revs.empty()) {
    bool same = false;
    {
      bool ok = false;
      const std::string latest = readWholeFile(revs.back().path, &ok);
      same = ok && latest == content;  // Byte-compare == hash-compare.
    }
    if (same) return {revs.back().id, revs.back().path};
  }
  const int next_id = revs.empty() ? 1 : revs.back().id + 1;
  const std::string slug = sanitizeNote(note);
  std::string filename =
      zeroPad4(next_id) + "_" + nowStamp() + (slug.empty() ? "" : "_" + slug) + ".yaml";
  fs::path out = fs::path(revisionDirFor(song)) / filename;
  {
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    f << content;
  }
  return {next_id, out.string()};
}

std::vector<RevisionEntry> EnhancementManager::listRevisions(
    const std::string& song) const {
  std::vector<RevisionEntry> out;
  std::error_code ec;
  fs::path dir = revisionDirFor(song);
  if (!fs::is_directory(dir, ec)) return out;
  fs::directory_iterator it(dir, ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    fs::path p = it->path();
    if (p.extension() != ".yaml") continue;
    int id = 0;
    std::string ts;
    if (!parseRevisionStem(p.stem().string(), &id, &ts)) continue;
    RevisionEntry e;
    e.id = id;
    e.timestamp = ts;
    e.path = p.string();
    out.push_back(e);
  }
  std::sort(out.begin(), out.end(),
            [](const RevisionEntry& a, const RevisionEntry& b) {
              return a.id < b.id;
            });
  return out;
}

std::pair<bool, std::string> EnhancementManager::getRevisionContent(
    const std::string& song, int rev_id) const {
  std::error_code ec;
  fs::path dir = revisionDirFor(song);
  if (!fs::is_directory(dir, ec)) return {false, ""};
  const std::string prefix = zeroPad4(rev_id) + "_";
  std::string best;
  fs::directory_iterator it(dir, ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    fs::path p = it->path();
    if (p.extension() != ".yaml") continue;
    if (p.filename().string().rfind(prefix, 0) != 0) continue;
    if (best.empty() || p.string() < best) best = p.string();
  }
  if (best.empty()) return {false, ""};
  bool ok = false;
  std::string content = readWholeFile(best, &ok);
  if (!ok) return {false, ""};
  return {true, content};
}

bool EnhancementManager::revertToRevision(const std::string& song, int rev_id) {
  auto found = getRevisionContent(song, rev_id);
  if (!found.first) return false;
  std::error_code ec;
  fs::create_directories(enhance_dir_, ec);
  fs::path target = yamlPathFor(song);
  // Locate the exact revision file again for a byte-exact copy.
  const std::string prefix = zeroPad4(rev_id) + "_";
  fs::directory_iterator it(revisionDirFor(song), ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    fs::path p = it->path();
    if (p.extension() != ".yaml") continue;
    if (p.filename().string().rfind(prefix, 0) != 0) continue;
    fs::copy_file(p, target, fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    if (song == watched_song_) refreshWatchCache();
    return true;
  }
  return false;
}

std::pair<bool, std::string> EnhancementManager::loadYamlFile(
    const std::string& song) const {
  bool ok = false;
  std::string content = readWholeFile(yamlPathFor(song), &ok);
  if (!ok) return {false, ""};
  return {true, content};
}

std::vector<SimNoteEvent> EnhancementManager::compileEnhancement(
    const std::string& song, const std::string& target) const {
  std::vector<SimNoteEvent> out;
  opl_voices_.clear();
  if (repo_root_.empty()) return out;
  fs::path bridge = fs::path(repo_root_) / "dos_port" / "tools" /
                    "dos-gb-audio-dbg" / "src" / "enhancement_dump.py";
  std::error_code ec;
  if (!fs::is_regular_file(bridge, ec)) {
    bridge = fs::path(repo_root_) / "dos_port" / "tools" / "viewer" /
             "src" / "enhancement_dump.py";
  }
  if (!fs::is_regular_file(bridge, ec)) return out;
  const std::string cmd =
      "python3 " + sysex_bridge::shellQuote(bridge.string()) + " " +
      sysex_bridge::shellQuote(repo_root_) + " " +
      sysex_bridge::shellQuote(song) + " --target " +
      sysex_bridge::shellQuote(target) + " 2>/dev/null";
  FILE* pipe = ::popen(cmd.c_str(), "r");
  if (pipe == nullptr) return out;
  bool saw_ok = false;
  bool saw_end = false;
  char buf[512];
  std::string carry;
  auto handleLine = [&](const std::string& line) {
    std::istringstream ls(line);
    std::string kind;
    ls >> kind;
    if (kind == "OK") {
      saw_ok = true;
    } else if (kind == "CH") {
      int idx = 0, tier = 0, mc = 0, prog = -1;
      float vol = 1.0f;
      std::string name;
      ls >> idx >> name >> tier >> mc >> prog >> vol;
      if (!ls.fail() && mc >= 0 && mc < 16 && prog >= 0 && prog < 128) {
        SimNoteEvent pcev;
        pcev.frame = 0;
        pcev.channel = static_cast<std::uint8_t>(mc);
        pcev.note = static_cast<std::uint8_t>(prog);
        pcev.type = SimEventType::ProgramChange;
        out.push_back(pcev);
      }
    } else if (kind == "NOTE") {
      std::uint32_t frame = 0;
      std::uint32_t dur = 0;
      int ch = 0, key = 0, vel = 0;
      ls >> frame >> dur >> ch >> key >> vel;
      if (ls.fail() || ch < 0 || ch > 15 || key < 0 || key > 127) return;
      if (vel < 1) vel = 1;
      if (vel > 127) vel = 127;
      if (dur == 0) dur = 1;
      SimNoteEvent on;
      on.frame = frame;
      on.channel = static_cast<std::uint8_t>(ch);
      on.note = static_cast<std::uint8_t>(key);
      on.velocity = static_cast<std::uint8_t>(vel);
      on.duration_frames = static_cast<std::uint16_t>(
          dur > 0xFFFF ? 0xFFFF : dur);
      on.is_note_on = true;
      out.push_back(on);
      SimNoteEvent off;
      off.frame = frame + dur;
      off.channel = on.channel;
      off.note = on.note;
      off.velocity = 0;
      off.duration_frames = 0;
      off.is_note_on = false;
      out.push_back(off);
    } else if (kind == "OPLVOICE") {
      // Authored tier-1 FM voice (--target opl3 only): MIDI channel,
      // channel volume, C0 pan bits, then the 11 raw OPL register bytes
      // in gen_opl_patches.py PATCHES order.
      int mc = -1, vol = -1, pan = -1, b[11] = {0};
      ls >> mc >> vol >> pan;
      for (int i = 0; i < 11; ++i) ls >> b[i];
      bool ok = !ls.fail() && mc >= 0 && mc < 16;
      for (int i = 0; ok && i < 11; ++i) ok = (b[i] >= 0 && b[i] <= 255);
      if (ok) {
        Opl3Device::VoicePatch patch;
        for (int i = 0; i < 11; ++i) {
          patch.reg[i] = static_cast<std::uint8_t>(b[i]);
        }
        patch.volume =
            static_cast<std::uint8_t>(std::min(127, std::max(0, vol)));
        const int pan_bits = pan & 0x30;
        patch.pan =
            static_cast<std::uint8_t>(pan_bits != 0 ? pan_bits : 0x30);
        opl_voices_[static_cast<std::uint8_t>(mc)] = patch;
      }
    } else if (kind == "END") {
      saw_end = true;
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
  if (!saw_ok || !saw_end) {
    out.clear();
    return out;
  }
  std::stable_sort(out.begin(), out.end(), eventLess);
  return out;
}

std::vector<SimNoteEvent> EnhancementManager::loadMidiFile(
    const std::string& path) const {
  return parseMidiFile(path).notes;
}

MidiFileData EnhancementManager::parseMidiFile(
    const std::string& path, bool drop_enhancement_tracks) const {
  MidiFileData result;
  bool ok = false;
  const std::string bytes = readWholeFile(path, &ok);
  if (!ok || bytes.size() < 14) return result;
  const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
  SmfReader r(data, bytes.size());
  if (r.u8() != 'M' || r.u8() != 'T' || r.u8() != 'h' || r.u8() != 'd') {
    return result;
  }
  const std::uint32_t header_len = r.u32be();
  const std::uint16_t format = r.u16be();
  const std::uint16_t ntracks = r.u16be();
  const std::uint16_t division = r.u16be();
  if (!r.ok() || (format != 0 && format != 1) || (division & 0x8000) != 0 ||
      division == 0) {
    return result;  // SMPTE divisions carry no tempo map; unsupported.
  }
  r.skip(header_len > 6 ? header_len - 6 : 0);
  if (!r.ok()) return result;

  struct TrackInfo {
    std::size_t start_pos = 0;
    std::size_t end_pos = 0;
  };
  std::vector<TrackInfo> track_infos;
  track_infos.reserve(ntracks);
  std::vector<std::pair<std::uint32_t, std::uint32_t> > tempos;

  for (std::uint16_t t = 0; t < ntracks; ++t) {
    if (r.u8() != 'M' || r.u8() != 'T' || r.u8() != 'r' || r.u8() != 'k') {
      return result;
    }
    const std::uint32_t track_len = r.u32be();
    if (!r.ok()) return result;
    const std::size_t track_start = r.pos();
    const std::size_t track_end = track_start + track_len;
    track_infos.push_back({track_start, track_end});
    if (!extractTempos(&r, track_end, &tempos)) return result;
    r.seek(track_end);
  }

  std::sort(tempos.begin(), tempos.end());
  auto tempoAt = [&](std::uint32_t tick) -> std::uint32_t {
    std::uint32_t tempo = 1000000;  // Default tempo: 1s per quarter.
    for (const auto& te : tempos) {
      if (te.first <= tick) {
        tempo = te.second;
      } else {
        break;
      }
    }
    return tempo;
  };
  auto tickToFrame = [&](std::uint32_t tick) -> std::uint32_t {
    // frames = ticks * (usec/quarter) * 60fps / (1e6 * ticks/quarter).
    const double frames = static_cast<double>(tick) *
                          static_cast<double>(tempoAt(tick)) * 60.0 /
                          (1000000.0 * static_cast<double>(division));
    return static_cast<std::uint32_t>(frames + 0.5);
  };

  // Pass 2: parse each track with per-track Note-Off to Note-On matching,
  // retaining embedded SysEx frames instead of skipping them.
  // The loopEnd marker is always written (even one-shot songs), but loopStart
  // is only written when the song actually loops (`loop_start is not None`).
  // A sentinel distinguishes "loop from frame 0" (marker at tick 0) from
  // "no loop at all" (marker absent).
  constexpr std::uint32_t kNoTick = UINT32_MAX;
  std::uint32_t loop_start_tick = kNoTick;
  std::uint32_t loop_end_tick = kNoTick;
  for (const auto& ti : track_infos) {
    r.seek(ti.start_pos);
    std::string track_name;
    std::vector<SimNoteEvent> track_notes;
    SysExMessages track_sysex;
    if (!parseSmfTrack(&r, ti.end_pos, tickToFrame, &track_notes,
                       &track_sysex, &loop_start_tick, &loop_end_tick,
                       &track_name)) {
      return result;
    }
    // Baked enhancement tracks ("enh <name> tierN", merged by gb_to_midi.py
    // for the soundtrack-export side goal) never play at runtime: the live
    // bridge (compileEnhancement) is the single enhancement source, gated
    // per device by its tier target. Without this every enhancement note
    // sounds twice, and tier-2/3 leak onto the OPL3 tab via the mt32 base.
    if (drop_enhancement_tracks && track_name.compare(0, 4, "enh ") == 0) {
      continue;
    }
    result.notes.insert(result.notes.end(), track_notes.begin(),
                        track_notes.end());
    result.sysex.insert(result.sysex.end(), track_sysex.begin(),
                        track_sysex.end());
  }
  if (loop_start_tick != kNoTick && loop_end_tick != kNoTick) {
    result.loop_end_frame = tickToFrame(loop_end_tick);
    result.loop_start_frame = tickToFrame(loop_start_tick);
  }

  std::stable_sort(result.notes.begin(), result.notes.end(), eventLess);
  return result;
}

std::string EnhancementManager::midiPathFor(
    const std::string& song, const std::string& target) const {
  if (repo_root_.empty()) return "";
  return (fs::path(repo_root_) / "dos_port" / "assets" / "midi" / target /
          (song + ".mid"))
      .string();
}

std::vector<SimNoteEvent> EnhancementManager::loadSongBaseline(
    const std::string& song, const std::string& target) const {
  const std::string path = midiPathFor(song, target);
  if (!path.empty() && fs::exists(path)) {
    // Base channels only: the baked "enh ..." tracks stay in the file for
    // soundtrack export; the live bridge plays them per the device's tier.
    return parseMidiFile(path, true).notes;
  }
  return compileEnhancement(song, target);
}

bool EnhancementManager::renderBaseline(const std::string& song,
                                       std::string* err) const {
  static const char* const kTargets[2] = {"mt32", "gm"};
  if (song.empty() || repo_root_.empty()) {
    if (err != nullptr) *err = "no song (or no repo root)";
    return false;
  }
  std::error_code ec;
  const fs::path script = fs::path(repo_root_) / "dos_port" / "tools" /
                          "audio" / "gb_to_midi.py";
  if (!fs::is_regular_file(script, ec)) {
    if (err != nullptr) *err = "gb_to_midi.py not found";
    return false;
  }
  std::string last_line;
  for (const char* target : kTargets) {
    const std::string cmd =
        "python3 " + sysex_bridge::shellQuote(script.string()) +
        " --target " + target + " --songs " + sysex_bridge::shellQuote(song) +
        " 2>&1";
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (pipe == nullptr) {
      if (err != nullptr) *err = "popen failed";
      return false;
    }
    char buf[512];
    std::string carry;
    while (std::fgets(buf, sizeof(buf), pipe) != nullptr) {
      carry += buf;
      std::size_t nl = 0;
      while ((nl = carry.find('\n')) != std::string::npos) {
        last_line = carry.substr(0, nl);
        carry.erase(0, nl + 1);
      }
    }
    if (!carry.empty()) last_line = carry;
    const int status = ::pclose(pipe);
    // A failing render must not invalidate the current baseline: gb_to_midi
    // builds each song fully in memory and only then writes its .mid, so a
    // song that errors keeps its previous file. Still, report and stop —
    // the caller keeps the old baseline on false.
    if (status != 0) {
      if (err != nullptr) {
        *err = std::string(target) + ": " +
               (last_line.empty() ? "non-zero exit" : last_line);
      }
      return false;
    }
  }
  return true;
}

SysExMessages EnhancementManager::loadTimbreBank() const {
  SysExMessages timbres;
  SongTimbreSysex unused;
  sysex_bridge::run(repo_root_, "timbres", "", &timbres, &unused);
  return timbres;
}

SongTimbreSysex EnhancementManager::loadSongTimbreSysex(
    const std::string& song) const {
  SongTimbreSysex out;
  if (song.empty() || repo_root_.empty()) return out;
  sysex_bridge::run(repo_root_, "song", song, nullptr, &out);
  return out;
}

}  // namespace audio_dbg
