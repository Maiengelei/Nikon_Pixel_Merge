#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>

namespace npm {
// Writes to a caller-owned temporary path; only publish it after finish().
// row() receives exactly width*3 samples in RGB order, 14-bit sensor codes.
class NefxWriter {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    NefxWriter(const std::filesystem::path& temporary, const std::filesystem::path& first_nef,
               int width, int height, bool correction, unsigned frame_count);
    ~NefxWriter();
    void row(const uint16_t* rgb);
    void finish();
};
}
