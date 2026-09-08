#include <bedrock/relay/PlatformBuilder.hpp>
#include <bedrock/protodef/ProtoDefPacketDecoder.hpp>
#include <iostream>
#include <map>
#include <stdexcept>
using namespace bedrock;
using P=platform::Pos;
static int checks=0;
static void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
static std::vector<uint8_t> item(int id,int count=64,size_t extra=10) {
    ProtoDefWriter w;w.zigzag32(id);if(id){w.u16le(count);w.varuint32(0);w.u8(0);w.zigzag32(id);w.varuint32(extra);
        w.bytes(std::vector<uint8_t>(extra));}return w.take();
}
static std::vector<uint8_t> inventory(int window,const std::vector<std::vector<uint8_t>>& items,bool modern) {
    ProtoDefWriter w;w.varuint32(window);w.varuint32(items.size());for(auto& i:items)w.bytes(i);
    if(modern){w.u8(window?7:12);w.u8(0);w.zigzag32(0);}return w.take();
}
int main(){try{
    for(int x:{-1008670,-33,-17,-16,-1,0,15,16,33})for(int z:{-223924,-33,-17,-16,-1,0,15,16,33})for(int side:{-1,1})for(int q=0;q<4;++q){
        platform::Plan plan{{x,179,z},platform::direction(q*90.f),side,10};plan.lanes=3;plan.gapChunks=2;
        auto last=plan.at(0);
        for(int i=0;i<plan.rows();++i){auto p=plan.at(i);require(platform::horizontalDistance(last,p)<=1,"snake discontinuity");last=p;
            auto row=plan.row(i);if(row.size()>5){auto a=row[5].pos,b=row[6].pos;
                require(platform::chunk(a.x)==platform::chunk(b.x)&&platform::chunk(a.z)==platform::chunk(b.z),"split chest crosses chunk");}
        }
    }
    // U-shaped corridor: the only route first walks AWAY from the destination.
    platform::RouteMemory memory;
    auto floor=[](P p){return p.y==180 && ((p.z==0 || p.z==32)&&p.x>=0&&p.x<=160 || p.x==0&&p.z>=0&&p.z<=32);};
    for(int x=0;x<=160;++x){memory.observe({x,180,0},floor);memory.observe({x,180,32},floor);}
    for(int z=0;z<=32;++z)memory.observe({0,180,z},floor);
    platform::Search s({160,180,0},{160,180,32},32768);
    while(!s.done())s.step([&](P p){return memory.contains(p);},128);
    require(s.route().size()==353,"global backward detour");require(s.route()[1].x==159,"route must walk backwards first");
    for(bool modern:{false,true}) {
        PlatformBuilder b;b.version(modern?"1.21.100":"1.21.2");require(b.supported,"supported layout");
        b.palette({{1,"minecraft:quartz_block"},{2,"minecraft:glowstone"},{3,"minecraft:chest"},{4,"minecraft:crafting_table"}});
        auto items=std::vector<std::vector<uint8_t>>(36,item(0));for(int i=0;i<4;++i)items[i]=item(i+1);
        items[10]=item(999,1,6*1024*1024);
        b.inventory(inventory(0,items,modern),true,1);require(b.retainedBytes()<2048,"nested NBT must not be retained");
        b.record();b.opened(10,0,{-1000,181,-2000},2);require(b.supplies.size()==1,"record chest coordinates");
        b.closed(10,true,3);b.closed(10,false,4);b.record();b.opened(11,0,{-1002,180,-2000},5);
        require(b.supplies.size()==2,"record all chests");b.closed(11,true,6);b.closed(11,false,7);
        require(b.removeSupply(1)&&b.supplies.size()==1,"remove one record only");
        std::map<P,int> blocks;for(int x=-3;x<=3;++x)for(int z=-3;z<=0;++z)blocks[{x,179,z}]=1;
        auto world=[&](P p){PlatformBuilder::Block w;w.known=true;auto it=blocks.find(p);w.air=it==blocks.end();
            if(!w.air){w.runtime=it->second;w.name=PlatformBuilder::Names[it->second-1];w.safeFloor=it->second<=2;}else w.name="air";return w;};
        PlatformBuilder::Camera c{.5,181.62,.5,0,0,100,10,true};
        // Initial row preserves existing floor, the next row is built in air.
        for(int x=-3;x<=3;++x)blocks[{x,179,0}]=x==0?2:1;
        b.settings.chunks=1;b.start(c,world,100);require(b.busy(),"start builder");
        for(uint64_t now=100;now<3000;now+=50){auto out=b.poll(c,world,now);
            if(out.move){c.x=out.x;c.y=out.y;c.z=out.z;}
            for(auto& p:out.packets)if(p.name=="inventory_transaction"){
                auto decoded=ProtoDefPacketDecoder(modern?"1.21.100":"1.21.2").decodePacketStrict(p.name,p.payload);
                (void)decoded;
                require(b.stage==PlatformBuilder::Stage::Place,"placement state");
                auto before=b.placed;
                b.correction(c,now+1);require(b.busy(),"correction must not stop");
                auto pending=b.poll(c,world,now+1500);require(pending.packets.empty(),"no blind placement retry");
                require(b.placed==before,"no fake confirmation");
                ShulkerDeposit observer;observer.setVersion(modern?"1.21.100":"1.21.2");
                auto click=observer.observeTransaction(p.payload);require(click.has_value(),"read placement target");
                const P offsets[]={{0,-1,0},{0,1,0},{0,0,-1},{0,0,1},{-1,0,0},{1,0,0}};
                auto d=offsets[click->face];P placed{click->x+d.x,click->y+d.y,click->z+d.z};
                blocks[placed]=placed.x==0?2:1;
                b.blockUpdate(placed,blocks[placed],world);b.poll(c,world,now+1600);
                require(b.placed==before+1,"UpdateBlock unblocks next placement after correction");
                goto placement_done;
            }
        }
        throw std::runtime_error("no placement generated");
        placement_done:
        b.stop();require(!b.busy(),"stop without window");
        b.start(c,world,10000);b.opened(9,0,{1,180,1},10001);
        auto close=b.poll(c,world,11000);bool sawClose=false;
        for(auto& p:close.packets)if(p.name=="container_close"){sawClose=true;require(p.client&&p.payload.size()==3,"close wire both versions");}
        require(sawClose,"close foreign window instead of disabling");
        b.closed(9,false,11020);require(b.stage==PlatformBuilder::Stage::Close,"server close alone not enough");
        b.closed(9,true,11040);require(b.stage!=PlatformBuilder::Stage::Close,"two-sided close barrier");

        // Three recorded chests: two empty, third contains material. No new
        // stack-request packet, and no repeated visit to the first chest.
        PlatformBuilder refill;refill.version(modern?"1.21.100":"1.21.2");
        refill.palette({{1,"quartz_block"},{2,"glowstone"},{3,"chest"},{4,"crafting_table"}});
        items.assign(36,item(0));items[1]=item(2);
        refill.inventory(inventory(0,items,modern),true,0);
        refill.supplies={{2,180,-2},{2,180,-5},{2,180,-8}};
        blocks.clear();for(int xx=-3;xx<=3;++xx)for(int zz=-10;zz<=0;++zz)blocks[{xx,179,zz}]=xx==0?2:1;
        for(auto p:refill.supplies)blocks[p]=3;
        c={.5,181.62,.5,0,0,100,10,true};refill.start(c,world,100);
        ShulkerDeposit observer;observer.setVersion(modern?"1.21.100":"1.21.2");
        int opened=0,moved=0;bool returned=false;std::set<P> visited;
        for(uint64_t now=100;now<60000&&!returned;now+=50) {
            auto output=refill.poll(c,world,now);if(output.move){c.x=output.x;c.y=output.y;c.z=output.z;}
            for(auto& packet:output.packets) {
                require(packet.name!="item_stack_request","legacy transport only");
                ProtoDefPacketDecoder(modern?"1.21.100":"1.21.2").validatePacketStrict(packet.name,packet.payload);
                if(packet.name=="container_close") {
                    refill.closed(packet.payload[0],true,now+1);refill.closed(packet.payload[0],false,now+2);
                }
                if(packet.name!="inventory_transaction")continue;
                if(refill.stage==PlatformBuilder::Stage::Open) {
                    auto click=observer.observeTransaction(packet.payload);require(click.has_value(),"supply click");
                    P p{click->x,click->y,click->z};require(visited.insert(p).second,"do not loop one empty chest");
                    ++opened;refill.opened(opened,0,p,now+1);
                    auto slots=std::vector<std::vector<uint8_t>>(54,item(0));if(p.z==-8)slots[53]=item(1);
                    refill.inventory(inventory(opened,slots,modern),true,now+2);
                } else if(refill.stage==PlatformBuilder::Stage::Transfer) {
                    ++moved;require(opened==3,"visit all chests before transfer");
                    ShulkerDeposit::TransactionInfo info;observer.observeTransaction(packet.payload,&info);
                    require(info.type==0 && info.actions==2,"two balanced legacy actions");
                } else if(refill.stage==PlatformBuilder::Stage::Place) {
                    if(moved)returned=true;
                    else {
                        auto click=observer.observeTransaction(packet.payload);require(click.has_value(),"placement before refill");
                        const P offsets[]={{0,-1,0},{0,1,0},{0,0,-1},{0,0,1},{-1,0,0},{1,0,0}};
                        auto d=offsets[click->face];P p{click->x+d.x,click->y+d.y,click->z+d.z};blocks[p]=2;
                        refill.blockUpdate(p,2,world);
                    }
                }
            }
        }
        if(!(opened==3 && moved==1 && returned))throw std::runtime_error("refill and return: opens="+std::to_string(opened)+" moved="+std::to_string(moved)+" stage="+std::to_string(int(refill.stage))+" status="+refill.status);
        require(true,"refill and return to construction");
    }
    std::cout<<checks<<" PlatformBuilder checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<'\n';return 1;}}
