#pragma once
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <zlib.h>

namespace bedrock {
// Read-only ZIP index: never extracts archive paths. Only the current entry is
// inflated, with an exact size and CRC bound (including deflate bombs).
class MapArchive {
public:
    static constexpr uint64_t MaxArchive=2ULL*1024*1024*1024, MaxTotal=4ULL*1024*1024*1024;
    static constexpr uint32_t MaxEntry=8*1024*1024+80, MaxEntries=10000;
    struct Entry { std::string name; uint32_t offset,packed,size,crc; uint16_t method,flags; };
    std::vector<Entry> entries;
    std::filesystem::path path;
    uint64_t fingerprint=0;
    void open(const std::filesystem::path& file) {
        MapArchive next; next.path=file;
        std::ifstream in(file,std::ios::binary); if(!in) fail("Не удалось открыть ZIP");
        const auto size=std::filesystem::file_size(file);
        if(size<22 || size>MaxArchive) fail("ZIP должен быть меньше 2 ГиБ");
        auto tail=read(in,size-std::min<uint64_t>(size,65557),std::min<uint64_t>(size,65557));
        size_t end=tail.size();
        for(size_t at=tail.size()-22;;--at){if(u32(tail,at)==0x06054b50 && at+22+u16(tail,at+20)==tail.size()){end=at;break;}if(at==0)break;}
        if(end==tail.size())fail("Повреждён конец ZIP");
        const auto count=u16(tail,end+10), diskCount=u16(tail,end+8);
        uint32_t length=u32(tail,end+12),offset=u32(tail,end+16);
        if(u16(tail,end+4)||u16(tail,end+6)||count!=diskCount||count>MaxEntries||count==65535||length>8*1024*1024 || uint64_t(offset)+length>size-22)
            fail("ZIP64, многотомные ZIP или более 10000 файлов не поддерживаются");
        auto directory=read(in,offset,length); size_t p=0; uint64_t total=0; std::set<std::string> names;
        next.fingerprint=1469598103934665603ULL;
        for(auto c:directory){next.fingerprint^=c;next.fingerprint*=1099511628211ULL;}
        for(unsigned i=0;i<count;++i) {
            if(p+46>directory.size() || u32(directory,p)!=0x02014b50)fail("Повреждён каталог ZIP");
            auto n=u16(directory,p+28),extra=u16(directory,p+30),comment=u16(directory,p+32);
            if(p+46+n+extra+comment>directory.size() || !n || n>512)fail("Некорректное имя в ZIP");
            std::string name(directory.begin()+p+46,directory.begin()+p+46+n);
            if(name.find('\0')!=std::string::npos || name.find('\\')!=std::string::npos || name.front()=='/' || name.find(':')!=std::string::npos)
                fail("Небезопасный путь в ZIP");
            for(size_t a=0;a<name.size();) { auto b=name.find('/',a);if(b==std::string::npos)b=name.size();
                auto part=name.substr(a,b-a);if(part==".." || part==".")fail("Небезопасный путь в ZIP");a=b+1; }
            std::string lower=name;std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return char(c>='A'&&c<='Z'?c+32:c);});
            if(lower.ends_with(".qznbt")) {
                Entry e{name,u32(directory,p+42),u32(directory,p+20),u32(directory,p+24),u32(directory,p+16),u16(directory,p+10),u16(directory,p+8)};
                if((e.flags&1) || (e.method!=0&&e.method!=8) || !e.size || e.size>MaxEntry || e.packed>MaxEntry+65536 ||
                    uint64_t(e.offset)+30+e.packed>offset || ((u32(directory,p+38)>>16)&0170000)==0120000)
                    fail("Зашифрованный, слишком большой или неподдерживаемый QZNBT в ZIP");
                total+=e.size;if(total>MaxTotal || !names.insert(lower).second)fail("Повторы имён или более 4 ГиБ NBT в ZIP");
                next.entries.push_back(std::move(e));
            }
            p+=46+n+extra+comment;
        }
        if(p!=directory.size() || next.entries.empty())fail("В ZIP нет .qznbt файлов либо каталог повреждён");
        std::sort(next.entries.begin(),next.entries.end(),[](const Entry& a,const Entry& b){return a.name<b.name;});
        *this=std::move(next);
    }
    std::vector<uint8_t> load(size_t index) const {
        const auto& e=entries.at(index);std::ifstream in(path,std::ios::binary);if(!in)fail("ZIP недоступен");
        auto header=read(in,e.offset,30);
        if(u32(header,0)!=0x04034b50 || u16(header,6)!=e.flags || u16(header,8)!=e.method)fail("ZIP изменён после загрузки");
        const auto name=read(in,uint64_t(e.offset)+30,u16(header,26));
        if(std::string(name.begin(),name.end())!=e.name)fail("Имя файла ZIP не совпадает");
        auto packed=read(in,uint64_t(e.offset)+30+u16(header,26)+u16(header,28),e.packed);
        std::vector<uint8_t> bytes(e.size);
        if(e.method==0) {if(packed.size()!=bytes.size())fail("Неверный размер ZIP");bytes=std::move(packed);}
        else {
            z_stream z{};z.next_in=packed.data();z.avail_in=unsigned(packed.size());z.next_out=bytes.data();z.avail_out=unsigned(bytes.size());
            if(inflateInit2(&z,-MAX_WBITS)!=Z_OK)fail("Не удалось распаковать ZIP");
            auto result=inflate(&z,Z_FINISH);bool ok=result==Z_STREAM_END && z.total_out==e.size && z.total_in==e.packed;
            inflateEnd(&z);if(!ok)fail("Повреждённый deflate или превышен размер NBT");
        }
        if(uint32_t(crc32(0,bytes.data(),unsigned(bytes.size())))!=e.crc)fail("CRC файла QZNBT не совпадает");
        return bytes;
    }
private:
    [[noreturn]] static void fail(const char* s){throw std::runtime_error(s);}
    static uint16_t u16(const std::vector<uint8_t>& b,size_t p){return uint16_t(b.at(p))|uint16_t(b.at(p+1))<<8;}
    static uint32_t u32(const std::vector<uint8_t>& b,size_t p){return uint32_t(u16(b,p))|uint32_t(u16(b,p+2))<<16;}
    static std::vector<uint8_t> read(std::ifstream& in,uint64_t at,size_t count){
        std::vector<uint8_t> b(count);in.clear();in.seekg(std::streamoff(at));in.read(reinterpret_cast<char*>(b.data()),std::streamsize(count));
        if(!in || size_t(in.gcount())!=count)fail("ZIP обрезан или изменён");return b;
    }
};
}
