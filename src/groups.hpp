#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace npm {
struct FrameInfo {
    std::filesystem::path path;
    std::string group_id, model, lens;
    double focal_mm = 0, aperture = 0, exposure_seconds = 0;
    unsigned iso = 0;
    int index = 0; // 1-based order recorded by the camera
    int expected = 0;
    double shift_x = 0, shift_y = 0; // measured sensor displacement, in source pixels
};

struct Group {
    std::string id, model;
    int expected = 0;
    std::vector<FrameInfo> frames;
    std::vector<int> missing, duplicates;
    std::vector<std::string> problems;
    bool complete() const {
        return expected > 0 && frames.size() == static_cast<std::size_t>(expected)
            && missing.empty() && duplicates.empty() && problems.empty();
    }
};

struct ScanIssue {
    std::filesystem::path path;
    std::string message;
};

struct ScanResult {
    std::vector<Group> groups;
    std::vector<ScanIssue> issues;
    std::size_t ordinary_nefs = 0;
};

// A normal exposure returns nullopt. Malformed or unsupported shift metadata throws.
std::optional<FrameInfo> inspect_frame(const std::filesystem::path& path);
// Inputs may be folders (recursive, no symlinked subfolders) or individual files.
// .nef is case-insensitive. Groups and frames have deterministic order.
ScanResult scan_groups(const std::vector<std::filesystem::path>& inputs,
                       const std::function<void()>& checkpoint = {});
} // namespace npm
