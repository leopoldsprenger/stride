#include "config.h"

#include <fstream>

#include "util.h"

Config::Config(std::filesystem::path path) : path_(std::move(path)) { load(); }

void Config::load() {
  values_.clear();
  std::ifstream f(path_);
  if (!f) return;
  std::string line;
  while (std::getline(f, line)) {
    line = trimmed(line);
    if (line.empty() || line[0] == '#') continue;
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    values_[trimmed(line.substr(0, eq))] = trimmed(line.substr(eq + 1));
  }
}

std::optional<std::string> Config::get(const std::string& key) const {
  auto it = values_.find(key);
  if (it == values_.end() || it->second.empty()) return std::nullopt;
  return it->second;
}

void Config::set(const std::string& key, const std::string& value) {
  values_[key] = value;
  save();
}

void Config::save() const {
  std::filesystem::create_directories(path_.parent_path());
  std::ofstream f(path_, std::ios::binary | std::ios::trunc);
  f << "# Stride config -- generated/edited by Stride itself (or your home-manager module).\n";
  for (auto& [k, v] : values_) f << k << '=' << v << '\n';
}
