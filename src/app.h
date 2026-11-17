#pragma once
#include <string>
#include <vector>

namespace app {

void init(float dpi_scale);
void frame();
void shutdown();
void drop_file(const std::string& utf8_path);

// Command line use without a window. Returns -1 when the arguments ask for nothing.
int run_cli(const std::vector<std::string>& args);

}  // namespace app
