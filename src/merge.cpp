#include "merge.hpp"
#include "pdaf.hpp"
#include "raw_tiff.hpp"
#include <algorithm>
#include <cmath>
#include <thread>

namespace npm {
CAProfile read_ca_profile(const std::filesystem::path& nef) {
    tiff::Mapping file(nef);
    const auto t=file.view(); const auto raw=t.rawdir(t.root());
    const auto lens=t.nikon_block(t.tag(raw,0xc7d5));
    const auto& tag=t.tag(lens.fields,7);
    const uint8_t* p=t.get(tag.data,tag.size);
    tiff::require(tag.size>=88 && !std::memcmp(p,"0100",4) && p[4]==1 && p[5]==2 &&
                  tiff::u32(p+16)==3 && tiff::u32(p+52)==3,
                  "Unsupported or missing lateral chromatic aberration profile");
    CAProfile profile;
    for(int c=0;c<2;++c) {
        auto& coeff=c ? profile.blue : profile.red;
        for(int i=0;i<4;++i) {
            const auto* k=p+(c?56:20)+i*8;
            const int den=static_cast<int32_t>(tiff::u32(k+4));
            tiff::require(den>0,"Invalid chromatic aberration profile divisor");
            coeff[i]=static_cast<int32_t>(tiff::u32(k))/double(den);
            tiff::require(std::abs(coeff[i])<=0.05,"Lens correction outside supported range");
        }
    }
    // The optical centre follows the measured position of the first
    // exposure. Coordinates in this Nikon block are in native sensor pixels.
    const auto& motion=t.tag(lens.fields,8);
    const auto* m=t.get(motion.data,motion.size);
    tiff::require(motion.size>=20 && !std::memcmp(m,"0100",4),
                  "Unsupported sensor shift metadata");
    auto shift=[&](int pos) {
        const int den=static_cast<int32_t>(tiff::u32(m+pos+4));
        tiff::require(den>0,"Invalid sensor shift divisor");
        double value=static_cast<int32_t>(tiff::u32(m+pos))/double(den);
        tiff::require(std::abs(value)<4,"Invalid first exposure sensor shift");
        return value;
    };
    profile.sensor_x=shift(4);profile.sensor_y=shift(12);
    return profile;
}
namespace {
template<class F> void parallel_rows(int count,const F& run) {
    const int n=std::max(1,std::min({8,int(std::thread::hardware_concurrency()),(count+7)/8}));
    std::vector<std::thread> threads;
    try {
        for(int i=1;i<n;i++)
            threads.emplace_back([&,i]{run(count*i/n,count*(i+1)/n);});
        run(0,count/n);
    } catch(...) {
        for(auto& t:threads)t.join();
        throw;
    }
    for(auto& t:threads)t.join();
}

// Nikon Z 8 sensor: sampled PDAF rows between the top/bottom margins. For
// repeated exposures this site always has an ordinary blue sample two rows away.
bool pdaf_row(int y) { return y>=29 && y<=5489 && y%12==5; }

// The two polynomials are evaluated on the distance to each photosite.
// Keep the cube rounded independently: a one-ulp change before truncation
// can change an otherwise constant 14-bit highlight by one code value.
inline double cubic_weight(double distance) {
    const double square=distance*distance;
    const double cube=square*distance;
    if(distance<=1)
        return std::fma(-2.0,square,1.0)+cube;
    return std::fma(5.0,square,std::fma(-8.0,distance,4.0))-cube;
}

inline std::array<double,4> weights(double fraction) {
    return {cubic_weight(1+fraction),cubic_weight(fraction),
            cubic_weight(1-fraction),cubic_weight(2-fraction)};
}

inline uint16_t sample(const std::vector<uint16_t>& rgb,int w,int h,int c,
                       double x,double y) {
    const int ix=int(std::floor(x)),iy=int(std::floor(y));
    const auto ax=weights(x-ix), ay=weights(y-iy);
    double value=0;
    for(int j=0;j<4;++j) {
        const auto* row=rgb.data()+std::clamp(iy+j-1,0,h-1)*size_t(w)*3;
        for(int i=0;i<4;++i) {
            const double weight=ax[i]*ay[j];
            const auto site=row[3*std::clamp(ix+i-1,0,w-1)+c];
            value=std::fma(weight,double(site),value);
        }
    }
    return uint16_t(std::clamp(value,0.,16383.));
}
}
void merge(int n,MergeGeometry g,const CAProfile* correction,
           std::function<std::vector<uint16_t>(int)> read_raw,
           std::function<void(int,const uint16_t*,int)> write_row) {
    tiff::require(n==4||n==8||n==16||n==32,"Unsupported pixel shift frame count");
    const bool repeat=n==8||n==32;
    const int scale=n>=16?2:1, cycle=4*scale*scale;
    tiff::require(g.left>=1 && g.top>=(repeat?3:1) && g.width>0 && g.height>0 &&
        g.raw_width>0 && g.raw_width<=12000 && g.raw_height>0 && g.raw_height<=9000 &&
        g.left<=g.raw_width-g.width && g.top<=g.raw_height-g.height,
        "Invalid active sensor area");
    const int out_width=g.width*scale, out_height=g.height*scale;
    int offset_x=0,offset_y=0;
    if(correction) {
        // A lens warp near the active crop can require real photosites just
        // outside it. Reconstruct the common sensor area before cropping.
        const int pad_left=g.left-1, pad_top=g.top-(repeat?3:1);
        offset_x=pad_left*scale;offset_y=pad_top*scale;
        g.left-=pad_left;g.top-=pad_top;
        g.width=g.raw_width-g.left;g.height=g.raw_height-g.top;
    }
    const int w=g.width*scale,h=g.height*scale;
    std::vector<uint16_t> rgb(size_t(w)*h*3);
    // The four quadrants repeat in the same order on both the integer and
    // half-pixel grids. A repeated exposure starts two sensor rows higher.
    constexpr int xs[]={0,1,1,0},ys[]={0,0,1,1};
    for(int f=0;f<n;++f) {
        const auto raw=read_raw(f);
        tiff::require(raw.size()==size_t(g.raw_width)*g.raw_height,"Unexpected sensor dimensions");
        const int sx=g.left-xs[f%4], sy=g.top-ys[f%4]-2*(f/cycle);
        const int px=scale==1?0:xs[(f/4)%4], py=scale==1?0:ys[(f/4)%4];
        const bool second=f>=cycle;
        parallel_rows(g.height,[&](int first,int last) {
            for(int y=first;y<last;y++) {
                const int ry=sy+y, green_bit=14+(ry&1);
                const bool bad_first=pdaf_row(ry+2),bad_second=pdaf_row(ry);
                const auto* in=raw.data()+size_t(ry)*g.raw_width+sx;
                auto* dst=rgb.data()+(size_t(scale*y+py)*w+px)*3;
                for(int x=0;x<g.width;x++,dst+=3*scale) {
                    const int c=(ry&1)+((sx+x)&1);
                    const uint16_t v=in[x];
                    if(c==0) {
                        const auto r=second?((dst[0]&16383)+v)/2:v;
                        dst[0]=(dst[0]&0xc000)|r;
                    } else if(c==1) {
                        // Keep the low bit for each green phase in the two
                        // spare bits of red. This preserves the reference
                        // order: floor each repeat pair, then average greens.
                        if(!second && repeat) dst[0]|=(v&1)<<green_bit;
                        dst[1]+=v-(second?((v^(dst[0]>>green_bit))&1):0);
                    } else if(!second || bad_first) dst[2]=v;
                    else if(!bad_second) dst[2]=(dst[2]+v)/2;
                }
            }
        });
    }
    if(!repeat) correct_pdaf(rgb,w,h,scale,g.top);
    parallel_rows(h,[&](int first,int last){
        for(size_t i=size_t(first)*w*3;i<size_t(last)*w*3;i+=3) {
            rgb[i]&=16383;rgb[i+1]>>=repeat?2:1;
        }
    });
    if(!correction) {
        for(int y=0;y<h;y++) write_row(y,rgb.data()+size_t(y)*w*3,w);
        return;
    }
    // The lens polynomial uses the entire sensor diagonal, including the
    // masked margins. The optical centre is expressed on the native grid.
    const double cx=((g.raw_width-1)*0.5-g.left+correction->sensor_x)*scale;
    const double cy=((g.raw_height-1)*0.5-g.top+correction->sensor_y)*scale;
    const double sensor_radius=0.5*scale*std::hypot(g.raw_width,g.raw_height);
    constexpr int band=64;
    std::vector<uint16_t> output(size_t(out_width)*3*band);
    for(int start=0;start<out_height;start+=band) {
        const int rows=std::min(out_height-start,band);
        parallel_rows(rows,[&](int first,int last){
            for(int j=first;j<last;j++) {
                const int y=j+start+offset_y;const double v=y-cy;
                auto* out=output.data()+size_t(j)*out_width*3;
                for(int x=offset_x;x<offset_x+out_width;x++,out+=3) {
                    const double u=x-cx;
                    const double r=std::hypot(u,v)/sensor_radius;
                    const double r2=r*r, r3=r2*r;
                    out[1]=rgb[3*(size_t(y)*w+x)+1];
                    for(int c:{0,2}) {
                        const auto& k=c==0?correction->red:correction->blue;
                        // Rounding the powers first and applying the scale about
                        // the optical centre preserves integer-boundary samples.
                        const double delta=std::fma(k[0],r3,
                            std::fma(k[1],r2,std::fma(k[2],r,k[3])));
                        const double magnification=1.0+delta;
                        const double source_x=std::fma(u,magnification,cx);
                        const double source_y=std::fma(v,magnification,cy);
                        out[c]=sample(rgb,w,h,c,source_x,source_y);
                    }
                }
            }
        });
        for(int j=0;j<rows;j++)write_row(start+j,output.data()+size_t(j)*out_width*3,out_width);
    }
}
}
