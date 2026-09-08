#pragma once
#include <bedrock/relay/AutoCraftStore.hpp>
#include <bedrock/relay/PlatformBuilder.hpp>
#include <bedrock/nbt/BedrockNbt.hpp>
#include <bedrock/BinaryStream.hpp>
#include <map>

namespace bedrock {
// One ZIP entry at a time. No file I/O, network calls, NBT trees, or timers here.
// The host serializes callbacks and sends Output only after encoding succeeds.
class MapShulkerQueue {
public:
    using Slot=ShulkerDeposit::Slot; using Pos=platform::Pos; using Camera=PlatformBuilder::Camera;
    using World=PlatformBuilder::World; using Packet=PlatformBuilder::Packet;
    using Target=AutoCraftStore::Target;
    struct Timing {
        int craft=1000,open=700,close=700,place=500,transfer=600,hold=1500,breakMs=3500,pickup=1000,store=700,next=1000,timeout=30000;
        void clamp(){for(auto p:{&craft,&open,&close,&place,&transfer,&hold,&pickup,&store,&next})*p=std::clamp(*p,100,10000);
            transfer=std::max(transfer,500);hold=std::max(hold,300);breakMs=std::clamp(breakMs,3000,15000);timeout=std::clamp(timeout,10000,120000);}
    } timing;
    enum class Stage {Idle,NeedTemplate,Crafting,AfterCraft,Place,PlaceWait,OpenBox,OpeningBox,Maps,Transfer,Closing,Hold,ReturnMap,Break,Digging,BreakWait,Pickup,FindChest,OpeningChest,Store,Next,Paused};
    Stage stage=Stage::Idle;
    std::string status="Загрузите ZIP с .qznbt",name;
    size_t index=0,total=0; unsigned mapsDone=0,stored=0;
    ShulkerDeposit slots;
    AutoCraftStore craft;
    std::vector<Target> chests;
    Target table,box,openedTarget;
    std::map<int,int64_t> templateMaps;
    Slot prepared;
    int mapId=0,shulkerId=0,shulkerRuntime=0;
    bool busy()const{return stage!=Stage::Idle && stage!=Stage::Paused;}
    bool reserved()const{return busy() || stage==Stage::Paused;}
    bool needsTemplate()const{return stage==Stage::NeedTemplate;}
    int windowId()const{return window;}
    int heldIndex()const{return hand;}
    bool canStart()const{return stage==Stage::Idle && total>0 && index<total;}
    void resetSession(){bool interrupted=reserved();slots.resetSession();craft.reset(slots);window=-1;container.clear();watch.clear();queued.clear();prepared={};
        stage=interrupted?Stage::Paused:Stage::Idle;status=interrupted?"Соединение прервано. Проверьте текущий шалкер; автоматического повтора нет":"Загрузите ZIP или нажмите Старт";}
    void configureArchive(size_t count,size_t completed=0){if(reserved())throw std::runtime_error("Сначала остановите модуль карт");total=count;index=std::min(completed,count);stored=unsigned(index);name.clear();prepared={};status="ZIP готов. Файлов: "+std::to_string(total);}
    void start(Camera c,Target workbench,std::vector<Target> storage,Target placement,uint64_t now){
        if(!canStart()){status=index>=total?"Очередь завершена / загрузите ZIP":"Сначала проверьте и остановите предыдущий цикл";return;}
        if(!slots.supported || slots.authoritative){status="Нужен legacy InventoryTransaction, 1.21.2 или 1.21.100";return;}
        if(!c.known || !slots.inventoryReady || !mapId || !shulkerId || !shulkerRuntime){status="Жду ID сервера и инвентарь. Откройте и закройте инвентарь";return;}
        hand=-1;for(int i=0;i<9;++i)if(slots.player[i].known&&!slots.player[i].item.present()){hand=i;break;}
        if(hand<0){status="Освободите одну ячейку хотбара";return;}
        for(auto& s:slots.player)if(s.item.present()&&slots.shulkerIds.count(s.item.id)){status="Уберите остальные шалкеры: нужен однозначный контроль подбора";return;}
        if(storage.empty()){status="Нет доступного сундука в радиусе 6 блоков";return;}
        timing.clamp();origin=c;table=workbench;chests=std::move(storage);box=placement;
        tried.clear();fullChests.clear();watch.clear();mapsDone=0;stopRequested=equippedMap=picked=false;pickedSlot=-1;window=-1;stage=Stage::NeedTemplate;nextAt=now;status="Читаю текущий NBT из ZIP";
    }
    void loaded(Slot item,std::map<int,int64_t> maps,std::string filename,uint64_t now){
        if(!needsTemplate())return;
        if(!item.wire || item.item.id!=shulkerId || maps.empty() || maps.size()>27)throw std::runtime_error("Шаблон должен содержать карты в 27 ячейках шалкера");
        prepared=std::move(item);templateMaps=std::move(maps);name=std::move(filename);done.clear();mapsDone=0;currentMap=-1;resultSlot=-1;dropId=0;picked=false;
        slots.enabled=false;slots.stopped=false;craft.preparedResult=prepared;craft.resultId=shulkerId;craft.resultRuntime=shulkerRuntime;craft.templateName=name;
        craft.configureTiming(timing.craft,timing.open);craft.start(slots,{table,chests.front()},origin.x,origin.y,origin.z,now);
        if(!craft.busy()){pause(craft.status);return;}stage=Stage::Crafting;status="Крафт: "+name;
    }
    void pause(std::string reason){status=std::move(reason);stage=Stage::Paused;}
    void stop(uint64_t now){
        if(stage==Stage::Idle)return;
        if(craft.busy()){craft.stop(slots,"Остановлено. Проверьте текущий шалкер",now);stopRequested=true;stage=Stage::Crafting;return;}
        if(stage==Stage::OpeningBox||stage==Stage::OpeningChest){stopRequested=true;status="Ожидаю запрошенное окно перед остановкой";return;}
        if(window>=0){stopRequested=true;closeTo(Stage::Idle,now);}else{stage=Stage::Idle;prepared={};watch.clear();status="Остановлено. Текущий NBT не отмечен завершённым";}
    }
    void opened(int id,int type,Pos p,uint64_t now){
        if(stage==Stage::Crafting){craft.opened(slots,id,type,p.x,p.y,p.z,now);return;}
        if(!busy())return;
        const bool storage=stage==Stage::OpeningChest;
        auto expected=storage?destination:box;
        const int d=std::abs(p.x-expected.x)+std::abs(p.z-expected.z);
        if((stage!=Stage::OpeningBox&&!storage)||id<1||id>100||p.y!=expected.y||d>(storage?1:0)|| (storage?type!=0:(type!=0&&type!=30))){pause("Открылся другой контейнер. Проверьте его вручную");return;}
        window=id;windowType=type;openedTarget=expected;container.clear();ready=false;deadline=now+timing.timeout;nextAt=now+timing.open;
        stage=storage?Stage::Store:(currentMap>=0?Stage::ReturnMap:Stage::Maps);
        if(stopRequested)closeTo(Stage::Idle,now);
    }
    void closed(int id,bool client,uint64_t now){
        if(stage==Stage::Crafting){craft.closed(slots,id,now,client);return;}
        if(stage!=Stage::Closing || (id!=window&&id!=255)) {if(busy()&&id==window)pause("Окно закрыто во время обработки карты");return;}
        if(client)clientClosed=true;else serverClosed=true;
        if(clientClosed&&serverClosed){window=-1;container.clear();ready=false;stage=afterClose;nextAt=now+timing.close;deadline=now+timing.timeout;
            std::erase_if(watch,[](auto& entry){return entry.first.first!=0;});
            if(stage==Stage::Hold){holdAt=nextAt;mapData=false;} if(stage==Stage::Idle){prepared={};stopRequested=false;status="Остановлено. Проверьте текущий шалкер";}}
    }
    void inventory(const std::vector<uint8_t>& bytes,bool full,uint64_t now){
        auto inv=ShulkerDeposit::readInventory(bytes,full,slots.modern);
        if(inv.window!=0 && (window<0||inv.window!=uint32_t(window)))return;
        if(inv.window==0){slots.observeInventory(bytes,full,now);if(stage==Stage::Crafting)craft.inventory(slots,inv,now);}
        else {if(full){if(inv.items.size()!=27&&inv.items.size()!=54)throw std::runtime_error("Размер контейнера не поддержан");container.assign(inv.items.size(),{});ready=true;}
            auto first=full?0u:inv.slot;if(first>container.size()||inv.items.size()>container.size()-first)return;
            for(size_t i=0;i<inv.items.size();++i){Slot item{inv.items[i],{},true};const auto n=item.item.end-item.item.begin;
                if(item.item.id==mapId && n<=32768)item.wire=std::make_shared<const std::vector<uint8_t>>(bytes.begin()+item.item.begin,bytes.begin()+item.item.end);
                container[first+i]=std::move(item);}}
        auto first=full?0u:inv.slot;
        for(size_t i=0;i<inv.items.size();++i){auto it=watch.find({int(inv.window),int(first+i)});if(it!=watch.end()&&!inv.items[i].same(it->second)){
                pause("Сервер скорректировал перенос. Проверьте карту / шалкер; повтор не отправлен");return;}}
        if(stage==Stage::Pickup && inv.window==0 && picked) locatePicked();
    }
    void updateBlock(Pos p,bool air,bool shulker,uint64_t now){
        if(!(p==Pos{box.x,box.y,box.z}))return;
        if(stage==Stage::PlaceWait && shulker){watch.clear();slots.player[hand]=Slot{{},{},true};queued.push_back(slotPacket(0,hand,slots.player[hand]));stage=Stage::OpenBox;nextAt=now+timing.place;}
        if((stage==Stage::BreakWait||stage==Stage::Digging) && air){stage=Stage::Pickup;deadline=now+timing.timeout;nextAt=now+timing.pickup;status="Жду подбор своего шалкера";}
    }
    void dropped(uint64_t id,int itemId,double x,double y,double z){
        if((stage==Stage::Digging||stage==Stage::BreakWait||stage==Stage::Pickup) && itemId==shulkerId &&
            std::abs(x-box.x-.5)<1.5 && std::abs(y-box.y-.5)<2 && std::abs(z-box.z-.5)<1.5)dropId=id;
    }
    void taken(uint64_t id,uint64_t player){if(id&&id==dropId&&player==origin.runtime){picked=true;locatePicked();}}
    void mapUpdate(int64_t id,bool texture){if(stage==Stage::Hold&&currentMap>=0&&id==templateMaps.at(currentMap)&&texture)mapData=true;}
    void manual(){if(busy())pause("Ручное действие: модуль карт на паузе. Проверьте текущий предмет");}
    std::vector<Packet> poll(Camera c,const World& world,uint64_t now){
        std::vector<Packet> out;out.swap(queued);
        if(!busy()||stage==Stage::NeedTemplate)return out;
        if(!c.known || std::abs(c.x-origin.x)>.65 || std::abs(c.y-origin.y)>.7 || std::abs(c.z-origin.z)>.65){pause("Игрок сместился: остановлено для защиты предметов");return out;}
        if(stage==Stage::Crafting){
            if(craft.crafted && craft.stage!=AutoCraftStore::Stage::Closing)craft.stop(slots,"Шалкер создан",now);
            auto a=craft.poll(slots,now);status=craft.status;
            if(a.kind==AutoCraftStore::Action::Open){openedTarget=a.target;use(out,c,a.target,hand,0);}
            if(a.kind==AutoCraftStore::Action::Close){windowType=a.windowType;openedTarget=a.target;out.push_back(closePacket(a.window,a.windowType));}
            if(a.kind==AutoCraftStore::Action::CraftOne){for(auto& t:a.transactions)out.push_back({"inventory_transaction",std::move(t),false});
                for(auto& [i,w]:a.updates){out.push_back({"inventory_slot",ShulkerDeposit::slotPayload(0,i,&w,{},255,slots.modern),true});if(slots.player[i].item.id==shulkerId)resultSlot=i;}}
            if(!craft.busy()){
                if(stopRequested){stopRequested=false;stage=Stage::Idle;return out;}
                if(resultSlot<0){pause(status);return out;}
                stage=Stage::AfterCraft;nextAt=now+timing.close;
            }return out;
        }
        if(now<nextAt)return out;
        if(stage==Stage::Closing){if(!closeSent){out.push_back(closePacket(window,windowType));closeSent=true;deadline=now+timing.timeout;status="Закрываю окно Minecraft";}
            else if(now>=deadline)pause("Нет подтверждения закрытия окна");return out;}
        if(stage==Stage::OpeningBox||stage==Stage::OpeningChest||stage==Stage::PlaceWait||stage==Stage::BreakWait){if(now>=deadline)pause("Нет ответа сервера на действие — автоматический повтор запрещён");return out;}
        if(stage==Stage::Transfer){stage=transferNext;return out;}
        if(stage==Stage::AfterCraft){if(resultSlot!=hand){move(out,0,resultSlot,0,hand,Stage::Place,now);return out;}stage=Stage::Place;}
        if(stage==Stage::Place){
            auto pos=Pos{box.x,box.y,box.z};auto b=world(pos),support=world({pos.x,pos.y-1,pos.z});
            if(!b.known||!b.air||!support.known||!support.safeFloor){pause("Место рядом занято либо нет безопасной опоры");return out;}
            if(slots.player[hand].item.id!=shulkerId||slots.player[hand].item.count!=1){pause("Созданный шалкер не в рабочей ячейке");return out;}
            Target floor{pos.x,pos.y-1,pos.z,support.runtime,1};use(out,c,floor,hand,0);stage=Stage::PlaceWait;deadline=now+timing.timeout;status="Устанавливаю шалкер: жду UpdateBlock";return out;
        }
        if(stage==Stage::OpenBox){auto b=world({box.x,box.y,box.z});if(!b.known||b.name.find("shulker_box")==std::string::npos){pause("Шалкер на месте не найден");return out;}
            box.runtime=b.runtime;use(out,c,box,hand,0);stage=Stage::OpeningBox;deadline=now+timing.timeout;status="Открываю свой шалкер";return out;}
        if(stage==Stage::Maps||stage==Stage::ReturnMap||stage==Stage::Store){if(!ready){if(now>=deadline)pause("Сервер не прислал содержимое контейнера");return out;}}
        if(stage==Stage::Maps){
            auto next=std::find_if(templateMaps.begin(),templateMaps.end(),[&](auto& v){return !done.count(v.first);});
            if(next==templateMaps.end()){closeTo(Stage::Break,now);return out;}
            currentMap=next->first;
            if(size_t(currentMap)>=container.size()||container[currentMap].item.id!=mapId||container[currentMap].item.count!=1||!container[currentMap].wire){pause("В ожидаемом слоте нет одиночной карты. NBT сервера отличается");return out;}
            if(mapUuid(container[currentMap])!=std::optional<int64_t>(next->second)){pause("map_uuid карты отличается от шаблона — остановлено");return out;}
            move(out,window,currentMap,0,hand,Stage::Hold,now);
            if(stage!=Stage::Transfer)return out; // Keep a rejected transfer paused.
            stage=Stage::Transfer;transferNext=Stage::Closing;afterClose=Stage::Hold;closeSent=clientClosed=serverClosed=false;
            return out;
        }
        if(stage==Stage::Hold){
            if(!equippedMap){equip(out,c,hand);ProtoDefWriter request;request.zigzag64(templateMaps.at(currentMap));request.u32le(0);
                out.push_back({"map_info_request",request.take(),false});equippedMap=true;holdAt=now;deadline=now+timing.timeout;status="Показываю карту "+std::to_string(mapsDone+1)+" / "+std::to_string(templateMaps.size());return out;}
            if(now>=holdAt+timing.hold && mapData){stage=Stage::OpenBox;equippedMap=false;return out;}
            if(now>=deadline)pause("Нет данных изображения этой карты от сервера. Карта оставлена в руке");return out;
        }
        if(stage==Stage::ReturnMap){if(currentMap<0||size_t(currentMap)>=container.size()){pause("Неверный исходный слот карты");return out;}
            if(slots.player[hand].item.id!=mapId || mapUuid(slots.player[hand])!=std::optional<int64_t>(templateMaps.at(currentMap))){pause("Карта в руке изменилась — возврат не отправлен");return out;}
            move(out,0,hand,window,currentMap,Stage::Maps,now);if(stage==Stage::Transfer){done.insert(currentMap);++mapsDone;currentMap=-1;}return out;}
        if(stage==Stage::Break){auto b=world({box.x,box.y,box.z});if(!b.known||b.name.find("shulker_box")==std::string::npos){pause("Свой шалкер исчез до разрушения");return out;}
            watch.clear();
            equip(out,c,hand);playerAction(out,c,0);stage=Stage::Digging;nextAt=now+timing.breakMs;status="Ломаю только установленный этим циклом шалкер";return out;}
        if(stage==Stage::Digging){auto b=world({box.x,box.y,box.z});if(!b.known||b.name.find("shulker_box")==std::string::npos){pause("Блок изменился во время разрушения");return out;}
            box.runtime=b.runtime;use(out,c,box,hand,2);playerAction(out,c,2);stage=Stage::BreakWait;deadline=now+timing.timeout;return out;}
        if(stage==Stage::Pickup){if(picked&&pickedSlot>=0){stage=Stage::FindChest;tried.clear();return out;}if(now>=deadline)pause("Шалкер не подобран. Подберите его вручную; очередь не продолжена");return out;}
        if(stage==Stage::FindChest){auto it=std::find_if(chests.begin(),chests.end(),[&](auto& t){return !tried.count(t.key())&&!fullChests.count(t.key());});
            if(it==chests.end()){pause("Все найденные сундуки полны или недоступны. Шалкер остаётся в инвентаре");return out;}
            destination=*it;tried.insert(it->key());use(out,c,destination,hand,0);stage=Stage::OpeningChest;deadline=now+timing.timeout;status="Открываю сундук для готового шалкера";return out;}
        if(stage==Stage::Store){auto empty=std::find_if(container.begin(),container.end(),[](auto& s){return s.known&&!s.item.present();});
            if(empty==container.end()){fullChests.insert(destination.key());fullChests.insert(openedTarget.key());closeTo(Stage::FindChest,now);return out;}
            move(out,0,pickedSlot,window,int(empty-container.begin()),Stage::Next,now);if(stage==Stage::Transfer){transferNext=Stage::Closing;afterClose=Stage::Next;closeSent=clientClosed=serverClosed=false;nextAt=now+timing.store;}return out;}
        if(stage==Stage::Next){++index;++stored;prepared={};templateMaps.clear();watch.clear();currentMap=-1;pickedSlot=-1;equippedMap=false;
            stage=index<total?Stage::NeedTemplate:Stage::Idle;nextAt=now+timing.next;status=index<total?"Готово. Загружаю следующий NBT":"Очередь карт завершена";return out;}
        return out;
    }
    bool templateDue(uint64_t now)const{return needsTemplate()&&now>=nextAt;}
    static std::optional<int64_t> mapUuid(const Slot& slot){
        if(!slot.wire||slot.wire->size()>4096||slot.item.extraBegin<slot.item.begin)return {};
        auto r=BinaryStream::view(*slot.wire,slot.item.extraBegin-slot.item.begin);
        if(r.readU16LE()!=65535||r.readU8()!=1)return {};
        auto root=BedrockNbtCodec::read(r,BedrockNbtEncoding::LittleEndian,{8,2048,128,256});
        auto* id=root.root.find("map_uuid");if(!id||id->type!=NbtTagType::Long)return {};return id->integerValue;
    }
private:
    Stage afterClose=Stage::Idle,transferNext=Stage::Idle;
    Camera origin;Target destination;
    int hand=-1,window=-1,windowType=0,resultSlot=-1,currentMap=-1,pickedSlot=-1;
    bool ready=false,closeSent=false,clientClosed=false,serverClosed=false,equippedMap=false,mapData=false,picked=false,stopRequested=false;
    uint64_t nextAt=0,deadline=0,holdAt=0,dropId=0;
    std::vector<Slot> container;std::vector<Packet> queued;std::set<int> done;
    std::set<AutoCraftStore::Key> tried,fullChests;
    std::map<std::pair<int,int>,ShulkerDeposit::Item> watch;
    Packet slotPacket(int id,int i,const Slot& s)const{return {"inventory_slot",ShulkerDeposit::slotPayload(id,uint8_t(i),s.wire.get(),{},255,slots.modern),true};}
    static Packet closePacket(int id,int type){ProtoDefWriter w;w.u8(id);w.u8(type);w.boolValue(true);return {"container_close",w.take(),true};}
    void closeTo(Stage next,uint64_t now){afterClose=next;stage=Stage::Closing;nextAt=now;closeSent=clientClosed=serverClosed=false;}
    void locatePicked(){if(!picked)return;for(int i=0;i<36;++i)if(slots.player[i].known&&slots.player[i].item.id==shulkerId&&slots.player[i].item.count==1&&slots.player[i].wire){
        // Craft requires no pre-existing shulkers; pickup is also matched to the dropped entity.
        pickedSlot=i;break;}}
    void move(std::vector<Packet>& out,int from,int source,int to,int dest,Stage next,uint64_t now){
        auto& a=from==0?slots.player.at(source):container.at(source);auto& b=to==0?slots.player.at(dest):container.at(dest);
        if(!a.known||!a.item.present()||!a.wire||!b.known||b.item.present()){pause("Слоты переноса не совпали. Ничего не отправлено");return;}
        ProtoDefWriter w;w.zigzag32(0);w.varuint32(0);w.varuint32(2);
        w.varuint32(0);w.zigzag32(from);w.varuint32(source);w.bytes(*a.wire);w.zigzag32(0);
        w.varuint32(0);w.zigzag32(to);w.varuint32(dest);w.zigzag32(0);w.bytes(*a.wire);
        out.push_back({"inventory_transaction",w.take(),false});b=a;a=Slot{{},{},true};
        watch[{from,source}]=a.item;watch[{to,dest}]=b.item;
        out.push_back(slotPacket(from,source,a));out.push_back(slotPacket(to,dest,b));
        stage=Stage::Transfer;transferNext=next;nextAt=now+timing.transfer;status="Перенос: жду сервер";
    }
    void equip(std::vector<Packet>& out,Camera c,int slot){ProtoDefWriter w;w.varuint64(c.runtime);w.bytes(AutoCraftStore::wire(slots.player.at(slot)));w.u8(slot);w.u8(slot);w.u8(0);
        auto payload=w.take();out.push_back({"mob_equipment",payload,false});out.push_back({"mob_equipment",std::move(payload),true});
        ProtoDefWriter select;select.varuint32(slot);select.u8(0);select.boolValue(true);out.push_back({"player_hotbar",select.take(),true});}
    void use(std::vector<Packet>& out,Camera c,Target t,int slot,int action){equip(out,c,slot);ProtoDefWriter w;
        w.zigzag32(0);w.varuint32(2);w.varuint32(0);w.varuint32(action);if(slots.modern)w.varuint32(1);
        w.zigzag32(t.x);w.varuint32(uint32_t(t.y));w.zigzag32(t.z);w.zigzag32(t.face);w.zigzag32(slot);w.bytes(AutoCraftStore::wire(slots.player.at(slot)));
        w.f32le(c.x);w.f32le(c.y);w.f32le(c.z);w.f32le(.5);w.f32le(1);w.f32le(.5);w.varuint32(t.runtime);if(slots.modern)w.varuint32(1);
        out.push_back({"inventory_transaction",w.take(),false});}
    void playerAction(std::vector<Packet>& out,Camera c,int action){ProtoDefWriter w;w.varuint64(c.runtime);w.zigzag32(action);
        for(int i=0;i<2;++i){w.zigzag32(box.x);w.varuint32(box.y);w.zigzag32(box.z);}w.zigzag32(1);out.push_back({"player_action",w.take(),false});}
};
}
