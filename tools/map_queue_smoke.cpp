#include <bedrock/relay/MapArchive.hpp>
#include <bedrock/relay/MapShulkerQueue.hpp>
#include <bedrock/protodef/ProtoDefPacketDecoder.hpp>
#include <bedrock/protodef/ProtoDefNbt.hpp>
#include <iostream>
#include <fstream>
using namespace bedrock;
static int checks=0;
static void require(bool c,const std::string& s){++checks;if(!c)throw std::runtime_error(s);}
static std::vector<uint8_t> item(int id,int count=1,std::optional<int64_t> map={}){
    ProtoDefWriter w;w.zigzag32(id);if(!id)return w.take();w.u16le(count);w.varuint32(0);w.u8(0);w.zigzag32(id==5?500:0);
    ProtoDefWriter extra;
    if(map){extra.u16le(65535);extra.u8(1);writeProtoDefNbt(extra,nbtDocumentToProtoDefValue({"",NbtValue::compound({{"map_uuid",NbtValue::longInteger(*map)}})}),BedrockNbtEncoding::LittleEndian);}
    else extra.u16le(0);
    extra.u32le(0);extra.u32le(0);auto bytes=extra.take();w.varuint32(bytes.size());w.bytes(bytes);return w.take();
}
static ShulkerDeposit::Slot slot(std::vector<uint8_t> bytes){auto ptr=std::make_shared<const std::vector<uint8_t>>(std::move(bytes));PacketFieldCursor c(*ptr);ProtoDefReader r(c);return {ShulkerDeposit::readItem(r),ptr,true};}
static std::vector<uint8_t> inventory(int id,const std::vector<std::vector<uint8_t>>& slots,bool modern){ProtoDefWriter w;w.varuint32(id);w.varuint32(slots.size());for(auto& s:slots)w.bytes(s);
    if(modern){w.u8(id?7:12);w.u8(0);w.zigzag32(0);}return w.take();}
