#include <filesystem>
#include <iostream>

#include "app.h"
#include "store.h"
#include "util.h"

int main() {
  try {
    auto dir = dataDir();
    std::filesystem::create_directories(dir);
    Store store((dir / "stride.db").string());
    App(store).run();
  } catch (const std::exception& e) {
    std::cerr << "stride: " << e.what() << '\n';
    return 1;
  }
}
