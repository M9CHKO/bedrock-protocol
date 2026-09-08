// Members of RelayState. Both JNI and the Windows C ABI include this adapter;
// all construction/route/inventory decisions live in the public C++ module.
mutable std::mutex platformMutex;
bedrock::PlatformBuilder platformBuilder;
std::filesystem::path platformStorePath;
uint64_t platformSavedRevision=0;
std::vector<bedrock::VersionedGamePacket> platformClientPackets;
bedrock::AutoCraftStore::Target platformWindowTarget{};

bedrock::PlatformBuilder::Camera platformCamera() const {
    auto c=entityPositions.cameraSnapshot();
    return {c.x,c.y,c.z,c.yaw,c.pitch,c.runtimeId,c.inputTick,c.known};
}
bedrock::PlatformBuilder::Block platformBlock(bedrock::platform::Pos p) const {
    auto s=worldBlockSample(p.x,p.y,p.z);
    bedrock::PlatformBuilder::Block b; b.known=s.known;b.air=s.air;b.runtime=s.runtimeId;
    if(!s.known) return b;
    if(s.air) {b.name="air";return b;}
    std::lock_guard lock(blockRegistryMutex);
    const auto* block=blockRegistry?blockRegistry->blockByRuntimeId(s.runtimeId):nullptr;
    if(block) b.name=block->name;
    if(b.name.starts_with("minecraft:")) b.name.erase(0,10);
    b.safeFloor=b.name=="quartz_block" || b.name=="glowstone" || b.name=="stone" ||
        b.name=="cobblestone" || b.name=="dirt" || b.name=="grass" || b.name=="end_stone";
    return b;
}
bool platformBusy() const { std::lock_guard lock(platformMutex);return platformBuilder.busy(); }

