#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace npm {
// Correct a blue photosite on a Z 8 phase-detection row using the matching
// half-pixel phase from the neighboring ordinary row. Red supplies the local
// brightness ratio; blue/green gradients choose how much of it to apply.
// Green here is still the unrounded sum of the two sampled green phases.
inline uint16_t pdaf_blue(const uint16_t* site, const uint16_t* neighbor) {
    const unsigned R=site[0],G=site[1],B=site[2];
    const unsigned r=neighbor[0],g=neighbor[1],b=neighbor[2];
    if(!R || !G || !B || !r)return uint16_t(B);
    // Ratios p=b/B, q=r/R, s=g/G choose the reconstruction. Compare their
    // differences in tenths by cross multiplication: exactly 0.1 is a real
    // boundary for integer sensor codes and must not drift across branches.
    struct Difference { int64_t ten_num, den; };
    const auto diff=[](int64_t a,int64_t x,int64_t b,int64_t y) -> Difference {
        return {10*(a*y-b*x),x*y};
    };
    const auto ps=diff(b,B,g,G), pq=diff(b,B,r,R), qs=diff(r,R,g,G);
    // Reconstruction uses a single-precision red ratio followed by a
    // double-precision division and truncation to the sensor code.
    const double estimate=b/double(float(r)/R);
    enum { keep,blend,replace } op=keep;
    if(std::abs(ps.ten_num)<=ps.den) {
        if ((ps.ten_num>0 && pq.ten_num<pq.den) ||
            (ps.ten_num<=0 && pq.ten_num>=0 && pq.ten_num<=pq.den && qs.ten_num < -qs.den))op=blend;
    } else if(ps.ten_num>0) {
        if(B<R) {
            if(pq.ten_num<3*pq.den)op=blend;
            else {
                // On a strong blue edge, reject an implausible estimate in
                // dark or strongly red detail. G is the sum of two samples.
                const double green=G*.5;
                if(green>2000 && R-B<.4*green &&
                   (green-estimate>500 || estimate-B<500))op=replace;
            }
        }
    } else if(pq.ten_num<0 && pq.ten_num>=-pq.den) {
        op=replace;
    } else if(pq.ten_num < -pq.den && qs.ten_num>=-qs.den) {
        if(10*r>=11*R) op=blend;
        else if(std::abs(qs.ten_num)<=qs.den && std::abs(R-estimate)<=300.) op=replace;
    }
    if(op==keep)return uint16_t(B);
    return uint16_t(std::min(16383.,op==replace?estimate:(B+estimate)*.5));
}
// Neighbor rows are never PDAF rows, so this pass can operate in place.
inline void correct_pdaf(std::vector<uint16_t>& rgb,int w,int h,int scale,int raw_top) {
    for(int y=0;y<h;y++) {
        const int row=y/scale+raw_top;
        // The two full-color rows that use a given PDAF row of the RGGB
        // input are adjacent before the pixel-shift output is interleaved.
        const int badrow=(row&1)?row:row-1;
        if(badrow<29 || badrow>5489 || badrow%12!=5)continue;
        const int yn=y+((row&1)?-scale:scale);
        if(yn<0 || yn>=h)continue;
        uint16_t* dest=rgb.data()+size_t(y)*w*3;
        const uint16_t* ref=rgb.data()+size_t(yn)*w*3;
        for(int x=0;x<w;x++) dest[3*x+2]=pdaf_blue(dest+3*x,ref+3*x);
    }
}
}
