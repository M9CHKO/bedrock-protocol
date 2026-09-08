// RelayState members; shared JNI / Windows adapter. ZIP/NBT loading happens on
// snapshot/command workers, never in the receive callback.
mutable std::mutex mapQueueMutex;
std::shared_ptr<const bedrock::MapArchive> mapArchive;
bedrock::MapShulkerQueue mapQueue;
uint64_t mapArchiveRevision=0;
bool mapLoading=false;
int mapScreenWindow=-1;
std::vector<bedrock::VersionedGamePacket> mapClientPackets;
bedrock::AutoCraftStore::Target mapCloseTarget{};
bool mapBusy()const{std::lock_guard lock(mapQueueMutex);return mapQueue.reserved();}
bedrock::JsRuntimeValue mapSnapshotLocked()const{
    using J=bedrock::JsRuntimeValue;
    return J::object({{"busy",J::boolean(mapQueue.reserved())},{"running",J::boolean(mapQueue.busy())},
        {"status",J::string(mapQueue.status)},{"file",J::string(mapQueue.name)},
        {"total",J::number(mapQueue.total)},{"completed",J::number(mapQueue.index)},
        {"maps",J::number(mapQueue.mapsDone)},{"loaded",J::boolean(mapArchive && !mapArchive->entries.empty())}});
}
void loadNextMapTemplate(){
    std::shared_ptr<const bedrock::MapArchive> archive;size_t index;uint64_t revision;int id,runtime;
    {std::lock_guard lock(mapQueueMutex);if(mapLoading||!mapQueue.templateDue(steadyMilliseconds()))return;
        mapLoading=true;archive=mapArchive;index=mapQueue.index;revision=mapArchiveRevision;id=mapQueue.shulkerId;runtime=mapQueue.shulkerRuntime;}
    try {
        if(!archive)throw std::runtime_error("Сначала загрузите ZIP");
        auto root=QazaqNbtTransferReader(archive->load(index)).readRoot();
        auto* name=root.find("Name");auto* tag=root.find("tag");
        if(!name||name->type!=bedrock::NbtTagType::String||!isShulkerName(name->stringValue)||!tag||tag->type!=bedrock::NbtTagType::Compound)
            throw std::runtime_error("В файле нет шалкера QZNBTF02");
        const auto* items=tag->find("Items");
        if(!items||items->type!=bedrock::NbtTagType::List||items->listValue.empty()||items->listValue.size()>27)
            throw std::runtime_error("Нужен шалкер с картами непосредственно в его ячейках");
        std::map<int,int64_t> maps;
        for(auto& item:items->listValue){auto* n=item.find("Name");auto* slot=item.find("Slot");auto* count=item.find("Count");auto* t=item.find("tag");
            auto* uuid=t?t->find("map_uuid"):nullptr;
            if(!n||n->stringValue!="minecraft:filled_map"||!slot||slot->integerValue<0||slot->integerValue>26||!count||count->integerValue!=1||!uuid||uuid->type!=bedrock::NbtTagType::Long||!maps.emplace(int(slot->integerValue),uuid->integerValue).second)
                throw std::runtime_error("Каждая занятая ячейка шаблона должна содержать одну filled_map с map_uuid (Long)");
        }
        using V=bedrock::ProtoDefValue;
        auto extra=V::object({{"has_nbt",V::string("true")},{"nbt",V::object({{"version",V::integer(1)},{"nbt",bedrock::nbtValueToProtoDefValue(*tag)}})},
            {"can_place_on",V::array({})},{"can_destroy",V::array({})},{"blocking_tick",V::integer(0)}});
        auto prepared=prepareAutoCraftTemplate(extra,id,runtime);
        std::lock_guard lock(mapQueueMutex);mapLoading=false;
        if(revision==mapArchiveRevision&&mapQueue.needsTemplate()&&mapQueue.index==index)
            mapQueue.loaded(std::move(prepared),std::move(maps),archive->entries.at(index).name,steadyMilliseconds());
    }catch(const std::exception& e){std::lock_guard lock(mapQueueMutex);mapLoading=false;if(revision==mapArchiveRevision&&mapQueue.needsTemplate())mapQueue.pause("Ошибка ZIP/NBT: "+safeMessage(e.what()));}
}
bedrock::JsRuntimeValue mapSnapshot(){loadNextMapTemplate();std::lock_guard lock(mapQueueMutex);return mapSnapshotLocked();}
bedrock::JsRuntimeValue mapCommand(const bedrock::JsRuntimeValue& request){
    const auto op=stringProperty(request,"op");
    if(op=="load"){
        {std::lock_guard lock(mapQueueMutex);if(mapQueue.reserved()||mapLoading)throw std::runtime_error("Остановите карты перед заменой ZIP");}
        auto archive=std::make_shared<bedrock::MapArchive>();archive->open(std::filesystem::u8path(stringProperty(request,"path")));
        std::lock_guard lock(mapQueueMutex);if(mapQueue.reserved()||mapLoading)throw std::runtime_error("Модуль уже запущен");
        mapArchive=std::move(archive);++mapArchiveRevision;mapQueue.configureArchive(mapArchive->entries.size());return mapSnapshotLocked();
    }
    if(op=="start"||op=="toggle"){
        {std::lock_guard lock(mapQueueMutex);if(mapQueue.reserved()){mapQueue.stop(steadyMilliseconds());return mapSnapshotLocked();}}
        bool other=platformBusy()||autoCraftBusy();{std::lock_guard lock(mutex);other=other||areaFillRunning||pendingAutomationRequestId!=0;}
        if(other)throw std::runtime_error("Остановите строительство, Авто 2 и другие автоматизации");
        if(!nbtTestEndpointAllowed())throw std::runtime_error("NBT-крафт доступен только на портах 19132/19133");
        auto c=platformCamera();using Target=bedrock::AutoCraftStore::Target;std::vector<Target> tables,chests;Target placement;bool place=false;
        int px=int(std::floor(c.x)),py=int(std::floor(c.y-1.62+.01)),pz=int(std::floor(c.z));
        for(int x=px-6;x<=px+6;++x)for(int z=pz-6;z<=pz+6;++z)for(int y=py-2;y<=py+3;++y){
            double dx=x+.5-c.x,dy=y+.5-c.y,dz=z+.5-c.z;double distance=std::sqrt(dx*dx+dy*dy+dz*dz);
            if(distance>6)continue;auto b=platformBlock({x,y,z});if(!b.known)continue;
            // Search is six blocks; actual interaction obeys ordinary reach.
            if(distance<=4.25 && (b.name=="chest"||b.name=="trapped_chest"||b.name=="crafting_table")){
                bool clear=true;for(int i=1;i<20;++i){double t=double(i)/20;auto p=platformBlock({int(std::floor(c.x+dx*t)),int(std::floor(c.y+dy*t)),int(std::floor(c.z+dz*t))});
                    if(p.known&&!p.air && !(int(std::floor(c.x+dx*t))==x&&int(std::floor(c.y+dy*t))==y&&int(std::floor(c.z+dz*t))==z)){clear=false;break;}}
                if(clear){Target t{x,y,z,b.runtime,1,b.name!="crafting_table",distance};(t.chest?chests:tables).push_back(t);}}
        }
        for(auto [dx,dz]:{std::pair{1,0},std::pair{-1,0},std::pair{0,1},std::pair{0,-1}}){
            auto b=platformBlock({px+dx,py,pz+dz}),up=platformBlock({px+dx,py+1,pz+dz}),floor=platformBlock({px+dx,py-1,pz+dz});
            if(!place&&b.known&&b.air&&up.known&&up.air&&floor.known&&floor.safeFloor){placement={px+dx,py,pz+dz,0,1,false,1};place=true;}}
        if(tables.empty()||chests.empty())throw std::runtime_error("Подойдите ближе: в радиусе 6 нужны верстак и сундук с доступной гранью в пределах 4.25 блока");
        if(!place)throw std::runtime_error("Рядом нужна свободная ячейка с опорой и местом над шалкером");
        auto less=[](auto& a,auto& b){return a.distance<b.distance;};std::sort(tables.begin(),tables.end(),less);std::sort(chests.begin(),chests.end(),less);
        auto runtime=schematicRuntimeId("minecraft:undyed_shulker_box");
        std::lock_guard lock(mapQueueMutex);
        if(mapScreenWindow>=0)throw std::runtime_error("Закройте текущее окно Minecraft перед стартом карт");
        mapQueue.shulkerRuntime=runtime.value_or(0);
        mapQueue.start(c,tables.front(),std::move(chests),placement,steadyMilliseconds());return mapSnapshotLocked();
    }
    std::lock_guard lock(mapQueueMutex);
    if(op=="stop")mapQueue.stop(steadyMilliseconds());
    if(op=="configure"){
        auto& t=mapQueue.timing;
        for(auto [key,value]:{std::pair{"craft",&t.craft},{"open",&t.open},{"close",&t.close},{"place",&t.place},{"transfer",&t.transfer},{"hold",&t.hold},{"break",&t.breakMs},{"pickup",&t.pickup},{"store",&t.store},{"next",&t.next},{"timeout",&t.timeout}}){
            auto* v=request.get(key);if(v&&v->isNumber()&&std::isfinite(v->numberValue()))*value=int(std::clamp(v->numberValue(),0.0,120000.0));}t.clamp();
    }
    return mapSnapshotLocked();
}
void observeMapQueue(bedrock::BedrockRelayPacketEvent& event,bool serverbound)noexcept{
    try{
        auto& p=event.packet;auto& name=p.name;
        if(!serverbound&&(name=="item_registry"||name=="start_game")){
            std::vector<std::pair<int64_t,std::string>> palette;{std::lock_guard lock(mutex);for(auto& item:itemNames)palette.push_back(item);}
            std::lock_guard lock(mapQueueMutex);mapQueue.slots.setVersion(version);mapQueue.slots.shulkerIds.clear();mapQueue.slots.retainedIngredientIds.clear();
            for(auto& [id,n]:palette){if(n=="minecraft:filled_map")mapQueue.mapId=id;
                if(n=="minecraft:undyed_shulker_box")mapQueue.shulkerId=id;
                if(n=="minecraft:shulker_shell")mapQueue.craft.shellId=id;
                if(n=="minecraft:chest")mapQueue.craft.chestId=id;
                if(isShulkerName(n))mapQueue.slots.shulkerIds.insert(id);}
            mapQueue.slots.retainedIngredientIds={mapQueue.mapId,mapQueue.craft.shellId,mapQueue.craft.chestId};
        }
        std::lock_guard lock(mapQueueMutex);auto& q=mapQueue;auto now=steadyMilliseconds();
        if(!serverbound&&name=="start_game"){bedrock::RelayPacketEvent d(version,event,itemProtocolVariables,true);q.slots.authoritative=d.getBool("server_authoritative_inventory",true);return;}
        if(!serverbound&&name=="change_dimension"){q.resetSession();mapScreenWindow=-1;return;}
        if(!serverbound&&(name=="inventory_content"||name=="inventory_slot")){q.inventory(p.payload,name=="inventory_content",now);return;}
        if(name=="container_close"&&!p.payload.empty()){
            if(!serverbound&&(p.payload.front()==mapScreenWindow||p.payload.front()==255))mapScreenWindow=-1;
            q.closed(p.payload.front(),serverbound,now);return;}
        if(!q.busy() && name!="container_open")return;
        bedrock::PacketFieldCursor cursor(p.payload);bedrock::ProtoDefReader r(cursor);
        if(!serverbound&&name=="container_open"){auto id=r.u8(),type=r.u8();int x=r.zigzag32(),y=int(r.varuint32()),z=r.zigzag32();mapScreenWindow=id;q.opened(id,type,{x,y,z},now);return;}
        if(!serverbound&&(name=="update_block"||name=="update_block_synced")){int x=r.zigzag32(),y=int(r.varuint32()),z=r.zigzag32();auto rt=r.varuint32();r.varuint32();if(r.varuint32()!=0)return;
            auto b=platformBlock({x,y,z});q.updateBlock({x,y,z},b.known&&b.air,b.known&&b.runtime==int(rt)&&b.name.find("shulker_box")!=std::string::npos,now);return;}
        if(!serverbound&&name=="add_item_entity"){r.zigzag64();auto id=r.varuint64();auto item=bedrock::ShulkerDeposit::readItem(r);double x=r.readF32LE(),y=r.readF32LE(),z=r.readF32LE();q.dropped(id,item.id,x,y,z);return;}
        if(!serverbound&&name=="take_item_entity"){auto id=r.varuint64();auto target=r.varuint64();q.taken(id,target);return;}
        if(!serverbound&&name=="clientbound_map_item_data"){auto id=r.zigzag64();auto flags=r.varuint32();q.mapUpdate(id,(flags&2)!=0);return;}
        if(serverbound&&(name=="inventory_transaction"||name=="item_stack_request")){
            if(name=="item_stack_request")q.manual();else{auto legacy=r.zigzag32();if(legacy)q.manual();else{auto type=r.varuint32(),actions=r.varuint32();if(actions||type>=2)q.manual();}}return;}
    }catch(const std::exception& e){std::lock_guard lock(mapQueueMutex);if(mapQueue.busy())mapQueue.pause("Карты: "+safeMessage(e.what()));}
}
bool injectMapQueue(bedrock::BedrockRelayPacketEvent& event)noexcept{
    if(event.canceled||!event.replacements.empty())return false;
    try{
        std::lock_guard lock(mapQueueMutex);if(!mapQueue.reserved())return false;
        if(event.packet.name!="player_auth_input"&&event.packet.name!="move_player")return true;
        if(event.packet.name=="player_auth_input"){
            bedrock::RelayPacketEvent d(version,event,itemProtocolVariables,true);
            if(d.getBool("input_data.item_interact",false)||d.getBool("input_data.block_action",false)||d.getBool("input_data.item_stack_request",false)){mapQueue.manual();return true;}}
        auto output=mapQueue.poll(platformCamera(),[this](auto p){return platformBlock(p);},steadyMilliseconds());
        if(output.empty())return true;
        auto codec=bedrock::VersionedMcpeCodec::forVersion(version);std::vector<bedrock::VersionedGamePacket> packets{event.packet};
        for(auto& p:output){auto encoded=codec.packetCodec().makePacketByName(p.name,p.payload);if(p.client)mapClientPackets.push_back(std::move(encoded));else packets.push_back(std::move(encoded));}
        mapCloseTarget=mapQueue.openedTarget;event.replace(std::move(packets));return true;
    }catch(const std::exception& e){std::lock_guard lock(mapQueueMutex);mapQueue.pause("Ошибка кодирования карт: "+safeMessage(e.what()));return true;}
}
