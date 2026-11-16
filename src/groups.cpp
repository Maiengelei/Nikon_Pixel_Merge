#include "groups.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace npm {
namespace {
namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;
using u64 = std::uint64_t;
using u32 = std::uint32_t;
using u16 = std::uint16_t;
using std::runtime_error;

// Read only the small TIFF directories and fields we need. Offsets inside a
// Nikon type-3 MakerNote are relative to its own TIFF header, not file start.
class File {
public:
    std::ifstream stream;
    u64 size;
    explicit File(const fs::path& name) : stream(name, std::ios::binary | std::ios::ate) {
        if (!stream || stream.tellg() < 0) throw runtime_error("无法打开文件");
        size = static_cast<u64>(stream.tellg());
    }
    Bytes read(u64 offset, std::size_t n) {
        if (offset > size || n > size - offset) throw runtime_error("元数据超出文件范围");
        Bytes b(n);
        stream.seekg(static_cast<std::streamoff>(offset));
        if (!stream.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(n)))
            throw runtime_error("无法读取元数据");
        return b;
    }
};

struct Field { u16 type; u32 count; u64 at; u64 bytes; };
using Directory = std::map<u16, Field>;

class Tiff {
    File& file;
    u64 base, size;
    bool little = true;
    void bounds(u64 at, u64 length) const {
        if (at > size || length > size - at) throw runtime_error("TIFF 字段越界");
    }
    Bytes read(u64 at, std::size_t bytes) const {
        bounds(at, bytes);
        return file.read(base + at, bytes);
    }
public:
    u32 first;
    Tiff(File& f, u64 at, u64 n) : file(f), base(at), size(n) {
        auto b = read(0, 8);
        little = b[0] == 'I' && b[1] == 'I';
        if ((!little && !(b[0] == 'M' && b[1] == 'M')) || dec16(b.data()+2) != 42)
            throw runtime_error("不能识别 TIFF 文件头");
        first = dec32(b.data()+4);
    }
    u16 dec16(const std::uint8_t* b) const {
        return little ? u16(b[0] | (u16(b[1]) << 8)) : u16((u16(b[0]) << 8) | b[1]);
    }
    u32 dec32(const std::uint8_t* b) const {
        return little ? u32(dec16(b)) | (u32(dec16(b + 2)) << 16)
                      : (u32(dec16(b)) << 16) | u32(dec16(b + 2));
    }
    Directory dir(u32 off) const {
        const auto n = dec16(read(off, 2).data());
        if (n > 4096) throw runtime_error("TIFF 目录过大");
        auto b = read(u64(off)+2, std::size_t(n)*12);
        Directory result;
        constexpr std::array<int,14> sizes{0,1,1,2,4,8,1,1,2,4,8,4,8,4};
        for (int i = 0; i < n; ++i) {
            auto e = b.data()+i*12;
            u16 tag = dec16(e), type = dec16(e+2);
            if (type >= sizes.size() || !sizes[type]) continue;
            u32 count = dec32(e+4);
            u64 bytes = u64(count)*sizes[type];
            u64 at = bytes <= 4 ? u64(off)+2+i*12+8 : dec32(e+8);
            result.emplace(tag, Field{type, count, at, bytes});
        }
        return result;
    }
    Bytes field(const Field& f, std::size_t max_size) const {
        if (f.bytes > max_size) throw runtime_error("TIFF 字段长度无效");
        return read(f.at, static_cast<std::size_t>(f.bytes));
    }
    std::string ascii(const Directory& d, u16 tag) const {
        auto it = d.find(tag);
        if (it == d.end()) return {};
        if (it->second.type != 2) throw runtime_error("文本字段格式错误");
        auto b = field(it->second,1024);
        auto end = std::find(b.begin(), b.end(), 0);
        while (end != b.begin() && end[-1] == ' ') --end;
        return std::string(b.begin(),end);
    }
    u32 integer(const Directory& d, u16 tag) const {
        auto it = d.find(tag);
        if (it == d.end()) throw runtime_error("缺少必要的整数字段");
        const Field& f = it->second;
        if (f.count != 1 || (f.type != 3 && f.type != 4)) throw runtime_error("整数字段格式错误");
        auto b = field(f,4);
        return f.type == 3 ? dec16(b.data()) : dec32(b.data());
    }
    double rational(const Directory& d, u16 tag) const {
        auto it = d.find(tag);
        if (it == d.end()) return 0.;
        const Field& f = it->second;
        if (f.count != 1 || f.type != 5) throw runtime_error("有理数字段格式错误");
        auto b = field(f,8);
        u32 den = dec32(b.data()+4);
        if (!den) throw runtime_error("无效的曝光或镜头参数");
        return double(dec32(b.data()))/den;
    }
    Bytes block(const Directory& d,u16 tag,std::size_t min_length) const {
        auto it = d.find(tag);
        if (it == d.end() || it->second.type != 7 || it->second.bytes < min_length)
            throw runtime_error("缺少有效的像素位移元数据");
        auto b = field(it->second,1024);
        if (!(b[0]=='0' && b[1]=='1' && b[2]=='0' && b[3]=='0'))
            throw runtime_error("不支持此版本的像素位移元数据");
        return b;
    }
};

