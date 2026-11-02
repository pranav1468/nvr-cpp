#pragma once

#include "nvr/common/types.h"
#include <string>

namespace nvr {

// Unified configuration loader for server and local GUI subsystems
NvrConfig LoadNvrConfig(const std::string& config_path);

} // namespace nvr
