// A known scene encoded as separate RGGB exposures. The half-grid contains
// detail that a resize of a single frame cannot reconstruct.
#include "merge.hpp"
#include "pdaf.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

namespace {
int signal(int x,int y,int c) {
    return 1800+(x*17+y*21+c*97)+(x&1)*111+(y&1)*213;
}
void verify(int n,bool lens_on) {
    const npm::MergeGeometry geo{30,24,4,4,22,16};
    const int scale=n>=16?2:1, cycle=scale==2?16:4;
    const bool repeated=n==8 || n==32;
    // Position of the scene relative to the first exposure (half-pixel units).
    const std::array<std::array<int,2>,16> path{{
        {0,0},{2,0},{2,2},{0,2}, {1,0},{3,0},{3,2},{1,2},
        {1,1},{3,1},{3,3},{1,3}, {0,1},{2,1},{2,3},{0,3}
    }};
    int read=0,rows=0;
    npm::CAProfile zero{};
    npm::merge(n,geo,lens_on?&zero:nullptr,[&](int i){
        if(i!=read++) throw std::runtime_error("frame request order");
        std::vector<uint16_t> plane(geo.raw_width*geo.raw_height);
        int sx=path[i%cycle][0],sy=path[i%cycle][1]+4*(i/cycle);
        for(int y=0;y<geo.raw_height;++y) for(int x=0;x<geo.raw_width;++x){
            int c=(x%2)+(y%2);
            plane[y*geo.raw_width+x]=signal(2*(x-geo.left)+sx,2*(y-geo.top)+sy,c)+2*(i/cycle);
        }
        return plane;
    },[&](int y,const uint16_t* rgb,int w){
        if(y!=rows++ || w!=geo.width*scale) throw std::runtime_error("output geometry");
        for(int x=0;x<w;++x) for(int c=0;c<3;++c)
            if(rgb[3*x+c]!=signal(2*x/scale,2*y/scale,c)+int(repeated))
                throw std::runtime_error("unexpected RGB value for "+std::to_string(n)+" frames");
    });
    if(rows!=geo.height*scale || read!=n) throw std::runtime_error("incomplete raster");
}

void verify_highlight_rounding() {
    // Small white field with the lens coefficients of the four-frame Z 8 set.
    // Even a constant neighborhood can fall just below full scale after CA.
    const npm::MergeGeometry geo{30,24,4,4,22,16};
    constexpr double unit=1.0/1048576;
    const npm::CAProfile profile{{620*unit,-905*unit,0,258*unit},
                                 {-348*unit,618*unit,0,-130*unit},11./256,-14./256};
    constexpr std::array<uint16_t,12> expected{{
        16383,16383,16383, 16383,16383,16383,
        16382,16383,16382, 16382,16383,16383
    }};
    bool checked=false;
    npm::merge(4,geo,&profile,[&](int) {
        return std::vector<uint16_t>(geo.raw_width*geo.raw_height,16383);
    },[&](int y,const uint16_t* rgb,int width) {
        if(y==7) checked=width>=11 && std::equal(expected.begin(),expected.end(),rgb+7*3);
    });
    if(!checked) throw std::runtime_error("CA highlight rounding changed");
}
}
int main() {
    try {
        for(int n:{4,8,16,32}) for(bool ca:{false,true}) verify(n,ca);
        verify_highlight_rounding();
        // Verified Z 8 photosite neighborhoods at exact 0.1 threshold ties.
        // A double subtraction instead of a rational comparison changes the
        // chosen sample by tens of sensor codes in these real cases.
        struct Case { uint16_t site[3], neighbor[3], expected; };
        for (const Case& v : std::array<Case,3>{{
            {{1710,3320,1245},{1743,3500,1188},1245},
            {{2166,3905,1330},{1938,4058,1323},1404},
            {{1960,3637,1260},{1834,3651,1305},1260}
        }}) if(npm::pdaf_blue(v.site,v.neighbor)!=v.expected)
            throw std::runtime_error("PDAF boundary changed");
        std::cout<<"Known-scene merge tests passed\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