// Reversible encoding keeps arbitrary camera/serial text distinct and the ID
// safe in a filename. Ordinary alphanumeric camera names remain readable.
std::string key_text(const std::string& s) {
    std::ostringstream out;
    for (unsigned char c : s) {
        if ((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9')) out << c;
        else out << '_' << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
    }
    return out.str();
}
bool nef(const fs::path& p) {
    auto ext = p.extension().string();
    std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return char(std::tolower(c));});
    return ext == ".nef";
}
} // namespace

std::optional<FrameInfo> inspect_frame(const std::filesystem::path& path) {
    File f(path);
    Tiff t(f,0,f.size);
    auto ifd = t.dir(t.first);
    if (t.ascii(ifd,0x010f).rfind("NIKON",0) != 0 || !ifd.count(0x8769)) return {};
    auto model = t.ascii(ifd,0x0110);
    auto exif = t.dir(t.integer(ifd,0x8769));
    auto ni = exif.find(0x927c);
    if (ni == exif.end()) return {};
    auto note = ni->second;
    if (note.bytes < 18 || note.type != 7) return {};
    if (note.at > f.size || note.bytes > f.size - note.at) throw runtime_error("Nikon 元数据已截断");
    auto sig = f.read(note.at,10);
    if (!(sig[0]=='N' && sig[1]=='i' && sig[2]=='k' && sig[3]=='o' && sig[4]=='n'
        && sig[5]==0 && sig[6]==2)) return {};
    Tiff nt(f,note.at+10,note.bytes-10);
    auto nd = nt.dir(nt.first);
    if (!nd.count(0x005e)) return {};
    auto ps = nt.block(nd,0x005e,6);
    if (!ps[4] && !ps[5]) return {}; // regular exposure
    if (ps[4]<1 || ps[4]>4) throw runtime_error("不支持的像素位移拍摄模式");
    FrameInfo info;
    info.path = path;
    info.model = model;
    info.expected = 2 << ps[4]; // modes 1..4 -> 4,8,16,32
    // The eight-shot pattern uses positions 1..4 then 17..20 (the
    // second pass of the 32-position sensor route), not positions 5..8.
    info.index = info.expected == 8 && ps[5] >= 17 && ps[5] <= 20 ? ps[5]-12 : ps[5];
    if (info.expected == 8 && ps[5] >= 5 && ps[5] <= 8) throw runtime_error("8 张模式的位移位置无效");
    if (info.index < 1 || info.index > info.expected) throw runtime_error("像素位移帧号超出拍摄模式范围");
    auto burst = nt.block(nd,0x0056,12);
    u32 burst_id = nt.dec32(burst.data()+4);
    if (nt.dec32(burst.data()+8) != static_cast<u32>(info.index)) throw runtime_error("连拍帧号与像素位移帧号冲突");
    u32 shutter = nt.integer(nd,0x00a7);
    if (shutter < u32(info.index-1)) throw runtime_error("快门计数与帧号冲突");
    auto serial = nt.ascii(nd,0x001d);
    if (serial.empty()) serial = t.ascii(exif,0xa431);
    if (serial.empty() || model.empty()) throw runtime_error("缺少相机身份信息，不能可靠分组");
    info.group_id = key_text(model) + "-" + key_text(serial) + "-" + std::to_string(burst_id)
                    + "-" + std::to_string(shutter-info.index+1);
    auto shift = nt.block(nd,0x0054,20);
    auto signed32 = [&](int pos) {
        auto n = nt.dec32(shift.data()+pos);
        return n >= 0x80000000u ? std::int64_t(n)-0x100000000LL : std::int64_t(n);
    };
    if (signed32(8)<=0 || signed32(16)<=0) throw runtime_error("传感器位移分母无效");
    info.shift_x = double(signed32(4))/signed32(8);
    info.shift_y = double(signed32(12))/signed32(16);
    info.lens = t.ascii(exif,0xa434);
    info.focal_mm = t.rational(exif,0x920a);
    info.aperture = t.rational(exif,0x829d);
    info.exposure_seconds = t.rational(exif,0x829a);
    if (exif.count(0x8827)) info.iso = t.integer(exif,0x8827);
    return info;
}

