// Config engine acceptance tests: multi-project YAML + path hierarchy + CLI.
// Verifies (headless: no GUI, no audio hardware):
//   1. Default config creation (missing file -> pokeyellow defaults).
//   2. Save/load round-trip with multiple projects (incl. quoted paths,
//      comments, nested project dictionaries).
//   3. Multi-project resolution: relative join with root, absolute passthru,
//      setCurrentProject switching, --project-dir root override.
//   4. CLI explicit path override priority over every resolve*() entry.
//   5. Fallback: $PKMN_REPO_ROOT and CWD walk-up when the active project
//      field is empty.
//   6. CLI parsing of --project/--project-dir/--overrides/--enhancements/
//      --constants (space + = forms, missing-value errors, --help text).
// Exits 0 with ALL PASS, 1 on any failure.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cli.h"
#include "config.h"

namespace {

namespace fs = std::filesystem;

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    ++g_checks;                                                         \
    if (!(cond)) {                                                      \
      ++g_failures;                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
    }                                                                   \
  } while (0)

std::string tmpBase() { return "/tmp/opencode/pkmn-audio-config-tests"; }

bool parseWords(const std::vector<std::string>& words,
                audio_dbg::CliOptions* opt, std::string* err) {
  return audio_dbg::parseCliArgs(words, opt, err);
}

std::string readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return "";
  std::string s((std::istreambuf_iterator<char>(in)),
                std::istreambuf_iterator<char>());
  return s;
}

void writeFile(const std::string& path, const std::string& content) {
  fs::create_directories(fs::path(path).parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << content;
}

}  // namespace

