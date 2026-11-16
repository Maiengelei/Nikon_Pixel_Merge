#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>

namespace npm {
// Samples are native 14-bit sensor values, including the camera's black offset.
struct RawPlane { int width, height; std::vector<uint16_t> pixels; };
struct RGBImage { int width, height; std::vector<uint16_t> pixels; };
RawPlane read_nef(const std::filesystem::path& path);
RGBImage read_nefx(const std::filesystem::path& path);
}
