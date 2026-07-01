#pragma once

#include <cstddef>
#include <string>

namespace xiaoman
{

struct ConfigLoadResult
{
    bool loaded = false;
    std::string path;
    size_t applied = 0;
    size_t skipped_existing = 0;
};

ConfigLoadResult load_config_from_args(int argc, char **argv);

} // namespace xiaoman
