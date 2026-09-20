// ConfigManager implementation: lightweight YAML + path hierarchy.
// See config.h for the contract.

#include "config.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace audio_dbg {
namespace {

namespace fs = std::filesystem;

constexpr const char* kDefaultRoot = "/mnt/sdb1/Code/Active Code/pokeyellow_msdos";
constexpr const char* kDefaultOverrides = "dos_port/tools/audio/overrides";
constexpr const char* kDefaultEnhancements = "dos_port/tools/audio/enhancements";
constexpr const char* kDefaultConstants = "constants/music_constants.asm";
constexpr const char* kDefaultHeaders = "audio/headers";

std::string trim(const std::string& s) {
  std::size_t b = 0;
  while (b < s.size() &&
         (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) {
    ++b;
  }
  std::size_t e = s.size();
  while (e > b &&
         (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' ||
          s[e - 1] == '\n')) {
    --e;
  }
  return s.substr(b, e - b);
}

std::size_t indentOf(const std::string& line) {
  std::size_t n = 0;
  while (n < line.size() && (line[n] == ' ' || line[n] == '\t')) ++n;
  return n;
}

// Strips a trailing "# comment" unless the '#' sits inside single/double
// quotes. Handles backslash escapes inside double quotes.
std::string stripComment(const std::string& s) {
  bool in_single = false;
  bool in_double = false;
  for (std::size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (in_single) {
      if (c == '\'') in_single = false;
      continue;
    }
    if (in_double) {
      if (c == '\\' && i + 1 < s.size()) {
        ++i;
        continue;
      }
      if (c == '"') in_double = false;
      continue;
    }
    if (c == '\'') {
      in_single = true;
    } else if (c == '"') {
      in_double = true;
    } else if (c == '#') {
      return trim(s.substr(0, i));
    }
  }
  return s;
}

std::string unquote(const std::string& s) {
  const std::string t = trim(s);
  if (t.size() >= 2 &&
      ((t.front() == '"' && t.back() == '"') ||
       (t.front() == '\'' && t.back() == '\''))) {
    const char q = t.front();
    std::string out;
    for (std::size_t i = 1; i + 1 < t.size(); ++i) {
      if (q == '"' && t[i] == '\\' && i + 1 + 1 < t.size() + 1) {
        const char n = t[i + 1];
        if (n == 'n') {
          out += '\n';
        } else if (n == 't') {
          out += '\t';
        } else {
          out += n;
        }
        ++i;
      } else {
        out += t[i];
      }
    }
    return out;
  }
  return t;
}

bool needsQuote(const std::string& s) {
  if (s.empty()) return true;
  if (s.front() == ' ' || s.back() == ' ') return true;
  for (char c : s) {
    if (c == ':' || c == '#' || c == '"' || c == '\'' || c == '\n') return true;
  }
  if (s.find("  ") != std::string::npos) return false;  // plain is fine
  if (s.find(' ') != std::string::npos) return true;
  return false;
}

std::string quote(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '\\' || c == '"') out += '\\';
    out += c;
  }
  out += "\"";
  return out;
}

std::string yamlValue(const std::string& s) {
  return needsQuote(s) ? quote(s) : s;
}

}  // namespace

ConfigManager::ConfigManager() : config_path_(defaultConfigPath()) {
  ProjectConfig d = defaultProject();
  current_project_ = d.name;
  projects_[d.name] = d;
}

ConfigManager::ConfigManager(const std::string& config_path)
    : config_path_(config_path) {
  ProjectConfig d = defaultProject();
  // Keep the default in memory until load() replaces it, so a manager
  // is usable even before load() and so a missing file yields defaults.
  if (current_project_.empty()) current_project_ = d.name;
  if (projects_.empty()) projects_[d.name] = d;
  if (config_path_.empty()) config_path_ = defaultConfigPath();
}

std::string ConfigManager::defaultConfigPath() {
  const char* home = std::getenv("HOME");
  std::string h = (home != nullptr && home[0] != '\0') ? home : ".";
  return (fs::path(h) / ".config" / "dos-gb-audio-dbg" / "config.yaml")
      .string();
}

ProjectConfig ConfigManager::defaultProject() {
  ProjectConfig p;
  p.name = "pokeyellow";
  p.root = kDefaultRoot;
  p.overrides = kDefaultOverrides;
  p.enhancements = kDefaultEnhancements;
  p.constants = kDefaultConstants;
  p.headers = kDefaultHeaders;
  return p;
}