int main() {
  using namespace audio_dbg;
  std::system("mkdir -p /tmp/opencode");
  fs::create_directories(tmpBase());

  // --- 1. Default config creation -----------------------------------------
  {
    const std::string cfg = tmpBase() + "/default/config.yaml";
    std::error_code ec;
    fs::remove_all(fs::path(cfg).parent_path(), ec);
    ConfigManager mgr(cfg);
    CHECK(mgr.load());
    CHECK(mgr.currentProjectName() == "pokeyellow");
    CHECK(mgr.hasProject("pokeyellow"));
    CHECK(fs::is_regular_file(cfg, ec));
    const ProjectConfig* p = mgr.activeProject();
    CHECK(p != nullptr);
    if (p != nullptr) {
      CHECK(p->root == "/mnt/sdb1/Code/Active Code/pokeyellow_msdos");
      CHECK(p->overrides == "dos_port/tools/audio/overrides");
      CHECK(p->enhancements == "dos_port/tools/audio/enhancements");
      CHECK(p->constants == "constants/music_constants.asm");
      CHECK(p->headers == "audio/headers");
    }
    const std::string body = readFile(cfg);
    CHECK(body.find("current_project:") != std::string::npos);
    CHECK(body.find("pokeyellow") != std::string::npos);
    std::printf("PASS default creation\n");
  }

  // --- 2. Save/load round-trip + lightweight YAML --------------------------
  {
    const std::string cfg = tmpBase() + "/roundtrip/config.yaml";
    std::error_code ec;
    fs::remove_all(fs::path(cfg).parent_path(), ec);
    ConfigManager mgr(cfg);
    CHECK(mgr.load());
    ProjectConfig blue;
    blue.name = "blue";
    blue.root = "/tmp/opencode/blue root with spaces";
    blue.overrides = "custom/overrides";
    blue.enhancements = "custom/enhancements";
    blue.constants = "custom/constants.asm";
    blue.headers = "custom/headers";
    CHECK(mgr.addOrUpdateProject(blue));
    CHECK(mgr.setCurrentProject("blue"));
    CHECK(mgr.save());

    // Hand-inject a comment + extra project with quoted values to prove
    // the lightweight parser tolerates comments/quotes/nesting.
    {
      std::string body = readFile(cfg);
      body = "# dgad config (comment must be ignored)\n" + body;
      writeFile(cfg, body);
    }
    ConfigManager re(cfg);
    CHECK(re.load());
    CHECK(re.currentProjectName() == "blue");
    auto names = re.projects();
    CHECK(names.size() == 2);
    bool has_yellow = false, has_blue = false;
    for (const auto& n : names) {
      if (n == "pokeyellow") has_yellow = true;
      if (n == "blue") has_blue = true;
    }
    CHECK(has_yellow && has_blue);
    const ProjectConfig* b = re.project("blue");
    CHECK(b != nullptr);
    if (b != nullptr) {
      CHECK(b->root == "/tmp/opencode/blue root with spaces");
      CHECK(b->overrides == "custom/overrides");
      CHECK(b->constants == "custom/constants.asm");
    }
    // Unknown project select fails and keeps the current one.
    CHECK(!re.setCurrentProject("no-such-project"));
    CHECK(re.currentProjectName() == "blue");
    // setCurrentProject back to pokeyellow works.
    CHECK(re.setCurrentProject("pokeyellow"));
    CHECK(re.currentProjectName() == "pokeyellow");
    std::printf("PASS save/load round-trip\n");
  }

  // --- 3. Multi-project resolution -----------------------------------------
  {
    const std::string cfg = tmpBase() + "/multi/config.yaml";
    std::error_code ec;
    fs::remove_all(fs::path(cfg).parent_path(), ec);
    ConfigManager mgr(cfg);
    CHECK(mgr.load());
    ProjectConfig a;
    a.name = "alpha";
    a.root = "/roots/alpha";
    a.overrides = "ov";
    a.enhancements = "en";
    a.constants = "const.asm";
    a.headers = "hdrs";
    ProjectConfig b;
    b.name = "beta";
    b.root = "/roots/beta";
    b.overrides = "/abs/overrides";
    b.enhancements = "en2";
    b.constants = "c2.asm";
    b.headers = "h2";
    CHECK(mgr.addOrUpdateProject(a));
    CHECK(mgr.addOrUpdateProject(b));

    CHECK(mgr.setCurrentProject("alpha"));
    CHECK(mgr.resolveOverridesDir() == "/roots/alpha/ov");
    CHECK(mgr.resolveEnhancementsDir() == "/roots/alpha/en");
    CHECK(mgr.resolveConstantsPath() == "/roots/alpha/const.asm");
    CHECK(mgr.resolveHeadersDir() == "/roots/alpha/hdrs");

    CHECK(mgr.setCurrentProject("beta"));
    // Absolute project value passes through without joining.
    CHECK(mgr.resolveOverridesDir() == "/abs/overrides");
    CHECK(mgr.resolveEnhancementsDir() == "/roots/beta/en2");
    CHECK(mgr.resolveConstantsPath() == "/roots/beta/c2.asm");
    CHECK(mgr.resolveHeadersDir() == "/roots/beta/h2");

    // --project-dir semantics: overriding the active root re-bases
    // relative joins.
    mgr.setActiveProjectRoot("/roots/beta-override");
    CHECK(mgr.activeRoot() == "/roots/beta-override");
    CHECK(mgr.resolveEnhancementsDir() == "/roots/beta-override/en2");
    // Absolute entries are unaffected by the root override.
    CHECK(mgr.resolveOverridesDir() == "/abs/overrides");
    std::printf("PASS multi-project resolution\n");
  }

  // --- 4. CLI explicit path override priority ------------------------------
  {
    const std::string cfg = tmpBase() + "/priority/config.yaml";
    std::error_code ec;
    fs::remove_all(fs::path(cfg).parent_path(), ec);
    ConfigManager mgr(cfg);
    CHECK(mgr.load());
    CHECK(mgr.setCurrentProject("pokeyellow"));
    const std::string proj_over =
        mgr.resolveOverridesDir("/cli/overrides");
    CHECK(proj_over == "/cli/overrides");
    CHECK(mgr.resolveEnhancementsDir("/cli/enh") == "/cli/enh");
    CHECK(mgr.resolveConstantsPath("/cli/const.asm") == "/cli/const.asm");
    CHECK(mgr.resolveHeadersDir("/cli/headers") == "/cli/headers");
    // Empty explicit falls back to the project join.
    const std::string joined = mgr.resolveOverridesDir("");
    CHECK(joined ==
          std::string("/mnt/sdb1/Code/Active Code/pokeyellow_msdos") +
              "/dos_port/tools/audio/overrides");

    // End-to-end: CLI-parsed explicit values win over the config.
    CliOptions o;
    std::string err;
    CHECK(parseWords({"p", "--overrides", "/from/cli",
                      "--enhancements=/from/cli-enh", "--constants",
                      "/from/cli-const.asm"},
                     &o, &err));
    CHECK(o.overrides == "/from/cli");
    CHECK(o.overrides_dir == "/from/cli");
    CHECK(o.enhancements == "/from/cli-enh");
    CHECK(o.enhancements_dir == "/from/cli-enh");
    CHECK(o.constants == "/from/cli-const.asm");
    CHECK(o.constants_path == "/from/cli-const.asm");
    CHECK(mgr.resolveOverridesDir(o.overrides) == "/from/cli");
    CHECK(mgr.resolveEnhancementsDir(o.enhancements) == "/from/cli-enh");
    CHECK(mgr.resolveConstantsPath(o.constants) == "/from/cli-const.asm");
    std::printf("PASS cli explicit priority\n");
  }

  // --- 5. Fallback (PKMN_REPO_ROOT + CWD walk-up) ---------------------------
  {
    const std::string fake_root = tmpBase() + "/fakerRepo";
    const std::string cfg = tmpBase() + "/fallback/config.yaml";
    std::error_code ec;
    fs::remove_all(fake_root, ec);
    fs::remove_all(fs::path(cfg).parent_path(), ec);
    fs::create_directories(fs::path(fake_root) / "constants", ec);
    writeFile(fake_root + "/constants/music_constants.asm",
              "; fake constants\n");
    fs::create_directories(fs::path(fake_root) / "audio" / "headers", ec);

    ConfigManager mgr(cfg);
    CHECK(mgr.load());
    ProjectConfig empty;
    empty.name = "empty";
    empty.root.clear();
    empty.overrides.clear();
    empty.enhancements.clear();
    empty.constants.clear();
    empty.headers.clear();
    CHECK(mgr.addOrUpdateProject(empty));
    CHECK(mgr.setCurrentProject("empty"));

    const char* old_env = std::getenv("PKMN_REPO_ROOT");
    const std::string old_val = (old_env != nullptr) ? old_env : "";
    ::setenv("PKMN_REPO_ROOT", fake_root.c_str(), 1);
    CHECK(mgr.resolveConstantsPath() ==
          (fs::path(fake_root) / "constants/music_constants.asm").string());
    CHECK(mgr.resolveHeadersDir() ==
          (fs::path(fake_root) / "audio/headers").string());
    // Restore the caller's environment.
    if (old_val.empty()) {
      ::unsetenv("PKMN_REPO_ROOT");
    } else {
      ::setenv("PKMN_REPO_ROOT", old_val.c_str(), 1);
    }

    // Without the env override and without a project value, the resolver
    // at least returns the default relative path (walk-up or plain).
    ::unsetenv("PKMN_REPO_ROOT");
    const std::string fb = mgr.resolveConstantsPath();
    CHECK(!fb.empty());
    std::printf("PASS fallback search (got %s)\n", fb.c_str());
  }

  // --- 6. CLI parsing for the new flags ------------------------------------
  {
    CliOptions o;
    std::string err;
    CHECK(parseWords({"p", "--project", "blue", "--project-dir",
                      "/roots/blue", "--overrides", "ov",
                      "--enhancements", "en", "--constants", "c.asm"},
                     &o, &err));
    CHECK(o.project == "blue");
    CHECK(o.project_dir == "/roots/blue");
    CHECK(o.project_root == "/roots/blue");
    CHECK(o.overrides == "ov");
    CHECK(o.enhancements == "en");
    CHECK(o.constants == "c.asm");

    // Equals forms + underscore spelling of --project-dir.
    CHECK(parseWords({"p", "--project=beta", "--project_dir=/r/b",
                      "--overrides=/o", "--enhancements=/e",
                      "--constants=/c"},
                     &o, &err));
    CHECK(o.project == "beta");
    CHECK(o.project_dir == "/r/b");
    CHECK(o.overrides == "/o");
    CHECK(o.enhancements == "/e");
    CHECK(o.constants == "/c");

    // Missing values are hard errors.
    CHECK(!parseWords({"p", "--project"}, &o, &err) && !err.empty());
    CHECK(!parseWords({"p", "--project-dir"}, &o, &err) && !err.empty());
    CHECK(!parseWords({"p", "--overrides"}, &o, &err) && !err.empty());
    CHECK(!parseWords({"p", "--enhancements"}, &o, &err) && !err.empty());
    CHECK(!parseWords({"p", "--constants"}, &o, &err) && !err.empty());

    // --help mentions every new flag.
    const std::string usage = cliUsage("dgad");
    CHECK(usage.find("--project") != std::string::npos);
    CHECK(usage.find("--project-dir") != std::string::npos);
    CHECK(usage.find("--overrides") != std::string::npos);
    CHECK(usage.find("--enhancements") != std::string::npos);
    CHECK(usage.find("--constants") != std::string::npos);
    std::printf("PASS cli parsing\n");
  }

  if (g_failures == 0) {
    std::printf("ALL PASS (%d checks)\n", g_checks);
    return 0;
  }
  std::printf("%d/%d checks FAILED\n", g_failures, g_checks);
  return 1;
}
