#include "bundle.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;

namespace {
void putU32(std::string& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out += (char)((v >> (8 * i)) & 0xff);
}
void putU64(std::string& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out += (char)((v >> (8 * i)) & 0xff);
}
uint32_t getU32(const std::string& s, size_t off) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= (uint32_t)(uint8_t)s[off + i] << (8 * i);
  return v;
}
uint64_t getU64(const std::string& s, size_t off) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= (uint64_t)(uint8_t)s[off + i] << (8 * i);
  return v;
}
std::string readFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream o;
  o << f.rdbuf();
  return o.str();
}
}  // namespace

std::string packDir(const fs::path& dir) {
  std::vector<fs::path> files;
  if (fs::exists(dir)) {
    for (auto& e : fs::recursive_directory_iterator(dir))
      if (e.is_regular_file()) files.push_back(e.path());
  }
  std::sort(files.begin(), files.end());

  std::string out;
  for (auto& f : files) {
    std::string rel = fs::relative(f, dir).generic_string();
    std::string content = readFile(f);
    putU32(out, (uint32_t)rel.size());
    out += rel;
    putU64(out, (uint64_t)content.size());
    out += content;
  }
  return out;
}

void unpackToDir(const std::string& blob, const fs::path& outDir) {
  fs::create_directories(outDir);
  size_t off = 0;
  while (off < blob.size()) {
    if (off + 4 > blob.size()) throw std::runtime_error("corrupt mirror bundle (truncated path length)");
    uint32_t pathLen = getU32(blob, off);
    off += 4;
    if (off + pathLen > blob.size()) throw std::runtime_error("corrupt mirror bundle (truncated path)");
    std::string rel = blob.substr(off, pathLen);
    off += pathLen;
    if (off + 8 > blob.size()) throw std::runtime_error("corrupt mirror bundle (truncated content length)");
    uint64_t contentLen = getU64(blob, off);
    off += 8;
    if (off + contentLen > blob.size()) throw std::runtime_error("corrupt mirror bundle (truncated content)");

    fs::path dest = outDir / fs::path(rel);
    fs::create_directories(dest.parent_path());
    std::ofstream f(dest, std::ios::binary | std::ios::trunc);
    f.write(blob.data() + off, (std::streamsize)contentLen);
    off += contentLen;
  }
}
