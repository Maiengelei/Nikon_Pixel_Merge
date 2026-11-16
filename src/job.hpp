#pragma once
#include "groups.hpp"
#include <functional>
#include <stdexcept>
#include <string>

namespace npm {
using Progress = std::function<void(double, const std::string&)>;
struct Cancelled : std::runtime_error { Cancelled() : std::runtime_error("已取消合并") {} };
std::string group_problem(const Group&);
std::string suggested_filename(const Group&, bool correction);
void export_group(const Group&, const std::filesystem::path&, bool correction, const Progress& = {});
}