static std::vector<uint8_t> zip(const std::string& name,const std::vector<uint8_t>& bytes,bool deflated=false){
    std::vector<uint8_t> packed=bytes;
    if(deflated){packed.resize(bytes.size()+64);z_stream z{};deflateInit2(&z,1,Z_DEFLATED,-15,8,Z_DEFAULT_STRATEGY);z.next_in=const_cast<uint8_t*>(bytes.data());z.avail_in=bytes.size();z.next_out=packed.data();z.avail_out=packed.size();require(deflate(&z,Z_FINISH)==Z_STREAM_END,"make deflate fixture");packed.resize(z.total_out);deflateEnd(&z);}
    uint32_t crc=crc32(0,bytes.data(),bytes.size());ProtoDefWriter w;
    w.u32le(0x04034b50);w.u16le(20);w.u16le(0);w.u16le(deflated?8:0);w.u32le(0);w.u32le(crc);w.u32le(packed.size());w.u32le(bytes.size());w.u16le(name.size());w.u16le(0);w.bytes(std::vector<uint8_t>(name.begin(),name.end()));w.bytes(packed);
    auto local=w.take();uint32_t offset=local.size();ProtoDefWriter d;d.u32le(0x02014b50);d.u16le(20);d.u16le(20);d.u16le(0);d.u16le(deflated?8:0);d.u32le(0);d.u32le(crc);d.u32le(packed.size());d.u32le(bytes.size());d.u16le(name.size());d.u16le(0);d.u16le(0);d.u16le(0);d.u16le(0);d.u32le(0);d.u32le(0);d.bytes(std::vector<uint8_t>(name.begin(),name.end()));auto dir=d.take();
    ProtoDefWriter e;e.u32le(0x06054b50);e.u16le(0);e.u16le(0);e.u16le(1);e.u16le(1);e.u32le(dir.size());e.u32le(offset);e.u16le(0);auto end=e.take();local.insert(local.end(),dir.begin(),dir.end());local.insert(local.end(),end.begin(),end.end());return local;
}
static void save(const std::filesystem::path& p,const std::vector<uint8_t>& bytes){std::ofstream f(p,std::ios::binary);f.write((const char*)bytes.data(),bytes.size());}
int main(int argc,char** argv){try{
    std::filesystem::path dir=argc>1?argv[1]:"map-queue-test-artifacts";std::filesystem::create_directories(dir);
    for(bool compressed:{false,true}){
        auto bytes=zip("folder/0001.qznbt",{1,2,3,4,5},compressed);save(dir/"valid.zip",bytes);MapArchive a;a.open(dir/"valid.zip");require(a.entries.size()==1,"ZIP index");require(a.load(0)==std::vector<uint8_t>({1,2,3,4,5}),"ZIP roundtrip CRC");
        bytes.at(30+std::string("folder/0001.qznbt").size())^=1;save(dir/"bad-crc.zip",bytes);bool rejected=false;try{a.open(dir/"bad-crc.zip");a.load(0);}catch(...){rejected=true;}require(rejected,"CRC/deflate corruption rejected");
    }
    for(auto name:{"../escape.qznbt","/absolute.qznbt","C:/escape.qznbt","a\\b.qznbt","a/./b.qznbt","readme.txt"}){save(dir/"invalid.zip",zip(name,{1}));bool rejected=false;try{MapArchive a;a.open(dir/"invalid.zip");}catch(...){rejected=true;}require(rejected,"unsafe path/non-NBT rejected");}
    for(bool modern:{false,true}){
        MapShulkerQueue q;q.slots.setVersion(modern?"1.21.100":"1.21.2");q.slots.shulkerIds={5};q.slots.retainedIngredientIds={1,2,4};q.mapId=4;q.shulkerId=5;q.shulkerRuntime=500;q.craft.shellId=1;q.craft.chestId=2;
        q.timing.craft=100;q.timing.open=100;q.timing.close=100;q.timing.place=100;q.timing.transfer=500;q.timing.hold=300;q.timing.next=100;
        std::vector<std::vector<uint8_t>> player(36,item(0));player[9]=item(1,64);player[10]=item(2,64);q.inventory(inventory(0,player,modern),true,0);
        MapShulkerQueue::Camera c{.5,65.62,.5,0,0,1ULL<<35,10,true};
        MapShulkerQueue::Target table{0,64,2,100,1,false,2},chestA{2,64,2,200,1,true,3},chestB{-2,64,2,200,1,true,3},box{1,64,0,0,1,false,1};
        bool placed=false;auto world=[&](platform::Pos p){PlatformBuilder::Block b;b.known=true;b.air=true;b.name="air";
            if(p.y==63){b.air=false;b.safeFloor=true;b.name="quartz_block";b.runtime=10;}
            if(placed&&p==platform::Pos{1,64,0}){b.air=false;b.name="undyed_shulker_box";b.runtime=500;}return b;};
        auto prepared=slot(item(5));q.configureArchive(2);q.start(c,table,{chestA,chestB},box,0);require(q.needsTemplate(),"queue starts only after ZIP");
        std::vector<std::vector<uint8_t>> boxSlots(27,item(0));boxSlots[0]=item(4,1,4000000000000LL);boxSlots[26]=item(4,1,-5000000000000LL);
        auto mapOriginal=boxSlots;int nextWindow=1;unsigned crafts=0,holds=0,pickups=0,stores=0,closedCount=0;bool loadedThis=false;
        for(uint64_t now=0;now<180000&&q.index<2;now+=100){
            if(q.templateDue(now)){q.loaded(prepared,{{0,4000000000000LL},{26,-5000000000000LL}},"000"+std::to_string(q.index)+".qznbt",now);boxSlots=mapOriginal;loadedThis=true;}
            auto out=q.poll(c,world,now);
            for(auto& p:out){
                ProtoDefPacketDecoder(modern?"1.21.100":"1.21.2").validatePacketStrict(p.name,p.payload);++checks;
                if(p.name=="inventory_slot"&&p.client){auto inv=ShulkerDeposit::readInventory(p.payload,false,modern);auto& raw=inv.items.at(0);std::vector<uint8_t> w(p.payload.begin()+raw.begin,p.payload.begin()+raw.end);
                    if(inv.window==0)player.at(inv.slot)=w;else if(!q.openedTarget.chest)boxSlots.at(inv.slot)=w;}
                if(p.name=="container_close"){q.closed(p.payload.at(0),true,now);require(q.stage!=MapShulkerQueue::Stage::Hold,"client close alone not sufficient");q.closed(p.payload.at(0),false,now);++closedCount;}
                if(p.name=="map_info_request"){PacketFieldCursor cursor(p.payload);ProtoDefReader reader(cursor);auto uuid=reader.zigzag64();q.mapUpdate(uuid,false);q.mapUpdate(uuid+1,true);auto wait=q.poll(c,world,now+1);require(q.stage==MapShulkerQueue::Stage::Hold,"wrong/no texture cannot advance");q.mapUpdate(uuid,true);++holds;}
            }
            if(q.stage==MapShulkerQueue::Stage::Crafting&&q.craft.stage==AutoCraftStore::Stage::OpeningTable)q.opened(255,1,{0,64,2},now);
            if(q.stage==MapShulkerQueue::Stage::PlaceWait){require(!placed,"must not place twice");placed=true;player[q.heldIndex()]=item(0);q.updateBlock({1,64,0},false,true,now);++crafts;}
            if(q.stage==MapShulkerQueue::Stage::OpeningBox){int id=nextWindow++;q.opened(id,30,{1,64,0},now);q.inventory(inventory(id,boxSlots,modern),true,now);}
            if(q.stage==MapShulkerQueue::Stage::BreakWait){require(boxSlots==mapOriginal,"all maps returned before breaking");placed=false;q.updateBlock({1,64,0},true,false,now);q.dropped(1ULL<<40,5,1.5,64.5,.5);q.taken(1ULL<<40,c.runtime);
                player[q.heldIndex()]=item(5);q.inventory(inventory(0,player,modern),true,now);++pickups;}
            if(q.stage==MapShulkerQueue::Stage::OpeningChest){int id=nextWindow++;bool full=stores==0;auto t=full?chestA:chestB;q.opened(id,0,{t.x,t.y,t.z},now);
                std::vector<std::vector<uint8_t>> contents(27,full?item(99):item(0));q.inventory(inventory(id,contents,modern),true,now);++stores;}
            if(q.stage==MapShulkerQueue::Stage::Paused)throw std::runtime_error("cycle paused: "+q.status+" stage previous counters "+std::to_string(holds));
        }
        require(loadedThis&&q.index==2&&!q.busy(),"two templates completed");require(crafts==2&&holds==4&&pickups==2&&stores==3,"all stages and full chest fallback");require(closedCount>=10,"separate map GUI close barriers");
        // Correction and no-ack paths must never blind-repeat transactions.
        q.configureArchive(1);q.start(c,table,{chestA},box,200000);q.loaded(prepared,{{0,4000000000000LL}},"x.qznbt",200000);
        q.stage=MapShulkerQueue::Stage::PlaceWait;auto out=q.poll(c,world,400000);require(out.empty()&&q.stage==MapShulkerQueue::Stage::Paused,"placement timeout pauses without resend");
        // A rejected map transfer must stay paused; do not override it with Closing/Hold.
        MapShulkerQueue blocked;blocked.slots.setVersion(modern?"1.21.100":"1.21.2");blocked.mapId=4;blocked.shulkerId=5;blocked.shulkerRuntime=500;
        blocked.slots.shulkerIds={5};blocked.slots.retainedIngredientIds={4};
        player.assign(36,item(0));blocked.inventory(inventory(0,player,modern),true,0);blocked.configureArchive(1);blocked.start(c,table,{chestA},box,0);
        blocked.templateMaps={{0,4000000000000LL}};blocked.box=box;blocked.stage=MapShulkerQueue::Stage::OpeningBox;blocked.opened(50,30,{1,64,0},0);
        blocked.inventory(inventory(50,mapOriginal,modern),true,0);
        player[blocked.heldIndex()]=item(99);blocked.inventory(inventory(0,player,modern),true,0);
        auto rejected=blocked.poll(c,world,1000);require(rejected.empty()&&blocked.stage==MapShulkerQueue::Stage::Paused,"occupied hand rejects transfer without advancing");
        require(slot(item(4,1,-5000000000000LL)).item.id==4 && MapShulkerQueue::mapUuid(slot(item(4,1,-5000000000000LL)))==-5000000000000LL,"signed 64-bit map uuid retained");
    }
    std::cout<<"Map queue: "<<checks<<" checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<"Map queue FAILED: "<<e.what()<<"\n";return 1;}}
