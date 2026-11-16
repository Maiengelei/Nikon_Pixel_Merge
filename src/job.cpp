#include "job.hpp"
#include "raw.hpp"
#include "raw_tiff.hpp"
#include "merge.hpp"
#include "output.hpp"
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <deque>
#include <future>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <unistd.h>

namespace fs=std::filesystem;
namespace npm {
namespace {
std::string join(const std::vector<int>& numbers) {
    std::string s;
    for (int n:numbers) { if(!s.empty()) s+=", "; s+=std::to_string(n); }
    return s;
}
struct TempFile {
    fs::path path;
    explicit TempFile(const fs::path& parent) {
        std::string name=(parent/".nikon-merge-XXXXXX").string();
        int fd=mkstemp(name.data());
        if(fd<0) throw std::system_error(errno,std::generic_category(),"无法在目标文件夹创建临时文件");
        close(fd); path=name;
    }
    ~TempFile() { std::error_code ec; if(!path.empty()) fs::remove(path,ec); }
    void publish(const fs::path& dest) {
#ifdef __APPLE__
        if(renamex_np(path.c_str(),dest.c_str(),RENAME_EXCL)!=0)
#else
        if(link(path.c_str(),dest.c_str())!=0)
#endif
            throw std::system_error(errno,std::generic_category(),"无法保存（不会覆盖已有文件）");
    }
};
int validate_sensor(const fs::path& file) {
    tiff::Mapping mapping(file); auto t=mapping.view();
    auto root=t.root(),raw=t.rawdir(root); auto maker=t.maker(root);
    auto region=t.tag(maker.fields,0x45);
    if (t.num(t.tag(raw,256))!=8280 || t.num(t.tag(raw,257))!=5520 ||
        region.count!=4 || t.num(region,0)!=12 || t.num(region,1)!=8 || t.num(region,2)!=8256 || t.num(region,3)!=5504)
        throw std::runtime_error("此版本仅支持 Z 8 的 FX 全尺寸像素位移序列");
    return static_cast<int>(t.num(t.tag(root,274)));
}
}
std::string group_problem(const Group& g) {
    std::string s;
    if (!g.missing.empty()) s="缺帧："+join(g.missing);
    if (!g.duplicates.empty()) { if(!s.empty()) s+="；"; s+="重复帧："+join(g.duplicates); }
    for (const auto& issue:g.problems) { if(!s.empty()) s+="；"; s+=issue; }
    if (!s.empty()) return s;
    if (!g.complete()) return "序列不完整";
    if (g.model!="NIKON Z 8") return "此版本仅支持 Nikon Z 8";
    // Verify the recorded sensor positions before using the corresponding Z 8 route.
    // Positions are measured, so do not demand exact floating-point half steps.
    constexpr int dx[]={0,1,1,0}, dy[]={0,0,1,1};
    const int cycle=g.expected>=16?16:4;
    for (auto& f:g.frames) {
        int i=f.index-1, phase=cycle==16?(i/4)%4:0;
        if (std::abs(f.shift_x+dx[i%4]+0.5*dx[phase])>=0.25 ||
            std::abs(f.shift_y+dy[i%4]+0.5*dy[phase]+2*(i/cycle))>=0.25)
            return "第 "+std::to_string(f.index)+" 帧的传感器位置不符合已验证的位移模式";
    }
    return {};
}
std::string suggested_filename(const Group& g,bool ca) {
    return g.frames.front().path.stem().string()+"_"+std::to_string(g.expected)+(ca?"f_CA":"f")+".nefx";
}
void export_group(const Group& g,const fs::path& destination,bool ca,const Progress& progress) {
    auto announce=[&](double pct,const std::string& s){if(progress) progress(pct,s);};
    auto problem=group_problem(g);
    if(!problem.empty()) throw std::invalid_argument(problem);
    std::string ext=destination.extension().string();
    std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return char(std::tolower(c));});
    if(ext!=".nefx") throw std::invalid_argument("请使用 .nefx 扩展名");
    auto dest=fs::absolute(destination);
    if(fs::exists(dest)) throw std::runtime_error("目标文件已存在，请另选文件名");
    announce(0,"正在核对帧序与传感器尺寸…");
    const auto& first=g.frames.front().path;
    const int orientation=validate_sensor(first);
    for (const auto& f:g.frames) {
        auto live=inspect_frame(f.path);
        if(!live || live->group_id!=g.id || live->index!=f.index || live->expected!=g.expected ||
           live->shift_x!=f.shift_x || live->shift_y!=f.shift_y || live->iso!=f.iso || live->lens!=f.lens || live->focal_mm!=f.focal_mm ||
           live->aperture!=f.aperture || live->exposure_seconds!=f.exposure_seconds ||
           validate_sensor(f.path)!=orientation)
            throw std::runtime_error("文件已改变或方向不一致，请重新扫描："+f.path.filename().string());
    }
    const int scale=g.expected>=16?2:1;
    constexpr MergeGeometry geometry{8280,5520,12,8,8256,5504}; // checked against every file above
    const int w=8256*scale,h=5504*scale;
    if(fs::space(dest.parent_path()).available < uintmax_t(w)*h*3*14/8+12*1024*1024)
        throw std::runtime_error("目标磁盘可用空间不足");
    std::optional<CAProfile> lens;
    if(ca) {
        try { lens=read_ca_profile(first); }
        catch (const std::runtime_error&) {
            throw std::runtime_error("无法读取受支持的镜头色差配置。可以关闭色差矫正后重新合并。");
        }
    }
    TempFile tmp(dest.parent_path());
    NefxWriter writer(tmp.path,first,w,h,ca,g.expected);
    // Decode separate predictor streams in parallel while bounding the number of resident RAW planes.
    std::deque<std::future<RawPlane>> pending;
    int scheduled=0,consumed=0;
    size_t depth=std::clamp(std::thread::hardware_concurrency(),1u,3u);
    auto fill=[&](){
        while(pending.size()<depth && scheduled<g.expected) {
            auto path=g.frames[scheduled++].path;
            pending.emplace_back(std::async(std::launch::async,[path]{return read_nef(path);}));
        }
    };
    fill();
    merge(g.expected,geometry,lens?&*lens:nullptr,[&](int i){
        if(i!=consumed || pending.empty()) throw std::logic_error("合并器帧序错误");
        announce(.70*i/g.expected,"正在读取并合并第 "+std::to_string(i+1)+" / "+std::to_string(g.expected)+" 帧");
        auto image=pending.front().get(); pending.pop_front(); ++consumed; fill();
        return std::move(image.pixels);
    },[&](int y,const uint16_t* rgb,int width){
        if(width!=w || y<0 || y>=h) throw std::logic_error("输出尺寸不一致");
        if(y%96==0) announce(.70+.29*y/h,ca?"正在校正色差并写入合并图…":"正在写入合并图…");
        writer.row(rgb);
    });
    writer.finish(); announce(.999,"正在保存文件…");
    tmp.publish(dest);
}
}
