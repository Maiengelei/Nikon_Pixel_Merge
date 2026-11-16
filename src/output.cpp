#include "output.hpp"
#include "raw_tiff.hpp"
#include <array>
#include <cstdio>
#include <limits>

namespace npm {
namespace {
using namespace tiff;
using Bytes=std::vector<uint8_t>;
void put16(Bytes& v,size_t pos,uint16_t n){v.at(pos)=uint8_t(n);v.at(pos+1)=uint8_t(n>>8);}
void put32(Bytes& v,size_t pos,uint32_t n){put16(v,pos,uint16_t(n));put16(v,pos+2,uint16_t(n>>16));}
void align16(Bytes& v){v.resize((v.size()+15)&~size_t(15));}
uint32_t append(Bytes& v,const uint8_t* p,size_t n){
    align16(v); uint32_t off=uint32_t(v.size()); v.insert(v.end(),p,p+n);return off;
}
void crc(Bytes& prefix,size_t pos,size_t length){
    uint16_t val=0xffff;
    for(size_t i=pos;i<pos+length-2;i++){
        val^=uint16_t(prefix.at(i))<<8;
        for(int b=0;b<8;b++) val=(val&0x8000)?uint16_t((val<<1)^0x1021):uint16_t(val<<1);
    }
    put16(prefix,pos+length-2,val);
}
void set_crop(Bytes& v,size_t p,int w,int h){
    put16(v,p,0); put16(v,p+2,0); put16(v,p+4,uint16_t(w)); put16(v,p+6,uint16_t(h));
}
void scale_vr(Bytes& v,size_t p){
    // The two signed rational numerators are expressed in output pixels.
    for(int offset:{4,12}) {
        int64_t n=static_cast<int32_t>(u32(v.data()+p+offset));
        require(n*2>=INT32_MIN && n*2<=INT32_MAX,"Out of range camera adjustment");
        put32(v,p+offset,uint32_t(n*2));
    }
}
using Entry=std::array<uint8_t,12>;
Entry copied(View t,Tag tag){Entry e{};std::memcpy(e.data(),t.get(tag.entry,12),12);return e;}
Entry blob(uint16_t id,uint32_t pos,uint32_t length){
    Bytes field(12);put16(field,0,id);put16(field,2,7);put32(field,4,length);put32(field,8,pos);
    Entry e;std::copy(field.begin(),field.end(),e.begin());return e;
}
void directory(Bytes& a,size_t pos,std::vector<Entry> entries){
    std::sort(entries.begin(),entries.end(),[](auto& l,auto& r){return u16(l.data())<u16(r.data());});
    require(pos+6+12*entries.size()<=a.size(),"Invalid output directory placement");
    put16(a,pos,uint16_t(entries.size())); size_t cur=pos+2;
    for(auto& e:entries){std::copy(e.begin(),e.end(),a.begin()+cur);cur+=12;}
    put32(a,cur,0);
}
constexpr int pw=640,ph=424;
struct Header{Bytes bytes;size_t preview;};
Header header(const std::filesystem::path& source,int w,int h,bool ca,unsigned n){
    require(n==4 || n==8 || n==16 || n==32,"Unsupported frame count");
    int scale=n>=16?2:1;
    require(w==8256*scale && h==5504*scale,"Unsupported NEFX output size");
    Mapping m(source); auto t=m.view(); auto main=t.root();auto r=t.rawdir(main);
    auto maker=t.maker(main); auto sub=t.nikon_block(t.tag(r,0xc7d5));
    auto raw=t.num(t.tag(r,273)); require(raw<64u*1024*1024,"Unexpectedly large camera header");
    t.get(0,raw);
    // Preserve capture metadata and the camera's JPEG previews; only the area
    // before the first sensor byte is copied. Reference NEFX files are never inputs.
    Bytes out(t.bytes,t.bytes+raw);
    auto setRaw=[&](uint16_t id,int value){const Tag tag=t.tag(r,id);require(tag.count==1,"Unexpected RAW field count");
        if(tag.type==3) put16(out,tag.data,uint16_t(value));
        else {require(tag.type==4,"Unexpected RAW field type");put32(out,tag.data,uint32_t(value));}};
    setRaw(256,w);setRaw(257,h);setRaw(262,2);setRaw(277,3);setRaw(278,h);
    setRaw(279,uint32_t(size_t(w)*h*3*14/8));
    const uint8_t bits[]={14,0,14,0,14,0};
    const size_t bitEntry=t.tag(r,258).entry;
    auto bpos=append(out,bits,sizeof(bits));put32(out,bitEntry+4,3);put32(out,bitEntry+8,bpos);

    // Nikon RAW parameters describe the merged RGB raster, not the original
    // CFA lens-correction input. Recalculate checksums of modified subrecords.
    std::vector<Entry> subfields;
    for (auto& f:sub.fields) {
        if(f.id==7 || f.id==15) continue;
        subfields.push_back(copied(t,f));
        if(f.id==11){require(f.size==14,"Invalid RAW crop");set_crop(out,f.data+4,w,h);crc(out,f.data,f.size);}
        if(f.id==13){require(f.size==8,"Invalid RAW format");put16(out,f.data+4,10);crc(out,f.data,f.size);}
        if(f.id==8 && scale==2){require(f.size==24,"Invalid RAW adjustment");scale_vr(out,f.data);crc(out,f.data,f.size);}
    }
    directory(out,sub.base+u32(t.bytes+sub.base+4),std::move(subfields));

    // Keep the MakerNote's original internal origin and relative preview IFD
    // offsets. Two input-only entries are replaced by the merge record and
    // the linear RGB preview. Relocation is transparent to TIFF readers.
    auto block=maker.block;
    Bytes note(t.bytes+block.data,t.bytes+block.data+block.size);
    std::vector<Entry> fields;
    for(auto& f:maker.fields){
        if(f.id==0x58 || f.id==0x96)continue;
        fields.push_back(copied(t,f));size_t p=f.data-block.data;
        if(f.id==0x45){require(f.size==8,"Invalid camera crop");set_crop(note,p,w,h);}
        if(f.id==0x51){require(f.size>=12,"Invalid camera RAW format");put16(note,p+10,10);}
        if(f.id==0x54 && scale==2){require(f.size==20,"Invalid camera shift");scale_vr(note,p);}
        if(f.id==0xb7 && scale==2){require(f.size==80,"Unsupported focus metadata");std::fill(note.begin()+p+62,note.begin()+p+80,0);}
    }
    const uint8_t code=n==4?1:n==8?2:n==16?3:4;
    const uint8_t mergeInfo[]={'0','1','0','0',2,0,code,code,0,0,0,0,0,0,0,0};
    auto rel=append(note,mergeInfo,sizeof(mergeInfo));
    fields.push_back(blob(0xe2d,rel-10,sizeof(mergeInfo)));
    const uint8_t previewInfo[]={'0','1','0','0',16,0,0,0,uint8_t(pw),uint8_t(pw>>8),uint8_t(ph),uint8_t(ph>>8),3,14,14,14};
    auto ppos=append(note,previewInfo,sizeof(previewInfo));
    note.resize(note.size()+pw*ph*6);
    fields.push_back(blob(0xe2e,ppos-10,16+pw*ph*6));
    require(fields.size()<=maker.fields.size(),"No room for NEFX MakerNote records");
    directory(note,maker.base-block.data+u32(t.bytes+maker.base+4),std::move(fields));
    const auto origin=append(out,note.data(),note.size());
    put32(out,block.entry+4,uint32_t(note.size()));put32(out,block.entry+8,origin);
    const std::string software=ca?"Nikon Pixel Merge (CA on)":"Nikon Pixel Merge (CA off)";
    const auto softwareOffset=append(out,reinterpret_cast<const uint8_t*>(software.c_str()),software.size()+1);
    const auto softwareEntry=t.tag(main,305).entry;
    put32(out,softwareEntry+4,uint32_t(software.size()+1));put32(out,softwareEntry+8,softwareOffset);
    align16(out);setRaw(273,uint32_t(out.size()));
    return {std::move(out),origin+ppos+16};
}
void write_all(FILE* fp,const void* data,size_t size){
    if(std::fwrite(data,1,size,fp)!=size) throw std::runtime_error("Cannot write NEFX (check available disk space)");
}
}

struct NefxWriter::Impl {
    FILE* fp=nullptr;
    int width,height,rows=0;
    Bytes packed, preview;
    size_t previewOffset;
    std::array<int,pw+1> bounds;
    std::array<uint32_t,pw*3> sums{};
    int previewY=0,previewRows=0;
    Impl(const std::filesystem::path& dest,const std::filesystem::path& first,int w,int h,bool ca,unsigned n):
        width(w),height(h) {
        auto data=header(first,w,h,ca,n); previewOffset=data.preview;
        packed.resize(size_t(w)*3*14/8);preview.resize(pw*ph*6);
        // NX clamps the last exclusive preview-bin boundary to the last pixel.
        for(int i=0;i<=pw;i++)bounds[i]=std::min(i*w/pw,w-1);
        fp=std::fopen(dest.c_str(),"wb"); if(!fp) throw std::runtime_error("Cannot create temporary NEFX");
        try{write_all(fp,data.bytes.data(),data.bytes.size());}catch(...){std::fclose(fp);fp=nullptr;throw;}
    }
    ~Impl(){if(fp) std::fclose(fp);}
    void row(const uint16_t* rgb) {
        require(fp && rows<height && rgb,"Too many NEFX rows or invalid pixels");
        for(int i=0,j=0;i<width*3;i+=4,j+=7){
            const uint32_t a=rgb[i],b=rgb[i+1],c=rgb[i+2],d=rgb[i+3];
            require((a|b|c|d)<=16383,"Out of range NEFX sample");
            packed[j]=uint8_t(a);packed[j+1]=uint8_t((a>>8)|(b<<6));packed[j+2]=uint8_t(b>>2);
            packed[j+3]=uint8_t((b>>10)|(c<<4));packed[j+4]=uint8_t(c>>4);
            packed[j+5]=uint8_t((c>>12)|(d<<2));packed[j+6]=uint8_t(d>>6);
        }
        write_all(fp,packed.data(),packed.size());
        ++rows;
        if(rows==height)return; // The full-resolution row was already written.
        ++previewRows;
        for(int i=0;i<pw;i++){
            uint32_t a=0,b=0,c=0;
            for(int j=bounds[i];j<bounds[i+1];j++){a+=rgb[j*3];b+=rgb[j*3+1];c+=rgb[j*3+2];}
            sums[i*3]+=a;sums[i*3+1]+=b;sums[i*3+2]+=c;
        }
        if(rows==std::min((previewY+1)*height/ph,height-1)){
            for(int i=0;i<pw*3;i++){
                const int count=(bounds[i/3+1]-bounds[i/3])*previewRows;
                put16(preview,2*(previewY*pw*3+i),uint16_t(sums[i]/count));
            }
            sums.fill(0);++previewY;previewRows=0;
        }
    }
    void finish(){
        require(fp && rows==height && previewY==ph,"NEFX is incomplete");
        if(std::fseek(fp,long(previewOffset),SEEK_SET)) throw std::runtime_error("Cannot seek to NEFX preview");
        write_all(fp,preview.data(),preview.size());
        FILE* closeFp=fp;fp=nullptr;
        if(std::fclose(closeFp))throw std::runtime_error("Failed to finish NEFX");
    }
};
NefxWriter::NefxWriter(const std::filesystem::path& dest,const std::filesystem::path& first,int w,int h,bool ca,unsigned n):
    impl_(std::make_unique<Impl>(dest,first,w,h,ca,n)){}
NefxWriter::~NefxWriter()=default;
void NefxWriter::row(const uint16_t* p){impl_->row(p);}
void NefxWriter::finish(){impl_->finish();}
}
