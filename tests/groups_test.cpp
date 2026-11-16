// Standalone C++17 test. Optional first argument: local directory of NEF samples.
#include "groups.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>

namespace fs = std::filesystem;
using Bytes = std::vector<unsigned char>;

void require(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}

// Tiny TIFFs containing only the Nikon shift, burst, exposure and body tags.
// Make a big-endian MakerNote inside a little-endian NEF for one camera.
struct Header {
    Bytes data = Bytes(2048);
    bool big = false;
    unsigned next = 300;
    int fields = 0;
    explicit Header(bool big) : big(big) {
        data[0]=data[1]=big ? 'M':'I'; u16(2,42); u32(4,8);
    }
    void u16(unsigned o,unsigned n) { data[o+big]=n&255; data[o+!big]=(n>>8)&255; }
    void u32(unsigned o,unsigned n) {
        u16(o+2*big,n); u16(o+2*!big,n>>16);
    }
    void field(unsigned id,unsigned type,unsigned n,Bytes b) {
        unsigned off=10+12*fields++;
        u16(8,fields); u16(off,id); u16(off+2,type); u32(off+4,n);
        unsigned v=off+8;
        if (b.size()>4) {v=next; u32(off+8,v); next+=static_cast<unsigned>(b.size());}
        data.resize(std::max<std::size_t>(data.size(),v+b.size()));
        std::copy(b.begin(),b.end(),data.begin()+v);
    }
    Bytes number(unsigned n) const {
        Bytes b(4);
        for(int i=0;i<4;++i) b[big ? 3-i : i]=static_cast<unsigned char>(n>>(i*8));
        return b;
    }
    void ascii(unsigned id,std::string s) {
        s.push_back(0); field(id,2,static_cast<unsigned>(s.size()),Bytes(s.begin(),s.end()));
    }
    void numeric(unsigned id,unsigned n) {field(id,4,1,number(n));}
    Bytes block(std::initializer_list<unsigned> nums) const {
        Bytes b{'0','1','0','0'};
        for(auto num:nums) {auto bytes=number(num); b.insert(b.end(),bytes.begin(),bytes.end());}
        return b;
    }
};

void fixture(const fs::path& p,int mode,int index,unsigned burst=99,std::string serial="a",
             bool big=false,unsigned iso=64) {
    Header note(big);
    note.ascii(0x1d,serial);
    note.numeric(0xa7,1499+index);
    if(mode>0) {
        auto shifts=note.block({0xfffffff0u,256,24,256});
        note.field(0x54,7,static_cast<unsigned>(shifts.size()),shifts);
        auto start=note.block({burst,static_cast<unsigned>(index),1});
        note.field(0x56,7,static_cast<unsigned>(start.size()),start);
        Bytes desc{'0','1','0','0',static_cast<unsigned char>(mode),
            static_cast<unsigned char>(mode==2&&index>4?index+12:index)};
        note.field(0x5e,7,static_cast<unsigned>(desc.size()),desc);
    }
    Bytes nikon{'N','i','k','o','n',0,2,0x11,0,0};
    nikon.insert(nikon.end(),note.data.begin(),note.data.begin()+note.next);

    Header root(false);
    root.ascii(0x10f,"NIKON CORPORATION");root.ascii(0x110,"NIKON Z 8");
    root.numeric(0x8769,96);
    root.u16(96,2);
    root.u16(98,0x927c);root.u16(100,7);root.u32(102,static_cast<unsigned>(nikon.size()));root.u32(106,512);
    root.u16(110,0x8827);root.u16(112,3);root.u32(114,1);root.u16(118,iso);
    std::copy(nikon.begin(),nikon.end(),root.data.begin()+512);
    std::ofstream f(p,std::ios::binary);
    f.write(reinterpret_cast<const char*>(root.data.data()),512+static_cast<std::streamsize>(nikon.size()));
}

struct TempDir {
    fs::path path;
    TempDir() {
        path=fs::temp_directory_path()/("npm-groups-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if(!fs::create_directory(path)) throw std::runtime_error("Cannot create fixture folder");
    }
    ~TempDir() {std::error_code e;fs::remove_all(path,e);}
};

int main(int argc,char**argv) {
 try {
    TempDir temp;
    fs::path d=temp.path/"mixed", sub=d/"nested";
    fs::create_directories(sub);
    // Two bodies with identical burst and shutter counters. Shuffled names,
    // varying case, and two directories may not change group membership.
    for(int i=1;i<=4;++i) fixture(d/("photo-"+std::to_string(9-i)+".nEf"),1,i);
    for(int i=1;i<=8;++i) fixture((i%2?d:sub)/("other-"+std::to_string(100-i)+".NEF"),2,i,99,"b",true);
    fixture(d/"ordinary.NEF",0,1);
    std::ofstream(d/"corrupt.nef") << "not a TIFF";
    std::ofstream(d/"reference.NEFX") << "ignore";
    auto scan=npm::scan_groups({d,sub,d/"photo-8.nEf"});
    require(scan.groups.size()==2 && scan.ordinary_nefs==1 && scan.issues.size()==1,"mixed scans");
    std::map<int,std::string> ids;
    for(auto& g:scan.groups) {
        require(g.complete(),"complete mixed group");
        for (int i=0;i<g.expected;++i) {
            auto& info=g.frames.at(i);
            require(info.index==i+1 && info.shift_x==-16./256 && info.shift_y==24./256,"frame order/shift");
        }
        ids[g.expected]=g.id;
    }
    fs::rename(d/"photo-8.nEf",d/"name-does-not-matter.NEF");
    fs::remove(d/"photo-7.nEf");
    fs::copy_file(d/"photo-6.nEf",d/"frame-copy.nef");
    auto invalid=npm::scan_groups({d});
    for(auto& g:invalid.groups) {
        require(g.id==ids.at(g.expected),"rename changed group ID");
        if(g.expected==4) require(!g.complete() && g.missing==std::vector<int>{2}
            && g.duplicates==std::vector<int>{3},"missing and duplicate cancel out in count");
        else require(g.complete(),"unaffected mixed group");
    }
    fs::remove(d/"frame-copy.nef");
    fixture(d/"frame-2.nef",1,2,99,"a",false,800);
    auto inconsistent=npm::scan_groups({d});
    for(auto& g:inconsistent.groups) if(g.expected==4)
        require(!g.complete() && !g.problems.empty(),"mismatched ISO");
    // Broken file reports an issue without being absorbed into the other groups.
    fs::resize_file(d/"frame-2.nef",140);
    auto truncated=npm::scan_groups({d});
    require(truncated.issues.size()==2,"truncated MakerNote should be diagnosed");
    if(argc>1) {
        auto real=npm::scan_groups({argv[1]});
        require(real.issues.empty() && real.groups.size()==4,"real capture group count");
        std::map<int,int> modes;
        for(auto& g:real.groups) {
            require(g.complete() && !g.frames.front().lens.empty(),"real capture complete and lens known");
            ++modes[g.expected];
        }
        require(modes==std::map<int,int>{{4,1},{8,1},{16,1},{32,1}},"all real capture modes");
    }
    std::cout<<"Group recognition tests passed\n";
    return 0;
 } catch (const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;}
}
