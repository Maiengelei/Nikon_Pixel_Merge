#pragma once
// Minimal, bounded reader for the little-endian classic TIFF layout used by
// the supported camera. Offsets in Nikon MakerNotes have their own origin.
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace npm::tiff {
inline void require(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
inline uint16_t u16(const uint8_t* p) { return p[0] | uint16_t(p[1])<<8; }
inline uint32_t u32(const uint8_t* p) { return u16(p) | uint32_t(u16(p+2))<<16; }
struct Tag { uint16_t id, type; uint32_t count; size_t entry, data, size; };
struct View {
    const uint8_t* bytes;
    size_t length;
    const uint8_t* get(size_t pos, size_t n) const {
        require(pos<=length && n<=length-pos,"TIFF offset outside file"); return bytes+pos;
    }
    std::vector<Tag> dir(size_t base, size_t offset) const {
        require(base<=length && offset<=length-base,"Invalid TIFF directory");
        const size_t start=base+offset;
        const int n=u16(get(start,2)); require(n<=2048,"Oversize TIFF directory");
        get(start+2,12*size_t(n)+4);
        std::vector<Tag> entries;
        static constexpr int widths[]={0,1,1,2,4,8,1,1,2,4,8,4,8,4};
        for (int i=0;i<n;i++) {
            const size_t pos=start+2+12*size_t(i);
            const uint8_t* p=bytes+pos; const uint16_t type=u16(p+2);
            require(type>0 && type<sizeof(widths)/sizeof(*widths),"Unsupported TIFF field type");
            const uint32_t count=u32(p+4); const size_t size=size_t(count)*widths[type];
            const size_t data=size<=4 ? pos+8 : base+size_t(u32(p+8));
            get(data,size);
            entries.push_back({u16(p),type,count,pos,data,size});
        }
        return entries;
    }
    const Tag& tag(const std::vector<Tag>& dir, uint16_t id) const {
        for (const auto& t:dir) if(t.id==id) return t;
        throw std::runtime_error("Missing TIFF field "+std::to_string(id));
    }
    uint32_t num(const Tag& t,size_t i=0) const {
        require(i<t.count,"Missing TIFF value"); const auto* p=bytes+t.data;
        switch(t.type) { case 1:return p[i]; case 3:return u16(p+2*i); case 4:return u32(p+4*i); default:throw std::runtime_error("Invalid numeric TIFF field"); }
    }
    std::vector<Tag> root() const {
        require(length>=8 && !std::memcmp(bytes,"II*\0",4),"Expected little-endian Nikon TIFF");
        return dir(0,u32(bytes+4));
    }
    std::vector<Tag> rawdir(const std::vector<Tag>& main) const {
        auto& subs=tag(main,330);
        for(size_t i=0;i<subs.count;i++) {
            auto d=dir(0,num(subs,i)); if(num(tag(d,254))==0) return d;
        }
        throw std::runtime_error("No RAW image directory");
    }
    struct Maker { size_t base; Tag block; std::vector<Tag> fields; };
    Maker nikon_block(const Tag& t) const {
        const size_t base=t.data+10;
        require(t.size>=18 && !std::memcmp(bytes+t.data,"Nikon\0",6) && !std::memcmp(bytes+base,"II*\0",4),"Invalid Nikon metadata");
        // Constrain all maker-relative offsets to this block.
        View local{bytes, t.data+t.size};
        return {base,t,local.dir(base,u32(bytes+base+4))};
    }
    Maker maker(const std::vector<Tag>& main) const {
        const auto exif=dir(0,num(tag(main,34665)));
        return nikon_block(tag(exif,37500));
    }
};
class Mapping {
    const uint8_t* data_; size_t size_;
public:
    explicit Mapping(const std::filesystem::path& path) {
        const int fd=open(path.c_str(),O_RDONLY);
        if(fd<0) throw std::runtime_error("Cannot open "+path.string()+": "+std::strerror(errno));
        struct stat st{}; if(fstat(fd,&st)!=0 || !S_ISREG(st.st_mode) || st.st_size<8) {
            close(fd); throw std::runtime_error("Invalid RAW file: "+path.string());
        }
        size_=size_t(st.st_size);
        void* p=mmap(nullptr,size_,PROT_READ,MAP_PRIVATE,fd,0); close(fd);
        if (p==MAP_FAILED) throw std::runtime_error("Cannot map RAW file: "+path.string());
        data_=static_cast<const uint8_t*>(p);
    }
    ~Mapping(){munmap(const_cast<uint8_t*>(data_),size_);}
    Mapping(const Mapping&)=delete; Mapping& operator=(const Mapping&)=delete;
    View view() const {return {data_,size_};}
};
}
