// Optional independent cross-check; not a runtime dependency. Build with
// c++ -O2 -std=c++17 -Isrc tests/libraw_check.cpp src/raw.cpp $(pkg-config --cflags --libs libraw) -o build/libraw-check
// Use LibRaw >= 0.22 with one or more Z 8 NEFs and/or exported NEFX files.
#include "raw.hpp"
#include <libraw/libraw.h>
#include <iostream>
#include <stdexcept>

static size_t compare(const char* path) {
    LibRaw reference;
    reference.imgdata.rawparams.max_raw_memory_mb=4096;
    auto status=reference.open_file(path);
    if(status==LIBRAW_SUCCESS) status=reference.unpack();
    if(status!=LIBRAW_SUCCESS) throw std::runtime_error(libraw_strerror(status));
    const auto& r=reference.imgdata;
    const int w=r.sizes.raw_width,h=r.sizes.raw_height;
    size_t differences=0;
    if(r.idata.filters) {
        auto ours=npm::read_nef(path);
        if(ours.width!=w || ours.height!=h || !r.rawdata.raw_image)
            throw std::runtime_error("LibRaw disagrees on NEF layout");
        auto stride=r.sizes.raw_pitch/sizeof(uint16_t);
        for(int y=0;y<h;y++) for(int x=0;x<w;x++)
            differences+=ours.pixels[size_t(y)*w+x]!=r.rawdata.raw_image[y*stride+x];
    } else {
        auto ours=npm::read_nefx(path);
        if(ours.width!=w || ours.height!=h || !r.rawdata.color4_image)
            throw std::runtime_error("LibRaw disagrees on NEFX layout");
        auto stride=r.sizes.raw_pitch/(4*sizeof(uint16_t));
        for(int y=0;y<h;y++) for(int x=0;x<w;x++) for(int c=0;c<3;c++)
            differences+=ours.pixels[(size_t(y)*w+x)*3+c]!=r.rawdata.color4_image[y*stride+x][c];
    }
    std::cout<<path<<": "<<w<<"×"<<h<<", "<<differences<<" samples differ\n";
    return differences;
}
int main(int argc,char** argv) {
    if(argc<2) { std::cerr<<"Usage: libraw-check <camera.NEF|merged.NEFX> ...\n";return 2; }
    try {
        bool ok=true; for(int i=1;i<argc;i++) if(compare(argv[i]))ok=false;
        return ok?0:1;
    }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