ScanResult scan_groups(const std::vector<std::filesystem::path>& inputs, const std::function<void()>& checkpoint) {
    ScanResult out;
    std::set<fs::path> files;
    auto add = [&](const fs::path& p) {
        if (!nef(p)) return;
        std::error_code ec;
        auto canonical = fs::canonical(p,ec);
        if (ec) out.issues.push_back({p,ec.message()});
        else files.insert(std::move(canonical));
    };
    for (auto& p : inputs) {
        std::error_code ec;
        auto s = fs::status(p,ec);
        if (ec || !fs::exists(s)) {out.issues.push_back({p,ec ? ec.message() : "路径不存在"}); continue;}
        if (!fs::is_directory(s)) {if (fs::is_regular_file(s)) add(p); continue;}
        fs::recursive_directory_iterator it(p,ec), end;
        if (ec) {out.issues.push_back({p,ec.message()}); continue;}
        while (it != end) {
            if (checkpoint) checkpoint();
            auto entry = it->path();
            if (nef(entry)) {
                if (it->is_regular_file(ec)) add(entry);
                if (ec) out.issues.push_back({entry,ec.message()});
            }
            it.increment(ec);
            if (ec) out.issues.push_back({entry,ec.message()});
        }
    }
    std::map<std::string,Group> groups;
    for (auto& p : files) {
        if (checkpoint) checkpoint();
        try {
            auto info = inspect_frame(p);
            if (!info) {++out.ordinary_nefs; continue;}
            Group& g = groups[info->group_id];
            if (g.frames.empty()) {
                g.id = info->group_id;
                g.model = info->model;
                g.expected = info->expected;
            } else {
                const auto& a = g.frames.front();
                if (g.problems.empty() && (info->expected != g.expected || info->lens != a.lens ||
                    info->iso != a.iso || info->aperture != a.aperture || info->focal_mm != a.focal_mm ||
                    info->exposure_seconds != a.exposure_seconds))
                    g.problems.push_back("同组照片的拍摄模式、曝光或镜头设置不一致");
            }
            g.frames.push_back(std::move(*info));
        } catch (const std::exception& e) {out.issues.push_back({p,e.what()});}
    }
    for (auto& pair : groups) {
        auto& g = pair.second;
        std::stable_sort(g.frames.begin(),g.frames.end(),[](const auto& a,const auto& b){return a.index<b.index;});
        std::array<std::size_t,33> counts{};
        for (auto& f : g.frames) ++counts[f.index];
        for (int i=1;i<=g.expected;++i) {
            if (counts[i]==0) g.missing.push_back(i);
            if (counts[i]>1) g.duplicates.push_back(i);
        }
        out.groups.push_back(std::move(g));
    }
    return out;
}
} // namespace npm
