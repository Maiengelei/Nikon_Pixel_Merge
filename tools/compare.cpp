// Compare actual sensor-space samples, bypassing embedded previews and display transforms.
#include "raw_tiff.hpp"
#include <array>
#include <cmath>
#include <iomanip>
#include <iostream>

namespace {
using namespace npm::tiff;
struct Plane {
    Mapping m;
    int w,h;
    const uint8_t* ptr;
    Plane(const char* path): m(path) {
        auto v=m.view(); auto d=v.rawdir(v.root());
        auto n=[&](int t){return v.num(v.tag(d,t));};
        w=n(256); h=n(257);
        auto b=v.tag(d,258);
        require(w>0 && h>0 && w<=32768 && h<=32768 && w%4==0 && b.count==3 &&
            v.num(b)==14 && v.num(b,1)==14 && v.num(b,2)==14 && n(277)==3 &&
            n(262)==2 && n(284)==1 && v.tag(d,273).count==1 &&
            v.tag(d,279).count==1 && n(279)==size_t(w)*h*3*14/8,"Expected packed 14-bit RGB NEFX");
        ptr=v.get(n(273),n(279));
    }
};
struct Stats {
    uint64_t count=0, total=0, squares=0;
    std::array<uint64_t,16384> hist{};
    int max=0;
    void add(int delta) {
        auto d=std::abs(delta); ++count; total+=d; squares+=d*d;
        ++hist[d]; if(d>max) max=d;
    }
    int percentile(double f) const {
        uint64_t sum=0; for(int i=0;i<=max;++i) { sum+=hist[i]; if(sum>=count*f) return i; } return max;
    }
};
uint64_t block(const uint8_t* p) {
    return uint64_t(p[0]) | uint64_t(p[1])<<8 | uint64_t(p[2])<<16 | uint64_t(p[3])<<24 |
           uint64_t(p[4])<<32 | uint64_t(p[5])<<40 | uint64_t(p[6])<<48;
}
}
int main(int argc,char**argv) {
    if(argc!=3) { std::cerr<<"compare OUTPUT.nefx REFERENCE.nefx\n"; return 2; }
    try {
        Plane a(argv[1]),b(argv[2]);
        require(a.w==b.w && a.h==b.h,"Resolution mismatch");
        std::array<Stats,3> s;
        auto p=a.ptr,q=b.ptr;
        for(size_t i=0;i<size_t(a.w)*a.h*3;i+=4,p+=7,q+=7) {
            auto x=block(p),y=block(q);
            for(int k=0;k<4;k++,x>>=14,y>>=14) s[(i+k)%3].add(int(x&16383)-int(y&16383));
        }
        std::cout<<"width,height,channel,mae,rmse,p99,p999,max,exact_pct,within_1_pct,different,more_than_1\n";
        for(int c=0;c<3;c++){
            const Stats& t=s[c];
            std::cout<<a.w<<','<<a.h<<','<<"RGB"[c]<<','<<std::fixed<<std::setprecision(5)
                     <<double(t.total)/t.count<<','<<std::sqrt(double(t.squares)/t.count)<<','
                     <<t.percentile(.99)<<','<<t.percentile(.999)<<','<<t.max<<','
                     <<100.*t.hist[0]/t.count<<','<<100.*(t.hist[0]+t.hist[1])/t.count<<','<<t.count-t.hist[0]<<','<<t.count-t.hist[0]-t.hist[1]<<'\n';
        }
    } catch(const std::exception&e) {std::cerr<<e.what()<<'\n'; return 1;}
}
