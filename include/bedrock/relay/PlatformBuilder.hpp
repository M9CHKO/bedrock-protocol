#pragma once
#include <bedrock/relay/PlatformPlan.hpp>
#include <bedrock/relay/ShulkerDeposit.hpp>
#include <functional>
#include <optional>
#include <string>

namespace bedrock {
// Shared Android/Windows controller. The host serializes calls. No game pointers,
// UI calls, recursive NBT decoding, sleeping, or network sends from this class.
class PlatformBuilder {
public:
    using Pos = platform::Pos;
    using Slot = ShulkerDeposit::Slot;
    struct Settings {
        int chunks=16, side=1, placeMs=50, refillMs=350, refillStacks=3, lanes=1, gapChunks=2, turnSide=-1;
        double walkSpeed=.12, travelSpeed=.30;
        bool travelBoost=true, lookAlong=true;
        void clamp() {
            chunks=std::clamp(chunks,1,1024); side=side<0?-1:1;
            placeMs=std::clamp(placeMs,50,2000); refillMs=std::clamp(refillMs,150,2000);
            refillStacks=std::clamp(refillStacks,1,6);
            lanes=std::clamp(lanes,1,32);gapChunks=std::clamp(gapChunks,1,16);turnSide=turnSide<0?-1:1;
            walkSpeed=std::isfinite(walkSpeed)?std::clamp(walkSpeed,.04,.20):.12;
            travelSpeed=std::isfinite(travelSpeed)?std::clamp(travelSpeed,.06,.60):.30;
        }
    } settings;
    struct Block { bool known=false, air=false, safeFloor=false; int32_t runtime=0; std::string name; };
    struct Camera { double x=0,y=0,z=0; float yaw=0,pitch=0; uint64_t runtime=0,tick=0; bool known=false; };
    struct Packet { std::string name; std::vector<uint8_t> payload; bool client=false; };
    struct Output {
        std::vector<Packet> packets;
        bool control=false, move=false, sneak=false;
        double x=0,y=0,z=0,dx=0,dz=0; float yaw=0;
    };
    enum class Stage { Idle, Build, Place, Row, Supply, Open, Take, Transfer, Close, Return, Paused };
    using World = std::function<Block(Pos)>;
    static constexpr const char* Names[4] = {"quartz_block","glowstone","chest","crafting_table"};
    Stage stage=Stage::Idle;
    std::string status="Готов. .start — строительство";
    std::vector<Pos> supplies;
    bool recording=false, supported=false, modern=true, authoritative=false;
    uint64_t recordsRevision=0;
    unsigned placed=0, checked=0;
    int rowIndex=0;
    bool busy() const { return stage!=Stage::Idle && stage!=Stage::Paused; }
    bool ownsWindow() const { return window>=0 || stage==Stage::Open || stage==Stage::Close; }
    const platform::Plan& planView() const { return plan; }
    void version(const std::string& value) {
        ShulkerDeposit probe; probe.setVersion(value); supported=probe.supported; modern=probe.modern;
    }
    void palette(const std::vector<std::pair<int64_t,std::string>>& items) {
        ids={};
        for (auto& [id,name]:items) for (int m=0;m<4;++m)
            if (name==Names[m] || name==std::string("minecraft:")+Names[m]) ids[m]=int32_t(id);
    }
    void reset() {
        stage=Stage::Idle; status="Подключитесь и нажмите Старт";
        player={}; chest.clear(); inventoryReady=false; window=-1; heldSlot=0;
        recording=false; synthetic=false; sneaking=false; search.reset(); route.clear();
        pending.reset(); watched.reset(); queued.clear(); screenWindow=-1;
        memory.clear();
    }
    void stop(std::string why="Остановлено") {
        status=std::move(why); recording=false; route.clear(); search.reset(); synthetic=false;
        if (window>=0) { afterClose=Stage::Idle; closeRequested=false; stage=Stage::Close; }
        else stage=Stage::Idle;
        // In-flight transfers remain watched: stopping must not erase corrections.
    }
    void pause(std::string why) {
        status=std::move(why); synthetic=false; route.clear(); search.reset();
        if (window>=0) { afterClose=Stage::Paused; closeRequested=false; stage=Stage::Close; }
        else stage=Stage::Paused;
    }
    void start(Camera c, const World& world, uint64_t now, bool resume=false) {
        if (busy()) { status="Уже строю. Сначала Стоп"; return; }
        if (!supported || authoritative) { status="Нужны 1.21.2/1.21.100 и legacy InventoryTransaction"; return; }
        if (!c.known || !inventoryReady) { status="Жду инвентарь игрока; переоткройте его"; return; }
        if (std::any_of(ids.begin(),ids.end(),[](int id){return id==0;})) { status="Сервер ещё не прислал ID материалов"; return; }
        auto f=feet(c);
        if (!walkable(f,world)) { status="Встаньте на ровную площадку с безопасной опорой"; return; }
        settings.clamp(); synthetic=false; recording=false; visited.clear(); checked=0;
        pending.reset(); watched.reset(); closeRequested=false; window=-1;
        if (!resume || tasks.empty()) {
            plan={{f.x,f.y-1,f.z},platform::direction(c.yaw),settings.side,settings.chunks};
            plan.lanes=settings.lanes;plan.gapChunks=settings.gapChunks;plan.turnSide=settings.turnSide;
            rowIndex=0; taskIndex=0; placed=0; tasks=plan.row(0); stage=Stage::Build;
        } else { goal={plan.at(std::max(0,rowIndex-1)).x,plan.origin.y+1,plan.at(std::max(0,rowIndex-1)).z}; stage=Stage::Return; }
        deadline=now+30000; nextAt=now; lastProgress=now; status="Строительство запущено";
    }
    bool removeSupply(int index) {
        if (busy() || index<1 || size_t(index)>supplies.size()) return false;
        supplies.erase(supplies.begin()+index-1); ++recordsRevision; status="Запись сундука удалена"; return true;
    }
    void record() {
        if (busy()) { status="Остановите строительство перед записью"; return; }
        recording=true; status="Откройте сундук снабжения — он добавится в список";
    }
    void opened(int id,int type,Pos pos,uint64_t now) {
        screenWindow=id; screenType=type;
        if (recording && type==0 && id>0 && id<=100) {
            recording=false;
            if (std::find(supplies.begin(),supplies.end(),pos)==supplies.end() && supplies.size()<256) {
                supplies.push_back(pos); ++recordsRevision; status="Сундук записан. Всего: "+std::to_string(supplies.size());
            } else status="Сундук уже записан либо достигнут лимит 256";
        }
        if (!busy()) return;
        if (stage==Stage::Open && platform::InventoryReadGate::matchesSupply(id,type,pos,target)) {
            window=id; chest.clear(); chestReady=false; stage=Stage::Take; deadline=now+30000;
            status="Жду содержимое сундука"; return;
        }
        // User menus are not a stop condition. Actual containers need a real
        // close handshake before a new placement / supply open can be sent.
        if (window<0) { window=id; afterClose=stage; stage=Stage::Close; closeRequested=false; }
    }
    void closed(int id,bool client,uint64_t now) {
        if (client && id==screenWindow) screenWindow=-1;
        if (stage==Stage::Close && id==window) {
            closeBarrier.observe(id,client);
            if (closeBarrier.ready(screenWindow<0)) {
                window=-1; chest.clear(); chestReady=false; closeRequested=false;
                stage=afterClose; nextAt=now+350; deadline=now+30000; lastProgress=now;
                route.clear(); search.reset();
            }
        } else if (busy() && id==window) pause("Сундук закрыт во время переноса; проверьте инвентарь");
    }
    void inventory(const std::vector<uint8_t>& bytes,bool full,uint64_t now) {
        // At most 54 headers, skip each opaque Item.extra in constant time.
        const auto inv=ShulkerDeposit::readInventory(bytes,full,modern);
        if (inv.window!=0 && (window<0 || inv.window!=uint32_t(window))) return;
        const bool own=inv.window==0;
        if (full && ((own && inv.items.size()!=36) || (!own && inv.items.size()!=27 && inv.items.size()!=54))) return;
        if (full && !own) chest.assign(inv.items.size(),{});
        const size_t first=full?0:inv.slot, size=own?player.size():chest.size();
        if (first>size || inv.items.size()>size-first) return;
        for (size_t i=0;i<inv.items.size();++i) {
            Slot slot{inv.items[i],{},true};
            const auto length=slot.item.end-slot.item.begin;
            if (material(slot.item)>=0 && length<=512)
                slot.wire=std::make_shared<const std::vector<uint8_t>>(bytes.begin()+slot.item.begin,bytes.begin()+slot.item.end);
            const int index=int(first+i);
            (own?player[index]:chest[index])=slot;
            if (pending) pending->barrier.observe(inv.window,index,slot.item.id,slot.item.count);
            if (watched) {
                watched->barrier.observe(inv.window,index,slot.item.id,slot.item.count);
                if (watched->barrier.corrected()) { watched.reset(); pending.reset(); pause("Сервер отклонил перенос. Проверьте ресурсы и нажмите Продолжить"); }
            }
        }
        if (own && full) inventoryReady=true;
        if (!own && full) chestReady=true;
        (void)now;
    }
    void equipped(int slot) { if(slot>=0 && slot<9) heldSlot=slot; }
    void blockUpdate(Pos p,int32_t runtime,const World& world) {
        if (stage!=Stage::Place || taskIndex>=tasks.size() || !(p==tasks[taskIndex].pos)) return;
        const auto b=world(p);
        placementAck=b.known && b.runtime==runtime && b.name==Names[tasks[taskIndex].material];
    }
    void correction(Camera c,uint64_t now) {
        if (!busy() || !c.known) return;
        synthetic=true; x=c.x; y=c.y; z=c.z; route.clear(); search.reset();
        nextAt=std::max(nextAt,now+1000); deadline=std::max(deadline,now+30000); lastProgress=now;
        status="Коррекция позиции: жду сервер и перестраиваю путь";
    }
    void manualInventory() { if(busy()) pause("Ручное перемещение предметов — пауза"); }
    Output poll(Camera c,const World& world,uint64_t now) {
        Output out; out.packets.swap(queued);
        if(c.known)memory.observe(feet(c),[&](Pos p){return walkable(p,world);});
        if (!busy()) { releaseSneak(out,c); return out; }
        out.control=true; out.sneak=sneaking;
        out.yaw=c.yaw; out.x=c.x; out.y=c.y; out.z=c.z;
        if (!c.known || !c.runtime) return out;
        if (synthetic) { c.x=x; c.y=y; c.z=z; }
        out.x=c.x; out.y=c.y; out.z=c.z;
        if (now<nextAt) return out;
        if (stage==Stage::Close) {
            releaseSneak(out,c);
            if (!closeRequested) {
                closeBarrier.begin(window); closeRequested=true; deadline=now+30000;
                ProtoDefWriter w; w.u8(window); w.u8(screenType); w.boolValue(true);
                out.packets.push_back({"container_close",w.take(),true});
                status="Закрываю окно: жду Minecraft и сервер";
            } else if(now>deadline) { stage=Stage::Paused; status="Закройте окно вручную и нажмите Продолжить"; }
            return out;
        }
        if (stage==Stage::Place) {
            const auto& task=tasks.at(taskIndex); const auto b=world(task.pos);
            if (placementAck && b.known && b.name==Names[task.material]) {
                auto& held=player[placedSlot];
                if(held.item.id==placedId && held.item.count==placedCount) {
                    held=withCount(held,held.item.count-1);
                    out.packets.push_back(slotUpdate(0,placedSlot,held));
                }
                ++placed; ++taskIndex; stage=Stage::Build; releaseSneak(out,c);
            } else {
                if(now>deadline) { pause("Нет UpdateBlock для установки. Пауза без повторной отправки"); releaseSneak(out,c); }
                else status="Жду UpdateBlock от сервера (до 30 сек)";
                return out;
            }
        }
        if (stage==Stage::Transfer) {
            if(!pending) { pause("Потеряно состояние переноса"); return out; }
            auto& t=*pending;
            if(t.barrier.corrected()) { pending.reset(); pause("Сервер скорректировал перенос — проверьте слоты"); return out; }
            // Legacy Nukkit can accept without echoing slots. Predict only the
            // exact two unchanged slots after a quiet interval; keep watching.
            if(t.barrier.ready() || now>=t.at+std::max(500,settings.refillMs)) {
                auto empty=Slot{{},{},true};
                (t.window==0?player[t.source]:chest[t.source])=empty; player[t.destination]=t.item;
                out.packets.push_back(slotUpdate(t.window,t.source,empty));
                out.packets.push_back(slotUpdate(0,t.destination,t.item));
                watched=t; pending.reset(); stage=transferReturn; nextAt=now+settings.refillMs;
            }
            return out;
        }
        if(stage==Stage::Open) {
            if(now>deadline) { visited.insert(target); chooseSupply(c,now); }
            return out;
        }
        if(stage==Stage::Supply || stage==Stage::Return || stage==Stage::Row) {
            if(!walk(c,world,now,out)) return out;
            if(stage==Stage::Row) {
                ++rowIndex; taskIndex=0; tasks=plan.row(rowIndex);
                if(tasks.empty()) { stop("Готово: платформа построена"); return out; }
                stage=Stage::Build;
            } else if(stage==Stage::Return) stage=Stage::Build;
            else {
                const auto b=world(target);
                if(!b.known || (b.name!="chest" && b.name!="trapped_chest")) { visited.insert(target); chooseSupply(c,now); return out; }
                if(!inReach(c,target)) { visited.insert(target); chooseSupply(c,now); return out; }
                releaseSneak(out,c);
                int hand=emptyHotbar();
                if(hand<0) for(int i=0;i<9;++i) if(player[i].wire) { hand=i; break; }
                if(hand<0) { pause("Нужна пустая ячейка или обычный материал в хотбаре"); return out; }
                equip(out,c,hand); click(out,c,target,1,b.runtime,hand);
                stage=Stage::Open; deadline=now+30000; status="Открываю сундук снабжения"; return out;
            }
        }
        if(stage==Stage::Take) {
            if(!chestReady) { if(now>deadline) { visited.insert(target); afterClose=Stage::Supply; stage=Stage::Close; closeRequested=false; chooseAfterClose=true; } return out; }
            int have=count(needed);
            if(have<settings.refillStacks*64) {
                int dest=emptyPlayer();
                if(dest>=0) for(size_t i=0;i<chest.size();++i) if(material(chest[i].item)==needed && chest[i].wire) {
                    transfer(out,window,int(i),dest,chest[i],Stage::Take,now); status="Пополняю "+std::string(Names[needed]); return out;
                }
            }
            visited.insert(target); ++checked;
            afterClose=have>0?Stage::Return:Stage::Supply;
            chooseAfterClose=have==0;
            if(have>0) goal=returnPos;
            stage=Stage::Close; closeRequested=false; return out;
        }
        if(stage!=Stage::Build) return out;
        if(screenWindow>=0) { window=screenWindow; afterClose=Stage::Build; stage=Stage::Close; closeRequested=false; return out; }
        for(int budget=0;budget<12 && taskIndex<tasks.size();++budget) {
            const auto task=tasks[taskIndex]; const auto block=world(task.pos);
            if(block.known && ((rowIndex==0 && task.pos.y==plan.origin.y && block.safeFloor) || block.name==Names[task.material] || (plan.lanes>1 && task.pos.y==plan.origin.y &&
                (block.name=="quartz_block" || block.name=="glowstone")))) { ++taskIndex; continue; }
            if(!block.known) { status="Жду загрузку блока перед строительством"; return out; }
            if(!block.air) { pause("Место занято другим блоком; разрушение выключено"); return out; }
            int slot=findMaterial(task.material,true);
            if(slot<0) {
                slot=findMaterial(task.material,false);
                if(slot>=0) {
                    int dest=emptyHotbar();
                    if(dest<0) { pause("Освободите ячейку хотбара для стройматериала"); return out; }
                    transfer(out,0,slot,dest,player[slot],Stage::Build,now); return out;
                }
                needed=task.material; visited.clear(); checked=0; returnPos=feet(c);
                chooseSupply(c,now); return out;
            }
            if(slot!=heldSlot) { equip(out,c,slot); nextAt=now+50; return out; }
            if(!inReach(c,task.pos)) { pause("Блок вне досягаемости. Продолжите с площадки рядом"); return out; }
            static constexpr Pos delta[]={{0,1,0},{0,-1,0},{0,0,1},{0,0,-1},{1,0,0},{-1,0,0}};
            bool found=false; Pos against; int face=1; Block support;
            for(int f:{1,2,3,4,5,0}) {
                Pos p{task.pos.x+delta[f].x,task.pos.y+delta[f].y,task.pos.z+delta[f].z};
                auto b=world(p); if(b.known && !b.air && (b.safeFloor || b.name=="chest" || b.name=="crafting_table")) { against=p;face=f;support=b;found=true;break; }
            }
            if(!found) { status="Жду надёжную опору для установки"; return out; }
            const bool interactive=support.name=="chest" || support.name=="crafting_table";
            if(interactive && !sneaking) { sneak(out,c,true); nextAt=now+150; return out; }
            if(task.material==platform::Chest) {
                auto lateral=plan.at(rowIndex,1); auto center=plan.at(rowIndex);
                out.yaw=float(std::atan2(-double(lateral.x-center.x),double(lateral.z-center.z))*180.0/3.141592653589793);
            }
            watched.reset(); // the next intentional use changes this stack
            click(out,c,against,face,support.runtime,slot);
            placedSlot=slot; placedCount=player[slot].item.count; placedId=player[slot].item.id;
            placementAck=false; stage=Stage::Place; deadline=now+30000; nextAt=now+settings.placeMs;
            status="Ставлю "+std::string(Names[task.material]); return out;
        }
        if(taskIndex==tasks.size()) {
            goal={plan.at(rowIndex).x,plan.origin.y+1,plan.at(rowIndex).z};
            stage=Stage::Row; route.clear(); search.reset(); lastProgress=now;
        }
        return out;
    }
    size_t retainedBytes() const {
        size_t total=0; for(auto& s:player) if(s.wire) total+=s.wire->size();
        for(auto& s:chest) if(s.wire) total+=s.wire->size(); return total;
    }
private:
    std::array<int32_t,4> ids{};
    std::array<Slot,36> player{}; std::vector<Slot> chest;
    bool inventoryReady=false,chestReady=false,synthetic=false,sneaking=false,placementAck=false;
    bool closeRequested=false,chooseAfterClose=false;
    int screenWindow=-1,screenType=0,window=-1,heldSlot=0,needed=0,placedSlot=0,placedCount=0,placedId=0;
    uint64_t nextAt=0,deadline=0,lastProgress=0;
    double x=0,y=0,z=0;
    platform::Plan plan{}; std::vector<platform::Task> tasks; size_t taskIndex=0;
    Pos target{},goal{},returnPos{}; std::set<Pos> visited;
    std::vector<Pos> route; size_t routeIndex=0; std::optional<platform::Search> search;
    platform::RouteMemory memory;bool memorySearch=false;
    Stage afterClose=Stage::Idle,transferReturn=Stage::Build;
    platform::CloseBarrier closeBarrier;
    struct Transfer { int window,source,destination; Slot item; uint64_t at; platform::TransferBarrier barrier; };
    std::optional<Transfer> pending,watched;
    std::vector<Packet> queued;
    static Pos feet(const Camera& c) { return {int(std::floor(c.x)),int(std::floor(c.y-1.62+.01)),int(std::floor(c.z))}; }
    static bool inReach(const Camera& c,Pos p) { return std::hypot(std::hypot(p.x+.5-c.x,p.z+.5-c.z),p.y+.5-c.y)<=4.25; }
    static bool walkable(Pos p,const World& world) {
        auto floor=world({p.x,p.y-1,p.z}); if(!floor.known || !floor.safeFloor) return false;
        for(int n=0;n<3;++n) { auto b=world({p.x,p.y+n,p.z}); if(!b.known || !b.air) return false; } return true;
    }
    int material(const ShulkerDeposit::Item& i) const {
        if(!i.present() || i.metadata || i.count>64) return -1;
        for(int m=0;m<4;++m) if(ids[m] && i.id==ids[m]) return m; return -1;
    }
    int findMaterial(int m,bool hotbar) const {
        for(int i=hotbar?0:9;i<(hotbar?9:36);++i) if(material(player[i].item)==m && player[i].wire) return i; return -1;
    }
    int emptyHotbar() const { for(int i=0;i<9;++i) if(player[i].known && !player[i].item.present()) return i; return -1; }
    int emptyPlayer() const { for(int i=0;i<36;++i) if(player[i].known && !player[i].item.present()) return i; return -1; }
    int count(int m) const { int n=0; for(auto& s:player) if(material(s.item)==m && s.wire) n+=s.item.count; return n; }
    static std::vector<uint8_t> wire(const Slot& s) { if(!s.item.present()) return {0}; if(!s.wire) throw std::runtime_error("material wire unavailable"); return *s.wire; }
    static Slot withCount(Slot s,int count) {
        if(count<=0) return {{},{},true};
        auto bytes=wire(s); PacketFieldCursor cur(bytes); ProtoDefReader r(cur); r.zigzag32(); auto off=r.offset();
        bytes[off]=uint8_t(count);bytes[off+1]=uint8_t(count>>8); s.item.count=count;
        s.wire=std::make_shared<const std::vector<uint8_t>>(std::move(bytes)); return s;
    }
    Packet slotUpdate(int w,int slot,const Slot& s) const {
        auto data=wire(s); return {"inventory_slot",ShulkerDeposit::slotPayload(w,uint8_t(slot),&data,{},255,modern),true};
    }
    void equip(Output& out,Camera c,int slot) {
        ProtoDefWriter w; w.varuint64(c.runtime); w.bytes(wire(player[slot])); w.u8(slot);w.u8(slot);w.u8(0);
        out.packets.push_back({"mob_equipment",w.take()}); heldSlot=slot;
    }
    void click(Output& out,Camera c,Pos p,int face,int runtime,int slot) const {
        ProtoDefWriter w; w.zigzag32(0);w.varuint32(2);w.varuint32(0);w.varuint32(0);
        if(modern) w.varuint32(1);
        w.zigzag32(p.x);w.varuint32(uint32_t(p.y));w.zigzag32(p.z);w.zigzag32(face);w.zigzag32(slot);
        w.bytes(wire(player[slot]));w.f32le(c.x);w.f32le(c.y);w.f32le(c.z);
        w.f32le(face==4?0:face==5?1:.5f);w.f32le(face==0?0:face==1?1:.5f);w.f32le(face==2?0:face==3?1:.5f);
        w.varuint32(uint32_t(runtime));if(modern)w.varuint32(1);
        out.packets.push_back({"inventory_transaction",w.take()});
    }
    void sneak(Output& out,Camera c,bool active) {
        ProtoDefWriter w; w.varuint64(c.runtime);w.zigzag32(active?11:12);
        for(int i=0;i<2;++i) {w.zigzag32(0);w.varuint32(0);w.zigzag32(0);} w.zigzag32(0);
        out.packets.push_back({"player_action",w.take()}); sneaking=active; out.sneak=active;
    }
    void releaseSneak(Output& out,Camera c) { if(sneaking && c.runtime) sneak(out,c,false); }
    void transfer(Output& out,int w,int source,int destination,Slot item,Stage next,uint64_t now) {
        ProtoDefWriter p;p.zigzag32(0);p.varuint32(0);p.varuint32(2);
        p.varuint32(0);p.zigzag32(w);p.varuint32(source);p.bytes(wire(item));p.zigzag32(0);
        p.varuint32(0);p.zigzag32(0);p.varuint32(destination);p.zigzag32(0);p.bytes(wire(item));
        out.packets.push_back({"inventory_transaction",p.take()});
        pending=Transfer{w,source,destination,item,now,{}};
        pending->barrier.begin(w,source,destination,item.item.id,item.item.count);
        watched.reset(); transferReturn=next; stage=Stage::Transfer;
    }
    void chooseSupply(Camera c,uint64_t now) {
        route.clear();search.reset();chooseAfterClose=false;
        auto best=supplies.end(); int64_t distance=INT64_MAX;
        for(auto it=supplies.begin();it!=supplies.end();++it) {
            if(visited.contains(*it) || platform::supplyApproaches(*it,plan.origin.y+1).empty()) continue;
            auto d=platform::horizontalDistance(feet(c),*it); if(d<distance) {distance=d;best=it;}
        }
        if(best==supplies.end()) { pause("Пополните ресурсы: проверено "+std::to_string(checked)+" из "+std::to_string(supplies.size())+" сундуков; остальные недоступны"); return; }
        target=*best;
        auto approaches=platform::supplyApproaches(target,plan.origin.y+1);
        goal=*std::min_element(approaches.begin(),approaches.end(),[&](Pos a,Pos b){return platform::horizontalDistance(feet(c),a)<platform::horizontalDistance(feet(c),b);});
        stage=Stage::Supply;lastProgress=now;deadline=now+30000;status="Иду к записанному сундуку";
    }
    bool walk(Camera c,const World& world,uint64_t now,Output& out) {
        if(chooseAfterClose) { chooseSupply(c,now); if(stage!=Stage::Supply) return false; }
        if(stage==Stage::Supply && inReach(c,target)) {
            // Stop at a reachable safe approach, not on/in the chest.
            if(walkable(feet(c),world)) return true;
        }
        if(std::hypot(goal.x+.5-c.x,goal.z+.5-c.z)<.12 && feet(c).y==goal.y) return true;
        auto canWalk=[&](Pos p){return walkable(p,world);};
        if(routeIndex>=route.size()) {
            if(!search) {
                memorySearch=memory.contains(feet(c)) && memory.contains(goal);
                if(memorySearch) search.emplace(feet(c),goal,32768,0);
                else {
                    route=platform::straightSegment(feet(c),goal,canWalk,16);routeIndex=1;
                    if(route.size()<2) search.emplace(feet(c),goal,4096,16);
                }
            }
            if(search) {
                search->step([&](Pos p){return memorySearch?memory.contains(p):canWalk(p);},128);
                if(!search->done()) return false;
                route=search->route();routeIndex=1;search.reset();
            }
            if(route.size()<2) {
                status="Жду безопасный путь и загрузку чанков";nextAt=now+1000;
                if(now-lastProgress>30000) {
                    if(stage==Stage::Supply) {visited.insert(target);chooseSupply(c,now);}
                    else pause("Путь всё ещё недоступен. Проверьте опору и нажмите Продолжить");
                }
                return false;
            }
        }
        auto p=route[routeIndex];
        if(!walkable(p,world)) { route.clear();search.reset();return false; }
        double dx=p.x+.5-c.x,dz=p.z+.5-c.z,dist=std::hypot(dx,dz);
        if(dist<.08) {++routeIndex;return false;}
        double step=std::min(dist,(stage==Stage::Supply || stage==Stage::Return) && settings.travelBoost?settings.travelSpeed:settings.walkSpeed);
        // Synthetic motion is capped per 50ms, independent of packet floods.
        out.dx=dx/dist*step;out.dz=dz/dist*step;
        x=c.x+out.dx;y=c.y;z=c.z+out.dz;
        if(!walkable({int(std::floor(x)),feet(c).y,int(std::floor(z))},world)) {route.clear();return false;}
        synthetic=true; out.move=true;out.x=x;out.y=y;out.z=z;
        if(settings.lookAlong) out.yaw=float(std::atan2(-dx,dz)*180.0/3.141592653589793);
        nextAt=now+50;lastProgress=now;status=stage==Stage::Supply?"Иду за ресурсами":stage==Stage::Return?"Возвращаюсь к стройке":"Строю и иду вперёд";
        return false;
    }
};
} // namespace bedrock
