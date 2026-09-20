#pragma once
// Tiny key=value config file at $STRIDE_DATA_DIR/config (one `key=value`
// per line, blank lines and '#' comments ignored). Deliberately hand-rolled
// instead of pulling in a YAML/TOML library: it's simple enough for a
// home-manager module to write directly with `home.file`, and one fewer
// dependency to package for Nix.
#include <filesystem>
#include <map>
#include <optional>
#include <string>

class Config {
 public:
  explicit Config(std::filesystem::path path);

  std::optional<std::string> get(const std::string& key) const;
  // Rewrites the whole file with this key set (others preserved).
  void set(const std::string& key, const std::string& value);

 private:
  std::filesystem::path path_;
  std::map<std::string, std::string> values_;
  void load();
  void save() const;
};
