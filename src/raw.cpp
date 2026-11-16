#include "raw.hpp"
#include "raw_tiff.hpp"
#include <array>

namespace npm {
namespace {
using namespace tiff;
struct Prefix { uint8_t prefixBits, valueBits; };
// Nikon's fixed canonical codebook for 14-bit lossless NEF. Field values
// cross-checked with the LibRaw decoder and Z 8 reference captures.
constexpr auto codebook() {
    constexpr int counts[]={0,1,4,2,2,3,1,2};
    constexpr int symbols[]={7,6,8,5,9,4,10,3,11,12,2,0,1,13,14};
    std::array<Prefix,256> table{};
    int code=0,k=0;
    for(int bits=1;bits<=8;bits++,code<<=1){
        for(int i=0;i<counts[bits-1];i++,code++,k++){
            for(int j=code<<(8-bits); j<((code+1)<<(8-bits));j++)
                table[j]={uint8_t(bits),uint8_t(symbols[k])};
        }
    }
    return table;
}
constexpr auto prefixes=codebook();
struct Strip {int w,h; const uint8_t* data; size_t length;};
Strip strip(View t, const std::vector<Tag>& r, bool rgb){
    auto field=[&](uint16_t id){return t.num(t.tag(r,id));};
    const auto w=field(256),h=field(257);
    require(w==(rgb?8256u:8280u) || (rgb && w==16512),"Unsupported RAW width (FX Z 8 required)");
    require(h==(rgb?(w==8256?5504u:11008u):5520u),"Unsupported RAW height");
    require(field(259)==34713 && field(262)==(rgb?2u:32803u) && field(277)==(rgb?3u:1u)
        && field(284)==1 && field(278)==h,"此 RAW 排列或压缩模式暂不支持，请使用 Z 8 的 14 位无损压缩 NEF");
    const auto bits=t.tag(r,258);
    require(bits.count==(rgb?3u:1u),"Invalid bits per sample");
    for(size_t i=0;i<bits.count;i++) require(t.num(bits,i)==14,"Expected 14-bit RAW");
    require(t.tag(r,273).count==1 && t.tag(r,279).count==1,"Expected one RAW strip");
    return {int(w),int(h),t.get(field(273),field(279)),field(279)};
}
}

RawPlane read_nef(const std::filesystem::path& path){
    tiff::Mapping mapping(path); auto t=mapping.view(); auto main=t.root();
    auto r=t.rawdir(main); auto s=strip(t,r,false);
    auto cfa=t.tag(r,33422); require(cfa.size==4 && !std::memcmp(t.bytes+cfa.data,"\0\1\1\2",4),"此 RAW 色彩排列暂不支持（需要 RGGB）");
    auto note=t.maker(main); const auto meta=t.tag(note.fields,0x96);
    require(meta.size>=10 && t.bytes[meta.data]==0x46 && t.bytes[meta.data+1]==0x30,
        "仅支持尼康 14 位无损压缩 NEF，不支持高效率压缩");
    int vertical[2][2];
    for(int k=0;k<4;k++) vertical[k/2][k%2]=u16(t.bytes+meta.data+2+2*k);
    RawPlane result{s.w,s.h,std::vector<uint16_t>(size_t(s.w)*s.h)};
    const size_t totalBits=s.length*8;
    size_t bit=0;
    for(int y=0;y<s.h;y++){
        int hor[2]={vertical[y&1][0],vertical[y&1][1]};
        for(int x=0;x<s.w;x++){
            // An unaligned big-endian window holds a whole prefix and payload.
            const size_t byte=bit>>3;
            uint32_t window=0;
            if(byte+4<=s.length) {
                std::memcpy(&window,s.data+byte,4);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
                window=__builtin_bswap32(window);
#endif
            } else {
                require(byte<s.length,"NEF 数据提前结束");
                for(size_t i=0;i<s.length-byte;i++) window|=uint32_t(s.data[byte+i])<<(24-8*i);
            }
            window<<=(bit&7);
            const Prefix p=prefixes[window>>24];
            require(p.prefixBits!=0,"Invalid NEF prefix");
            bit+=p.prefixBits+p.valueBits;
            const int n=p.valueBits;
            const int magnitude=n ? int((window<<p.prefixBits)>>(32-n)) : 0;
            const int delta=n && (magnitude>>(n-1))==0 ? magnitude-((1<<n)-1) : magnitude;
            const int value=hor[x&1]+delta;
            require(unsigned(value)<16384,"NEF 数据损坏或压缩方式不受支持");
            hor[x&1]=value; if(x<2) vertical[y&1][x]=value;
            result.pixels[size_t(y)*s.w+x]=uint16_t(value);
        }
    }
    require(bit<=totalBits,"NEF 数据不完整");
    return result;
}
RGBImage read_nefx(const std::filesystem::path& path){
    tiff::Mapping mapping(path); auto t=mapping.view();
    const auto s=strip(t,t.rawdir(t.root()),true);
    const size_t count=size_t(s.w)*s.h*3;
    require(s.length==count*14/8,"Invalid NEFX strip size");
    RGBImage out{s.w,s.h,std::vector<uint16_t>(count)};
    for(size_t i=0;i<count;i+=4){
        const uint8_t* p=s.data+i/4*7;
        out.pixels[i]=u16(p)&0x3fff;
        out.pixels[i+1]=(uint32_t(u16(p+1))|uint32_t(p[3])<<16)>>6&0x3fff;
        out.pixels[i+2]=(uint32_t(u16(p+3))|uint32_t(p[5])<<16)>>4&0x3fff;
        out.pixels[i+3]=u16(p+5)>>2;
    }
    return out;
}
}
