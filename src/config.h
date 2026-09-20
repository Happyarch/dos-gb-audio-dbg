// Multi-project configuration and path hierarchy engine for dgad.
//
// `ConfigManager` owns ~/.config/dos-gb-audio-dbg/config.yaml (lightweight
// YAML, no external dependency):
//   current_project: pokeyellow
//   projects:
//     pokeyellow:
//       root: "/mnt/sdb1/Code/Active Code/pokeyellow_msdos"
//       overrides: "dos_port/tools/audio/overrides"
//       enhancements: "dos_port/tools/audio/enhancements"
//       constants: "constants/music_constants.asm"
//       headers: "audio/headers"
//
// Path precedence for every resolve*() entry point:
//   1) cli_explicit non-empty -> absolute precedence (returned as-is).
//   2) Active project's path (relative values are joined with project root).
//   3) Fallback: $PKMN_REPO_ROOT, then walk up from CWD, then sibling
//      pokeyellow_msdos checkouts, else the default relative path.

#ifndef DOS_GB_AUDIO_DBG_CONFIG_H_
#define DOS_GB_AUDIO_DBG_CONFIG_H_

#include <map>
#include <string>
#include <vector>

namespace audio_dbg {

struct ProjectConfig {
  std::string name;
  std::string root;
  std::string overrides = "dos_port/tools/audio/overrides";
  std::string enhancements = "dos_port/tools/audio/enhancements";
  std::string constants = "constants/music_constants.asm";
  std::string headers = "audio/headers";
};

class ConfigManager {
 public:
  ConfigManager();
  explicit ConfigManager(const std::string& config_path);

  static std::string defaultConfigPath();
  static ProjectConfig defaultProject();

  const std::string& configPath() const { return config_path_; }

  // Loads the YAML file. When the file does not exist, a default config
  // (pokeyellow) is installed in memory, parent directories are created,
  // and the file is written. Returns false only on I/O or parse errors
  // that leave no usable configuration.
  bool load();
  // Writes the current configuration (creating parent dirs). Const: the
  // on-disk image reflects the in-memory state without mutation.
  bool save() const;

  std::string currentProjectName() const { return current_project_; }
  // Selects the active project. Returns false when `name` is unknown.
  bool setCurrentProject(const std::string& name);

  // Names of all configured projects (sorted).
  std::vector<std::string> projects() const;

  bool hasProject(const std::string& name) const;
  const ProjectConfig* project(const std::string& name) const;
  ProjectConfig* mutableProject(const std::string& name);
  const ProjectConfig* activeProject() const;

  // Inserts or replaces a project entry.
  bool addOrUpdateProject(const ProjectConfig& p);
  // Overrides the active project's root (implements --project-dir).
  // No-op when there is no active project.
  void setActiveProjectRoot(const std::string& root);
  std::string activeRoot() const;

  // Path hierarchy (precedence: CLI > project > CWD walk-up fallback).
  std::string resolveOverridesDir(const std::string& cli_explicit = "") const;
  std::string resolveEnhancementsDir(
      const std::string& cli_explicit = "") const;
  std::string resolveConstantsPath(const std::string& cli_explicit = "") const;
  std::string resolveHeadersDir(const std::string& cli_explicit = "") const;

 private:
  std::string resolveField(const std::string& cli_explicit,
                           const std::string& project_value,
                           const std::string& fallback_relative) const;
  std::string fallbackSearch(const std::string& relative) const;
  static bool isAbsolutePath(const std::string& p);
  static std::string joinRoot(const std::string& root,
                              const std::string& rel);

  std::string config_path_;
  std::string current_project_ = "pokeyellow";
  std::map<std::string, ProjectConfig> projects_;
};

}  // namespace audio_dbg

#endif  // DOS_GB_AUDIO_DBG_CONFIG_H_
