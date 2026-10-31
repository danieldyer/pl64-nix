#pragma once
#include "types.hpp"
#include <nlohmann/json.hpp>
namespace ins {
Config configuration(const nlohmann::json &);
nlohmann::json configuration_json(const Config &);
} // namespace ins