bool ConfigManager::load() {
  std::error_code ec;
  if (!fs::is_regular_file(config_path_, ec)) {
    // First run: install defaults and persist them (creating parents).
    ProjectConfig d = defaultProject();
    current_project_ = d.name;
    projects_.clear();
    projects_[d.name] = d;
    save();
    return true;
  }
  std::ifstream in(config_path_);
  if (!in.is_open()) return false;

  std::string current;
  std::map<std::string, ProjectConfig> parsed;
  bool in_projects = false;
  std::size_t projects_indent = 0;
  std::string active;
  std::size_t proj_indent = 0;
  bool have_proj = false;

  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const std::size_t indent = indentOf(line);
    std::string body = trim(line);
    if (body.empty() || body[0] == '#') continue;
    body = stripComment(body);
    if (body.empty()) continue;
    const std::size_t colon = body.find(':');
    if (colon == std::string::npos) continue;
    const std::string key = trim(body.substr(0, colon));
    const std::string raw_value = trim(body.substr(colon + 1));
    const std::string value = unquote(raw_value);
    if (key.empty()) continue;

    if (indent == 0) {
      if (key == "current_project") {
        current = value;
        in_projects = false;
        have_proj = false;
        active.clear();
      } else if (key == "projects") {
        in_projects = true;
        projects_indent = 0;
        have_proj = false;
        active.clear();
      } else {
        in_projects = false;
      }
      continue;
    }

    if (!in_projects) continue;
    if (indent <= projects_indent) {
      // Malformed dedent out of the projects block: leave the section.
      in_projects = false;
      have_proj = false;
      active.clear();
      continue;
    }
    if (!have_proj) {
      // First indented entry under projects: -> project name.
      active = key;
      proj_indent = indent;
      have_proj = true;
      ProjectConfig p;
      p.name = active;
      // A `name: <inline>` value is ignored; fields come on deeper lines.
      parsed[active] = p;
      continue;
    }
    if (indent > proj_indent) {
      // Field of the active project.
      auto it = parsed.find(active);
      if (it == parsed.end()) continue;
      if (key == "root") {
        it->second.root = value;
      } else if (key == "overrides") {
        it->second.overrides = value;
      } else if (key == "enhancements") {
        it->second.enhancements = value;
      } else if (key == "constants") {
        it->second.constants = value;
      } else if (key == "headers") {
        it->second.headers = value;
      }
    } else {
      // Sibling project (same or shallower indent, still inside projects:).
      active = key;
      proj_indent = indent;
      ProjectConfig p;
      p.name = active;
      parsed[active] = p;
    }
  }

  if (!parsed.empty()) {
    projects_ = parsed;
    if (!current.empty() && parsed.count(current) != 0u) {
      current_project_ = current;
    } else if (parsed.count(current_project_) == 0u) {
      current_project_ = parsed.begin()->first;
    }
    // Ensure stored names match map keys.
    for (auto& kv : projects_) kv.second.name = kv.first;
    return true;
  }
  // Empty projects block: keep previous in-memory state, but honour a
  // top-level current_project when it names a known project.
  if (!current.empty() && projects_.count(current) != 0u) {
    current_project_ = current;
    return true;
  }
  return false;
}

bool ConfigManager::save() const {
  std::error_code ec;
  const fs::path p(config_path_);
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
  std::ofstream out(config_path_, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) return false;
  out << "current_project: " << yamlValue(current_project_) << "\n";
  out << "projects:\n";
  for (const auto& kv : projects_) {
    const ProjectConfig& pr = kv.second;
    out << "  " << yamlValue(pr.name) << ":\n";
    out << "    root: " << yamlValue(pr.root) << "\n";
    out << "    overrides: " << yamlValue(pr.overrides) << "\n";
    out << "    enhancements: " << yamlValue(pr.enhancements) << "\n";
    out << "    constants: " << yamlValue(pr.constants) << "\n";
    out << "    headers: " << yamlValue(pr.headers) << "\n";
  }
  out.flush();
  return static_cast<bool>(out);
}

bool ConfigManager::setCurrentProject(const std::string& name) {
  if (projects_.count(name) == 0u) return false;
  current_project_ = name;
  return true;
}

std::vector<std::string> ConfigManager::projects() const {
  std::vector<std::string> out;
  for (const auto& kv : projects_) out.push_back(kv.first);
  return out;
}

bool ConfigManager::hasProject(const std::string& name) const {
  return projects_.count(name) != 0u;
}

