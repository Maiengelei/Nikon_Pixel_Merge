#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <vector>

namespace npm {
// Full RGGB sensor raster and active crop, in pre-merge pixels.
struct MergeGeometry { int raw_width,raw_height,left,top,width,height; };
struct CAProfile { std::array<double,4> red,blue; double sensor_x=0,sensor_y=0; };
CAProfile read_ca_profile(const std::filesystem::path& nef);
// Sequential callbacks; input includes the native 14-bit black pedestal.
// Only 16/32-frame captures double the width and height of the active crop.
void merge(int frame_count, MergeGeometry geometry, const CAProfile* correction,
           std::function<std::vector<uint16_t>(int)> read_raw,
           std::function<void(int,const uint16_t*,int)> write_row);
}
