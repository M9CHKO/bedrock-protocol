#include <bedrock/relay/MapArchive.hpp>
#include <bedrock/relay/MapShulkerQueue.hpp>
#include <bedrock/protodef/ProtoDefPacketDecoder.hpp>
#include <bedrock/protodef/ProtoDefNbt.hpp>
#include <iostream>
#include <fstream>
using namespace bedrock;
static int checks=0;
static void require(bool c,const std::string& s){++checks;if(!c)throw std::runtime_error(s);}
static void placementSearchTests(){
    using P=MapPlacement;using C=P::Check;using Pos=P::Pos;using B=PlatformBuilder::Block;
    for(auto origin:{Pos{0,64,0},Pos{-1008668,180,-223851}}){
        P::Camera camera{origin.x+.5,origin.y+1.62,origin.z+.5,0,0,1,1,true};
        std::map<Pos,B> overrides;
        const auto cell=[&](int x,int y,int z){return Pos{origin.x+x,origin.y+y,origin.z+z};};
        const B air{true,true,false,0,"air"},floor{true,false,true,10,"quartz_block"},obstacle{true,false,false,20,"chest"};
        const auto world=[&](Pos p){auto it=overrides.find(p);return it!=overrides.end()?it->second:p.y==origin.y-1?floor:air;};
        auto site=P::find(camera,world);require(site.position.has_value(),"flat platform has a local site");
        require(P::check(camera,*site.position,world)==C::Ready,"chosen site passes execution-time validation");
        require(P::check(camera,origin,world)==C::PlayerOverlap,"do not place inside player");
        for(auto p:{cell(1,0,0),cell(-1,0,0),cell(0,0,1),cell(0,0,-1),cell(-1,0,-1),cell(-1,0,1),cell(1,0,-1)})overrides[p]=obstacle;
        site=P::find(camera,world);
        require(site.position==cell(1,0,1),"find diagonal when all four cardinal neighbours are occupied");
        overrides[cell(1,1,1)]=obstacle;
        require(P::check(camera,cell(1,0,1),world)==C::Occupied,"must leave lid clearance above selected site");
        overrides[cell(1,1,1)]=B{};
        require(P::check(camera,cell(1,0,1),world)==C::Unknown,"unloaded clearance is not air");
        overrides.clear();overrides[cell(1,-1,0)]=B{};
        require(P::check(camera,cell(1,0,0),world)==C::Unknown,"unloaded floor is not support");
        overrides[cell(1,-1,0)]=air;
        require(P::check(camera,cell(1,0,0),world)==C::NoSupport,"no floating shulker above void");
        overrides[cell(1,-1,0)]=obstacle;
        require(P::check(camera,cell(1,0,0),world)==C::NoSupport,"do not open an interactive block instead of placing");
        overrides.clear();overrides[cell(0,1,0)]=obstacle;
        require(P::check(camera,cell(1,0,0),world)==C::Occluded,"reject placement ray blocked by a solid cell");
        overrides.clear();camera.x=origin.x+.95;
        require(P::check(camera,cell(1,0,0),world)==C::PlayerOverlap,"fractional position can overlap adjacent block");
        require(P::check(camera,cell(2,0,0),world)==C::Ready,"second grid neighbour is nearby at a fractional position");
        require(P::check(camera,cell(3,0,0),world)==C::OutOfRange,"avoid distant drops without a pickup walking controller");
        camera.x=origin.x+.5;
        for(int dy:{-1,1}){
            const Pos expected=cell(1,dy,0);
            auto stepped=[&](Pos p){return p==cell(1,dy-1,0)?floor:air;};
            site=P::find(camera,stepped);require(site.position==expected,"search a neighbouring height as well as feet level");
        }
        site=P::find(camera,[](Pos){return B{};});
        require(!site.position&&site.count(C::Unknown)>0,"unknown world never starts placement");
        require(std::string(P::message(site)).find("не получило блоки")!=std::string::npos,"missing world data has distinct message");
        camera.known=false;int reads=0;
        site=P::find(camera,[&](Pos){++reads;return air;});require(!site.position&&reads==0,"unknown camera must not sample origin zero");
        camera.known=true;camera.y=std::numeric_limits<double>::quiet_NaN();
        require(!P::find(camera,world).position,"nonfinite camera rejected before coordinate conversion");
    }
}
static std::vector<uint8_t> item(int id,int count=1,std::optional<int64_t> map={},size_t imageBytes=0,bool imageFirst=false){
    ProtoDefWriter w;w.zigzag32(id);if(!id)return w.take();w.u16le(count);w.varuint32(0);w.u8(0);w.zigzag32(id==5?500:0);
    ProtoDefWriter extra;
    if(map){extra.u16le(65535);extra.u8(1);auto nbt=NbtValue::compound();
        if(imageFirst)nbt.set("Colors",NbtValue::byteArray(std::vector<uint8_t>(imageBytes,42)));
        nbt.set("map_uuid",NbtValue::longInteger(*map));
        if(!imageFirst&&imageBytes)nbt.set("Colors",NbtValue::byteArray(std::vector<uint8_t>(imageBytes,42)));
        BinaryStream encoded;BedrockNbtCodec::write(encoded,{"",nbt},BedrockNbtEncoding::LittleEndian);extra.bytes(encoded.buffer());}
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
static void placementPackets(const std::vector<MapShulkerQueue::Packet>& out,const MapShulkerQueue::Camera& c,
    const std::vector<uint8_t>& expected,int hand,bool modern){
    require(out.size()==4,"placement emits start-use, swing, transaction, stop-use after equipment settles");
    require(out[0].name=="player_action"&&out[1].name=="animate"&&out[2].name=="inventory_transaction"&&out[3].name=="player_action","bot placement packet ordering");
    const auto position=[](ProtoDefReader& r,int x,int y,int z){require(r.zigzag32()==x,"position X");require(r.varuint32()==uint32_t(y),"position Y");require(r.zigzag32()==z,"position Z");};
    for(int i:{0,3}){PacketFieldCursor cursor(out[i].payload);ProtoDefReader r(cursor);require(!out[i].client&&r.varuint64()==c.runtime,"use-on runtime and direction");require(r.zigzag32()==(i==0?28:29),"start/stop item use action");
        position(r,1,i==0?63:64,0);position(r,i==0?1:0,i==0?64:0,0);require(r.zigzag32()==(i==0?1:0),"use-on face");}
    PacketFieldCursor cursor(out[2].payload);ProtoDefReader r(cursor);require(r.zigzag32()==0&&r.varuint32()==2,"legacy item-use transaction");
    require(r.varuint32()==1&&r.varuint32()==0&&r.zigzag32()==0&&r.varuint32()==uint32_t(hand),"one inventory consumption action in selected slot");
    const auto checkItem=[&](){auto parsed=ShulkerDeposit::readItem(r);require(parsed.id==5&&parsed.count==1,"single shulker in transaction");
        require(std::vector<uint8_t>(out[2].payload.begin()+parsed.begin,out[2].payload.begin()+parsed.end)==expected,"complete NBT wire preserved in placement");};
    checkItem();require(r.zigzag32()==0,"placing one shulker consumes it to air");require(r.varuint32()==0,"click block");if(modern)require(r.varuint32()==1,"player input trigger");
    position(r,1,63,0);require(r.zigzag32()==1&&r.zigzag32()==hand,"top face and selected hotbar");checkItem();
    require(std::abs(r.readF32LE()-c.x)<.01&&std::abs(r.readF32LE()-c.y)<.01&&std::abs(r.readF32LE()-c.z)<.01,"network player position");
    require(r.readF32LE()==.5&&r.readF32LE()==1&&r.readF32LE()==.5&&r.varuint32()==10,"top face click and support runtime");if(modern)require(r.varuint32()==1,"successful client prediction");
}
static void templateTests(){
    using N=NbtValue;using T=NbtTagType;
    auto entry=N::compound({{"id",N::shortInteger(358)},{"Slot",N::byte(0)},{"Count",N::byte(1)},
        {"tag",N::compound({{"map_uuid",N::longInteger(-5000000000000LL)},{"Colors",N::byteArray(std::vector<uint8_t>(65536,42))}})}});
    const auto tag=[&](std::vector<N> values){return N::compound({{"Items",N::list(T::Compound,std::move(values))}});};
    auto original=tag({entry});auto copy=original;
    require(MapShulkerQueue::templateMapIds(original).at(0)==-5000000000000LL,"legacy numeric map ID supported");
    require(original==copy,"template image bytes and ID not rewritten");
    auto named=entry;named.compoundValue.erase(named.compoundValue.begin());named.set("Name",N::string("minecraft:filled_map"));
    require(MapShulkerQueue::templateMapIds(tag({named})).size()==1,"named maps remain supported");
    auto both=entry;both.set("Name",N::string("minecraft:filled_map"));require(MapShulkerQueue::templateMapIds(tag({both})).size()==1,"consistent name and legacy ID");
    const auto rejects=[&](const N& value){bool failed=false;try{MapShulkerQueue::templateMapIds(value);}catch(const std::exception&){failed=true;}require(failed,"invalid map template rejected");};
    for(auto pair:std::vector<NbtNamedValue>{{"id",N::shortInteger(1)},{"id",N::string("358")},{"Name",N::string("minecraft:stone")},
        {"Slot",N::string("0")},{"Slot",N::byte(27)},{"Slot",N::byte(-1)},{"Count",N::byte(2)},{"tag",N::compound({{"map_uuid",N::integer(1)}})}}){auto bad=entry;bad.set(pair.name,pair.value);rejects(tag({bad}));}
    rejects(tag({entry,entry}));rejects(tag({}));rejects(N::compound({{"Items",N::list(T::String,{N::string("map")})}}));
    for(bool first:{false,true}){
        auto s=slot(item(4,1,-5000000000000LL,65536,first));
        require(MapShulkerQueue::mapUuid(s)==-5000000000000LL,"large opaque image skipped before/after UUID");
        auto truncated=*s.wire;truncated.resize(truncated.size()-10);s.wire=std::make_shared<const std::vector<uint8_t>>(truncated);
        require(!MapShulkerQueue::mapUuid(s),"truncated map NBT rejected");
    }
    require(!MapShulkerQueue::mapUuid(slot(item(4,1,42,MapShulkerQueue::MaximumMapWireBytes))),"oversize map is bounded");
    const auto rawMap=[](N root){ProtoDefWriter extra;extra.u16le(65535);extra.u8(1);BinaryStream nbt;BedrockNbtCodec::write(nbt,{"",root},BedrockNbtEncoding::LittleEndian);extra.bytes(nbt.buffer());
        extra.u32le(0);extra.u32le(0);auto bytes=extra.take();ProtoDefWriter w;w.zigzag32(4);w.u16le(1);w.varuint32(0);w.u8(0);w.zigzag32(0);w.varuint32(bytes.size());w.bytes(bytes);return slot(w.take());};
    require(!MapShulkerQueue::mapUuid(rawMap(N::compound({{"nested",N::compound({{"map_uuid",N::longInteger(42)}})}}))),"nested UUID cannot impersonate root ID");
    require(!MapShulkerQueue::mapUuid(rawMap(N::compound({{"map_uuid",N::longInteger(42)},{"map_uuid",N::longInteger(42)}}))),"duplicate UUID rejected");
    require(!MapShulkerQueue::mapUuid(rawMap(N::compound({{"map_uuid",N::integer(42)}}))),"UUID must be Long");
    for(bool modern:{false,true}){
        MapShulkerQueue q;q.slots.setVersion(modern?"1.21.100":"1.21.2");q.mapId=4;q.stage=MapShulkerQueue::Stage::NeedTemplate;
        std::vector<std::vector<uint8_t>> player(36,item(0));player[0]=item(4,1,42,65536);
        auto update=inventory(0,player,modern);q.inventory(update,true,0);
        require(q.slots.player[0].wire&&*q.slots.player[0].wire==player[0],"large map retained from full inventory");
        const auto size=q.slots.cachedBytes();q.inventory(update,true,1);require(q.slots.cachedBytes()==size,"replacement releases old map cache");
        player[0]=item(4,1,42,MapShulkerQueue::MaximumMapWireBytes);
        bool rejected=false;try{q.inventory(inventory(0,player,modern),true,2);}catch(const std::exception&){rejected=true;}
        require(rejected&&!q.slots.player[0].wire,"oversize item rejected without stale wire reuse");
        auto large=item(4,1,42,900*1024);player.assign(36,large);
        rejected=false;try{q.inventory(inventory(0,player,modern),true,3);}catch(const std::exception&){rejected=true;}
        require(rejected&&q.slots.cachedBytes()<=ShulkerDeposit::MaximumCacheBytes,"aggregate map cache bounded at 16 MiB");
    }
}
int main(int argc,char** argv){try{
    placementSearchTests();
    templateTests();
    std::filesystem::path dir=argc>1?argv[1]:"map-queue-test-artifacts";std::filesystem::create_directories(dir);
    for(bool compressed:{false,true}){
        auto bytes=zip("folder/0001.qznbt",{1,2,3,4,5},compressed);save(dir/"valid.zip",bytes);MapArchive a;a.open(dir/"valid.zip");require(a.entries.size()==1,"ZIP index");require(a.load(0)==std::vector<uint8_t>({1,2,3,4,5}),"ZIP roundtrip CRC");
        bytes.at(30+std::string("folder/0001.qznbt").size())^=1;save(dir/"bad-crc.zip",bytes);bool rejected=false;try{a.open(dir/"bad-crc.zip");a.load(0);}catch(...){rejected=true;}require(rejected,"CRC/deflate corruption rejected");
    }
    for(auto name:{"../escape.qznbt","/absolute.qznbt","C:/escape.qznbt","a\\b.qznbt","a/./b.qznbt","readme.txt"}){save(dir/"invalid.zip",zip(name,{1}));bool rejected=false;try{MapArchive a;a.open(dir/"invalid.zip");}catch(...){rejected=true;}require(rejected,"unsafe path/non-NBT rejected");}
    for(bool modern:{false,true})for(bool large:{false,true}){
        MapShulkerQueue q;q.slots.setVersion(modern?"1.21.100":"1.21.2");q.slots.shulkerIds={5};q.slots.retainedIngredientIds={1,2,4};q.mapId=4;q.shulkerId=5;q.shulkerRuntime=500;q.craft.shellId=1;q.craft.chestId=2;
        q.timing.craft=100;q.timing.open=100;q.timing.close=100;q.timing.place=100;q.timing.transfer=500;q.timing.hold=300;q.timing.next=100;
        std::vector<std::vector<uint8_t>> player(36,item(0));player[9]=item(1,64);player[10]=item(2,64);q.inventory(inventory(0,player,modern),true,0);
        MapShulkerQueue::Camera c{.5,65.62,.5,0,0,1ULL<<35,10,true};
        MapShulkerQueue::Target table{0,64,2,100,1,false,2},chestA{2,64,2,200,1,true,3},chestB{-2,64,2,200,1,true,3},box{1,64,0,0,1,false,1};
        bool placed=false;auto world=[&](platform::Pos p){PlatformBuilder::Block b;b.known=true;b.air=true;b.name="air";
            if(p.y==63){b.air=false;b.safeFloor=true;b.name="quartz_block";b.runtime=10;}
            if(placed&&p==platform::Pos{1,64,0}){b.air=false;b.name="undyed_shulker_box";b.runtime=500;}return b;};
        auto prepared=slot(large?item(5,1,42,65536):item(5));q.configureArchive(2);q.start(c,table,{chestA,chestB},box,0);require(q.needsTemplate(),"queue starts only after ZIP");
        std::vector<std::vector<uint8_t>> boxSlots(27,item(0));boxSlots[0]=item(4,1,4000000000000LL,large?65536:0,true);boxSlots[26]=item(4,1,-5000000000000LL,large?65536:0);
        auto mapOriginal=boxSlots;int nextWindow=1;unsigned crafts=0,holds=0,pickups=0,stores=0,closedCount=0;bool loadedThis=false;uint64_t afterCraftAt=0,equippedAt=0;
        for(uint64_t now=0;now<180000&&q.index<2;now+=100){
            if(q.templateDue(now)){q.loaded(prepared,{{0,4000000000000LL},{26,-5000000000000LL}},"000"+std::to_string(q.index)+".qznbt",now);boxSlots=mapOriginal;loadedThis=true;afterCraftAt=equippedAt=0;}
            auto out=q.poll(c,world,now);
            if(q.stage==MapShulkerQueue::Stage::AfterCraft&&!afterCraftAt)afterCraftAt=now;
            if(q.stage==MapShulkerQueue::Stage::PlaceEquipped&&!equippedAt){equippedAt=now;
                require(out.size()==3&&out[0].name=="mob_equipment","equipment separate from placement");
                require(q.poll(c,world,now+179).empty()&&q.stage==MapShulkerQueue::Stage::PlaceEquipped,"equipment settling delay before use-on");
                auto changed=q;
                const auto obstructed=[&](platform::Pos p){if(p==platform::Pos{1,65,0})return PlatformBuilder::Block{true,false,true,10,"stone"};return world(p);};
                require(changed.poll(c,obstructed,now+180).empty()&&changed.stage==MapShulkerQueue::Stage::Paused,
                    "new obstruction after equip blocks placement transaction without consuming item");}
            if(q.stage==MapShulkerQueue::Stage::PlaceWait){require(now>=equippedAt+180,"no early click after equip");placementPackets(out,c,*prepared.wire,q.heldIndex(),modern);}
            for(auto& p:out){
                ProtoDefPacketDecoder(modern?"1.21.100":"1.21.2").validatePacketStrict(p.name,p.payload);++checks;
                if(p.name=="inventory_slot"&&p.client){auto inv=ShulkerDeposit::readInventory(p.payload,false,modern);auto& raw=inv.items.at(0);std::vector<uint8_t> w(p.payload.begin()+raw.begin,p.payload.begin()+raw.end);
                    if(inv.window==0)player.at(inv.slot)=w;else if(!q.openedTarget.chest)boxSlots.at(inv.slot)=w;}
                if(p.name=="container_close"){q.closed(p.payload.at(0),true,now);require(q.stage!=MapShulkerQueue::Stage::Hold,"client close alone not sufficient");q.closed(p.payload.at(0),false,now);++closedCount;}
                if(p.name=="map_info_request"){PacketFieldCursor cursor(p.payload);ProtoDefReader reader(cursor);auto uuid=reader.zigzag64();q.mapUpdate(uuid,false);q.mapUpdate(uuid+1,true);auto wait=q.poll(c,world,now+1);require(q.stage==MapShulkerQueue::Stage::Hold,"wrong/no texture cannot advance");q.mapUpdate(uuid,true);++holds;}
            }
            // Nukkit may echo a slot or a complete player inventory after each
            // transfer. Both must retain the map's original wire NBT for return.
            if(q.stage!=MapShulkerQueue::Stage::Crafting&&afterCraftAt&&now>=afterCraftAt+1000){
                for(const auto& p:out)if(p.name=="inventory_slot"&&p.client){
                    const auto inv=ShulkerDeposit::readInventory(p.payload,false,modern);
                    if(inv.window==0)q.inventory(p.payload,false,now);
                }
                q.inventory(inventory(0,player,modern),true,now);
            }
            if(q.stage==MapShulkerQueue::Stage::Crafting&&q.craft.stage==AutoCraftStore::Stage::OpeningTable)q.opened(255,1,{0,64,2},now);
            if(q.stage==MapShulkerQueue::Stage::AfterCraft&&now<afterCraftAt+1000)require(out.empty(),"local crafted item alone cannot start placement before server echo");
            if(q.stage==MapShulkerQueue::Stage::PlaceWait){require(!placed,"must not place twice");placed=true;player[q.heldIndex()]=item(0);
                if(large){q.inventory(inventory(0,player,modern),true,now);require(q.stage==MapShulkerQueue::Stage::PlaceWait,"server consumption before UpdateBlock is not a failed transfer");}
                q.updateBlock({2,64,0},false,true,now);require(q.stage==MapShulkerQueue::Stage::PlaceWait,"unrelated block update cannot confirm placement");
                q.updateBlock({1,64,0},false,true,now);++crafts;}
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
        q.stage=MapShulkerQueue::Stage::AfterCraft;out=q.poll(c,world,400001);
        require(out.empty()&&q.stage==MapShulkerQueue::Stage::Paused&&q.status.find("Сервер не подтвердил созданный шалкер")!=std::string::npos,"missing crafted-item echo reports craft failure without placing a local prediction");
        // A rejected map transfer must stay paused; do not override it with Closing/Hold.
        MapShulkerQueue blocked;blocked.slots.setVersion(modern?"1.21.100":"1.21.2");blocked.mapId=4;blocked.shulkerId=5;blocked.shulkerRuntime=500;
        blocked.slots.shulkerIds={5};blocked.slots.retainedIngredientIds={4};
        player.assign(36,item(0));blocked.inventory(inventory(0,player,modern),true,0);blocked.configureArchive(1);blocked.start(c,table,{chestA},box,0);
        blocked.templateMaps={{0,4000000000000LL}};blocked.box=box;blocked.stage=MapShulkerQueue::Stage::OpeningBox;blocked.opened(50,30,{1,64,0},0);
        blocked.inventory(inventory(50,mapOriginal,modern),true,0);
        player[blocked.heldIndex()]=item(99);blocked.inventory(inventory(0,player,modern),true,0);
        auto rejected=blocked.poll(c,world,1000);require(rejected.empty()&&blocked.stage==MapShulkerQueue::Stage::Paused,"occupied hand rejects transfer without advancing");
        for(auto stage:{MapShulkerQueue::Stage::Crafting,MapShulkerQueue::Stage::AfterCraft,MapShulkerQueue::Stage::PlaceEquipped,MapShulkerQueue::Stage::PlaceWait,
            MapShulkerQueue::Stage::OpeningBox,MapShulkerQueue::Stage::OpeningChest,MapShulkerQueue::Stage::Closing,MapShulkerQueue::Stage::Transfer,MapShulkerQueue::Stage::Paused}){
            blocked.stage=stage;blocked.prepared=prepared;blocked.templateMaps={{0,42}};blocked.index=1;blocked.total=3;blocked.mapsDone=5;
            blocked.craft.stage=AutoCraftStore::Stage::OpeningTable;blocked.craft.running=true;
            const auto before=blocked.slots.player;blocked.stop(5000);
            require(!blocked.reserved()&&!blocked.craft.busy()&&blocked.windowId()==-1&&blocked.heldIndex()==-1,"explicit stop clears even unresolved or paused workflow");
            require(!blocked.prepared.wire&&blocked.templateMaps.empty()&&blocked.mapsDone==0&&blocked.index==1&&blocked.total==3,"stop releases template but preserves completed progress");
            blocked.opened(70,30,{1,64,0},5010);blocked.closed(70,true,5010);blocked.updateBlock({1,64,0},false,true,5010);
            require(blocked.poll(c,world,100000).empty()&&!blocked.reserved(),"late responses never restart canceled work");
            for(size_t i=0;i<before.size();++i)require(before[i].item.same(blocked.slots.player[i].item),"stop never fabricates inventory rollback");
        }
        require(slot(item(4,1,-5000000000000LL)).item.id==4 && MapShulkerQueue::mapUuid(slot(item(4,1,-5000000000000LL)))==-5000000000000LL,"signed 64-bit map uuid retained");
    }
    std::cout<<"Map queue: "<<checks<<" checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<"Map queue FAILED: "<<e.what()<<"\n";return 1;}}