// Called by UI/command workers, not the packet receive hook. Bounded text-only
// database, isolated by server, port and dimension; never stores chest contents.
void platformStorageLocked() {
    uint64_t hash=1469598103934665603ULL;
    for(unsigned char c:destinationHost) {hash^=c;hash*=1099511628211ULL;}
    auto path=nbtTransferDirectory / ("platform-"+std::to_string(hash)+"-"+
        std::to_string(destinationPort)+"-"+std::to_string(miniMapDimension.load())+".txt");
    const auto save=[&](const std::filesystem::path& destination) {
        std::filesystem::create_directories(destination.parent_path());
        auto temporary=destination;temporary+=".tmp";
        {std::ofstream out(temporary,std::ios::trunc);if(!out)throw std::runtime_error("Не удалось сохранить сундуки");
            for(auto p:platformBuilder.supplies)out<<p.x<<' '<<p.y<<' '<<p.z<<'\n';
            out.flush();if(!out)throw std::runtime_error("Ошибка записи списка сундуков");}
        if(std::filesystem::exists(destination)) {auto backup=destination;backup+=".bak";
            std::filesystem::copy_file(destination,backup,std::filesystem::copy_options::overwrite_existing);
            std::filesystem::remove(destination);}
        std::filesystem::rename(temporary,destination);platformSavedRevision=platformBuilder.recordsRevision;
    };
    if(path!=platformStorePath) {
        if(!platformStorePath.empty() && platformSavedRevision!=platformBuilder.recordsRevision)save(platformStorePath);
        platformBuilder.supplies.clear();platformStorePath=path;
        if(std::filesystem::exists(path) && std::filesystem::file_size(path)<=16384) {
            std::ifstream in(path);int x,y,z;
            while(in>>x>>y>>z && platformBuilder.supplies.size()<256) {
                if(std::abs(int64_t(x))>30000000 || std::abs(int64_t(z))>30000000 || y< -64 || y>1024) continue;
                bedrock::platform::Pos p{x,y,z};
                if(std::find(platformBuilder.supplies.begin(),platformBuilder.supplies.end(),p)==platformBuilder.supplies.end())
                    platformBuilder.supplies.push_back(p);
            }
        }
        platformSavedRevision=platformBuilder.recordsRevision;
    }
    if(platformSavedRevision==platformBuilder.recordsRevision) return;
    // Keep a recoverable previous database on Windows, whose rename does not
    // replace an existing destination. No world blocks are affected.
    save(path);
}
bedrock::JsRuntimeValue platformSnapshotLocked() const {
    using J=bedrock::JsRuntimeValue;auto chests=J::array();
    for(auto p:platformBuilder.supplies)chests.push(J::object({{"x",J::number(p.x)},{"y",J::number(p.y)},{"z",J::number(p.z)}}));
    return J::object({{"running",J::boolean(platformBuilder.busy())},{"status",J::string(platformBuilder.status)},
        {"placed",J::number(platformBuilder.placed)},{"row",J::number(platformBuilder.rowIndex)},
        {"checked",J::number(platformBuilder.checked)},{"recording",J::boolean(platformBuilder.recording)},
        {"chests",std::move(chests)},{"cacheBytes",J::number(platformBuilder.retainedBytes())}});
}
bedrock::JsRuntimeValue platformSnapshot() {
    std::lock_guard lock(platformMutex);
    try {platformStorageLocked();} catch(const std::exception& e) {platformBuilder.status=safeMessage(e.what());}
    return platformSnapshotLocked();
}
bedrock::JsRuntimeValue platformCommand(const bedrock::JsRuntimeValue& request) {
    const auto op=stringProperty(request,"op");
    bool other=false;
    {std::lock_guard lock(mutex);other=areaFillRunning || pendingAutomationRequestId!=0;}
    other=other || autoCraftBusy() || mapBusy();
    auto camera=platformCamera();
    std::lock_guard lock(platformMutex);
    platformStorageLocked();platformBuilder.version(version);
    const auto integer=[&](const char* name,int fallback) {const auto* v=request.get(name);
        return v && v->isNumber() && std::isfinite(v->numberValue())?int(std::clamp(v->numberValue(),-100000.0,100000.0)):fallback;};
    const auto number=[&](const char* name,double fallback) {const auto* v=request.get(name);return v&&v->isNumber()?v->numberValue():fallback;};
    if(op=="configure") {
        auto& s=platformBuilder.settings;
        s.chunks=integer("chunks",s.chunks);s.side=integer("side",s.side);s.placeMs=integer("placeMs",s.placeMs);
        s.refillMs=integer("refillMs",s.refillMs);s.refillStacks=integer("refillStacks",s.refillStacks);
        s.lanes=integer("lanes",s.lanes);s.gapChunks=integer("gapChunks",s.gapChunks);s.turnSide=integer("turnSide",s.turnSide);
        s.walkSpeed=number("walkSpeed",s.walkSpeed);s.travelSpeed=number("travelSpeed",s.travelSpeed);s.clamp();
    } else if(op=="start" || op=="resume") {
        if(other)platformBuilder.status="Остановите Авто 2 / заполнение перед стройкой";
        else platformBuilder.start(camera,[this](auto p){return platformBlock(p);},steadyMilliseconds(),op=="resume");
    } else if(op=="stop")platformBuilder.stop();
    else if(op=="record")platformBuilder.record();
    else if(op=="remove")platformBuilder.removeSupply(integer("index",-1));
    platformStorageLocked();return platformSnapshotLocked();
}
void observePlatform(bedrock::BedrockRelayPacketEvent& event,bool serverbound) noexcept {
    try {
        auto& packet=event.packet;auto& name=packet.name;
        // Read palette independently before taking the module lock. Avoid the
        // existing main-state -> decoder lock inversion.
        if(!serverbound && (name=="start_game" || name=="item_registry")) {
            std::vector<std::pair<int64_t,std::string>> palette;
            {std::lock_guard lock(mutex);for(auto& pair:itemNames)palette.push_back(pair);}
            std::lock_guard lock(platformMutex);
            if(name=="start_game") {platformBuilder.reset();platformBuilder.version(version);}
            platformBuilder.palette(palette);
        }
        std::lock_guard lock(platformMutex);auto& b=platformBuilder;auto now=steadyMilliseconds();
        if(!serverbound && name=="start_game") {
            bedrock::RelayPacketEvent decoded(version,event,itemProtocolVariables,true);
            b.authoritative=decoded.getBool("server_authoritative_inventory",true);return;
        }
        if(!serverbound && name=="change_dimension") {b.reset();return;}
        if(!serverbound && (name=="inventory_content" || name=="inventory_slot")) {b.inventory(packet.payload,name=="inventory_content",now);return;}
        if(name=="container_close" && !packet.payload.empty()) {b.closed(packet.payload.front(),serverbound,now);return;}
        if(!serverbound && name=="container_open") {
            bedrock::PacketFieldCursor c(packet.payload);bedrock::ProtoDefReader r(c);
            const int id=r.u8(),type=r.u8();const int x=r.zigzag32(),y=int(r.varuint32()),z=r.zigzag32();
            if(type==1) {auto block=platformBlock({x,y,z});platformWindowTarget={x,y,z,block.runtime,1,false,0,now};}
            b.opened(id,type,{x,y,z},now);return;
        }
        if(!serverbound && (name=="update_block" || name=="update_block_synced")) {
            bedrock::PacketFieldCursor c(packet.payload);bedrock::ProtoDefReader r(c);
            const int x=r.zigzag32(),y=int(r.varuint32()),z=r.zigzag32();const auto runtime=r.varuint32();
            r.varuint32();if(r.varuint32()==0)b.blockUpdate({x,y,z},int32_t(runtime),[this](auto p){return platformBlock(p);});return;
        }
        if(serverbound && name=="mob_equipment") {
            bedrock::PacketFieldCursor c(packet.payload);bedrock::ProtoDefReader r(c);
            r.varuint64();bedrock::ShulkerDeposit::readItem(r);r.u8();b.equipped(r.u8());return;
        }
        if(serverbound && (name=="inventory_transaction" || name=="item_stack_request")) {
            if(name=="item_stack_request")b.manualInventory();
            else {bedrock::PacketFieldCursor c(packet.payload);bedrock::ProtoDefReader r(c);
                const auto legacy=r.zigzag32();if(legacy!=0)b.manualInventory();
                else {auto type=r.varuint32(),actions=r.varuint32();if(actions || type>=2)b.manualInventory();}}
            return;
        }
        if(!serverbound && b.busy() && (name=="correct_player_move_prediction" || name=="move_player")) {
            bedrock::RelayPacketEvent decoded(version,event,itemProtocolVariables,true);
            auto c=platformCamera();
            if(name=="move_player" && decoded.getUInt("runtime_id",0)!=c.runtime)return;
            if(name=="move_player" && decoded.getString("mode","")!="reset" && decoded.getString("mode","")!="teleport")return;
            c.x=decoded.getDouble("position.x",c.x);c.y=decoded.getDouble("position.y",c.y);c.z=decoded.getDouble("position.z",c.z);
            b.correction(c,now);
        }
    } catch(const std::exception& e) {std::lock_guard lock(platformMutex);if(platformBuilder.busy())platformBuilder.pause("Ошибка данных: "+safeMessage(e.what()));}
}
bool injectPlatform(bedrock::BedrockRelayPacketEvent& event) noexcept {
    if(event.canceled || !event.replacements.empty() || event.packet.name!="player_auth_input")return false;
    try {
        auto c=platformCamera();std::lock_guard lock(platformMutex);
        bedrock::RelayPacketEvent decoded(version,event,itemProtocolVariables,true);
        if(platformBuilder.busy()) {
            const auto* request=decoded.value("item_stack_request");
            const auto* actions=decoded.value("transaction.actions");
            if((request && request->kind==bedrock::PacketValue::Kind::Object) ||
                (actions && actions->kind==bedrock::PacketValue::Kind::Array && !actions->arrayValue.empty()) ||
                decoded.getBool("input_data.item_interact",false) || decoded.getBool("input_data.block_action",false)) {
                platformBuilder.manualInventory();return true;
            }
        }
        auto output=platformBuilder.poll(c,[this](auto p){return platformBlock(p);},steadyMilliseconds());
        if(!output.control && output.packets.empty())return false;
        auto codec=bedrock::VersionedMcpeCodec::forVersion(version);
        auto movement=event.packet;
        if(output.control) {
            // Pause/options/CPE menus do not disable simulation. Actual inventory
            // actions belong to the player and must not interleave transfers.
            decoded.set("position.x",output.x);decoded.set("position.y",output.y);decoded.set("position.z",output.z);
            decoded.set("delta.x",output.dx);decoded.set("delta.y",0.0);decoded.set("delta.z",output.dz);
            decoded.set("yaw",double(output.yaw));decoded.set("head_yaw",double(output.yaw));
            const double yaw=output.yaw*3.141592653589793/180.0,pitch=c.pitch*3.141592653589793/180.0;
            decoded.set("camera_orientation.x",-std::sin(yaw)*std::cos(pitch));
            decoded.set("camera_orientation.y",-std::sin(pitch));
            decoded.set("camera_orientation.z",std::cos(yaw)*std::cos(pitch));
            decoded.set("interact_rotation.x",double(c.pitch));decoded.set("interact_rotation.z",double(output.yaw));
            auto input=bedrock::area_fill::localInput(output.dx,output.dz,output.yaw);
            for(auto prefix:{"move_vector","analogue_move_vector","raw_move_vector"}) {decoded.set(std::string(prefix)+".x",input.x);decoded.set(std::string(prefix)+".z",input.z);}
            decoded.set("input_data.up",input.z>.01);decoded.set("input_data.down",input.z<-.01);
            decoded.set("input_data.left",input.x>.01);decoded.set("input_data.right",input.x<-.01);
            for(auto bit:{"up_left","up_right","down_left","down_right","jumping","jump_down","start_jumping","sprint_down","start_sprinting","sprinting"})
                decoded.set(std::string("input_data.")+bit,false);
            const bool wasSneaking=decoded.getBool("input_data.sneaking",false);
            decoded.set("input_data.sneaking",output.sneak);decoded.set("input_data.sneak_down",output.sneak);
            decoded.set("input_data.persist_sneak",output.sneak);
            decoded.set("input_data.start_sneaking",output.sneak && !wasSneaking);
            decoded.set("input_data.stop_sneaking",!output.sneak && wasSneaking);
            bedrock::ProtoDefPacketEncoder encoder(version,itemProtocolVariables);
            movement=codec.packetCodec().makePacketByName("player_auth_input",encoder.encodePacket("player_auth_input",bedrock::ProtoDefValue::object(decoded.decodedParams())));
        }
        std::vector<bedrock::VersionedGamePacket> packets{std::move(movement)};
        for(auto& p:output.packets) {
            auto packet=codec.packetCodec().makePacketByName(p.name,p.payload);
            if(p.client)platformClientPackets.push_back(std::move(packet));else packets.push_back(std::move(packet));
        }
        if(output.move) {
            using V=bedrock::ProtoDefValue;
            platformClientPackets.push_back(makeAreaProtocolPacket(version,"move_player",V::object({
                {"runtime_id",V::uinteger(c.runtime)},{"position",areaVec3(output.x,output.y,output.z)},
                {"pitch",V::floating(c.pitch)},{"yaw",V::floating(output.yaw)},{"head_yaw",V::floating(output.yaw)},
                {"mode",V::string("teleport")},{"on_ground",V::boolean(true)},{"ridden_runtime_id",V::uinteger(0)},
                {"teleport",V::object({{"cause",V::string("behavior")},{"source_entity_type",V::string("player")}})},
                {"tick",V::uinteger(c.tick)}})));
        }
        event.replace(std::move(packets));return true;
    } catch(const std::exception& e) {std::lock_guard lock(platformMutex);platformBuilder.pause("Ошибка стройки: "+safeMessage(e.what()));return true;}
}