const ProjectConfig* ConfigManager::project(const std::string& name) const {
  const auto it = projects_.find(name);
  return it == projects_.end() ? nullptr : &it->second;
}

ProjectConfig* ConfigManager::mutableProject(const std::string& name) {
  const auto it = projects_.find(name);
  return it == projects_.end() ? nullptr : &it->second;
}

const ProjectConfig* ConfigManager::activeProject() const {
  return project(current_project_);
}

bool ConfigManager::addOrUpdateProject(const ProjectConfig& p) {
  if (p.name.empty()) return false;
  projects_[p.name] = p;
  projects_[p.name].name = p.name;
  return true;
}

void ConfigManager::setActiveProjectRoot(const std::string& root) {
  ProjectConfig* a = mutableProject(current_project_);
  if (a != nullptr) a->root = root;
}

std::string ConfigManager::activeRoot() const {
  const ProjectConfig* a = activeProject();
  return a != nullptr ? a->root : "";
}

bool ConfigManager::isAbsolutePath(const std::string& p) {
  if (p.empty()) return false;
  // std::filesystem handles POSIX roots; also treat Windows drive roots
  // (C:\ / C:/) as absolute for cross-platform configs.
  if (fs::path(p).is_absolute()) return true;
  if (p.size() >= 3 && ((p[0] >= 'A' && p[0] <= 'Z') ||
                        (p[0] >= 'a' && p[0] <= 'z')) &&
      p[1] == ':' && (p[2] == '/' || p[2] == '\\')) {
    return true;
  }
  return false;
}

std::string ConfigManager::joinRoot(const std::string& root,
                                    const std::string& rel) {
  if (root.empty()) return rel;
  if (rel.empty()) return root;
  return (fs::path(root) / fs::path(rel)).string();
}

std::string ConfigManager::fallbackSearch(const std::string& relative) const {
  if (relative.empty()) return "";
  if (isAbsolutePath(relative)) return relative;
  std::error_code ec;
  if (const char* env = std::getenv("PKMN_REPO_ROOT")) {
    if (env[0] != '\0') {
      const fs::path cand = fs::path(env) / relative;
      if (fs::exists(cand, ec)) return cand.string();
    }
  }
  fs::path dir = fs::current_path(ec);
  if (!ec) {
    for (int i = 0; i < 12; ++i) {
      const fs::path cand = dir / relative;
      if (fs::exists(cand, ec)) return cand.string();
      if (!dir.has_parent_path()) break;
      dir = dir.parent_path();
    }
  }
  // Sibling checkouts next to this repo (covers a `dos-gb-audio-dbg`
  // CWD whose data lives in ../pokeyellow_msdos).
  const char* siblings[] = {"../pokeyellow_msdos", "../../pokeyellow_msdos"};
  for (const char* s : siblings) {
    const fs::path cand = fs::path(s) / relative;
    if (fs::exists(cand, ec)) return cand.string();
  }
  return relative;
}

std::string ConfigManager::resolveField(
    const std::string& cli_explicit, const std::string& project_value,
    const std::string& fallback_relative) const {
  if (!cli_explicit.empty()) return cli_explicit;
  if (!project_value.empty()) {
    if (isAbsolutePath(project_value)) return project_value;
    const std::string root = activeRoot();
    if (!root.empty()) return joinRoot(root, project_value);
    return project_value;
  }
  return fallbackSearch(fallback_relative);
}

std::string ConfigManager::resolveOverridesDir(
    const std::string& cli_explicit) const {
  const ProjectConfig* a = activeProject();
  const std::string v = (a != nullptr) ? a->overrides : "";
  return resolveField(cli_explicit, v, kDefaultOverrides);
}

std::string ConfigManager::resolveEnhancementsDir(
    const std::string& cli_explicit) const {
  const ProjectConfig* a = activeProject();
  const std::string v = (a != nullptr) ? a->enhancements : "";
  return resolveField(cli_explicit, v, kDefaultEnhancements);
}

std::string ConfigManager::resolveConstantsPath(
    const std::string& cli_explicit) const {
  const ProjectConfig* a = activeProject();
  const std::string v = (a != nullptr) ? a->constants : "";
  return resolveField(cli_explicit, v, kDefaultConstants);
}

std::string ConfigManager::resolveHeadersDir(
    const std::string& cli_explicit) const {
  const ProjectConfig* a = activeProject();
  const std::string v = (a != nullptr) ? a->headers : "";
  return resolveField(cli_explicit, v, kDefaultHeaders);
}

}  // namespace audio_dbg
