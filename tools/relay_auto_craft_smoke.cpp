// Exercises production Auto 2 and protocol encoders without networking/auth.
#include "../android/relay-app/app/src/main/cpp/native_bridge.cpp"
#include <iostream>

using A = bedrock::AutoCraftStore;
using S = bedrock::ShulkerDeposit;
using V = bedrock::ProtoDefValue;
static int checks = 0;
static void require(bool test, const char* what) {
    ++checks; if (!test) throw std::runtime_error(what);
}
static S::Slot item(int id, uint16_t count) {
    if (!id || !count) return {{}, {}, true};
    bedrock::ProtoDefWriter w;
    w.zigzag32(id); w.u16le(count); w.varuint32(0); w.u8(0); w.zigzag32(1);
    w.varuint32(10); w.u16le(0); w.u32le(0); w.u32le(0);
    auto raw = std::make_shared<const A::Wire>(w.take());
    bedrock::PacketFieldCursor c(*raw); bedrock::ProtoDefReader r(c);
    return {S::readItem(r), raw, true};
}
static V templateExtra() {
    using N = bedrock::NbtValue;
    const auto content = N::compound({{"Name",N::string("minecraft:stone")}, {"Count",N::byte(64)}, {"Slot",N::byte(0)}});
    const auto shulker = N::compound({{"Name",N::string("minecraft:shulker_box")}, {"Count",N::byte(1)},
        {"tag",N::compound({{"Items",N::list(bedrock::NbtTagType::Compound,std::vector<N>(27,content))}})}});
    auto root = bedrock::nbtDocumentToProtoDefValue({"",N::compound({
        {"CustomName",N::string("Weathertop Auto 2")},
        {"Items",N::list(bedrock::NbtTagType::Compound,std::vector<N>(27,shulker))},
        {"opaque_test",N::byteArray(std::vector<uint8_t>(128*1024,42))}})});
    bedrock::ProtoDefWriter writer;
    bedrock::writeProtoDefNbt(writer,root,bedrock::BedrockNbtEncoding::LittleEndian);
    return V::object({{"has_nbt",V::string("true")},
        {"nbt",V::object({{"version",V::integer(1)},{"nbt",V::bytes(writer.take())}})},
        {"can_place_on",V::array({})},{"can_destroy",V::array({})}});
}
static void setup(S& slots, A& machine, const std::string& version, int shells = 4, int chests = 2) {
    machine.configureTiming(1000,300);
    slots.setVersion(version); slots.authoritative = false; slots.inventoryReady = true;
    slots.shulkerIds = {777}; slots.retainedIngredientIds = {601, 55};
    for (auto& s : slots.player) s = item(0, 0);
    slots.player[30] = item(601, shells); slots.player[31] = item(55, chests);
    machine.shellId=601; machine.chestId=55; machine.resultId=777; machine.resultRuntime=123;
    RelayState state; state.version=version; state.itemProtocolVariables->setVariable("ShieldItemID",513);
    machine.preparedResult=state.prepareAutoCraftTemplate(templateExtra(),777,123);
    machine.templateName="Weathertop";
}
static std::vector<A::Target> targets() { return {{1,63,1,23,1,false,2}, {2,63,1,25,1,true,3}, {3,63,1,25,1,true,4}}; }
static void openTable(A& m, S& s, uint64_t now) {
    auto open=m.poll(s,now); require(open.kind==A::Action::Open && !open.target.chest,"open workbench");
    require(m.opened(s,1,1,1,63,1,now+1),"confirm workbench");
}
static void completeClose(A& m, S& s, uint32_t id, uint64_t now) {
    m.closed(s,id,now,true);
    m.closed(s,id,now,false);
}
static void chestReady(S& s) {
    s.chest.resize(27); for (auto& slot:s.chest) slot=item(0,0);
    s.chestReady=true;
}
static void verifyTransaction(const std::string& version, const A::Wire& bytes, size_t count) {
    bedrock::BedrockRelayPacketEvent event;
    event.packet=bedrock::VersionedPacketCodec::forVersion(version).makePacketByName("inventory_transaction",bytes);
    auto variables=std::make_shared<bedrock::ProtoDefVariableStore>();
    variables->setVariable("ShieldItemID",513);
    bedrock::RelayPacketEvent decoded(version,event,variables,true,true);
    const auto* transaction=decoded.value("transaction");
    require(transaction && transaction->get("actions") && transaction->get("actions")->arrayValue.size()==count,"valid actions count");
}
static bedrock::BedrockRelayPacketEvent containerOpen(const std::string& version,
    uint8_t id, uint8_t type, int x, int y, int z) {
    // Nukkit's actual workbench response includes windowId=-1 and entityId.
    bedrock::ProtoDefWriter w;
    w.u8(id); w.u8(type); w.zigzag32(x); w.varuint32(y); w.zigzag32(z); w.zigzag64(123);
    bedrock::BedrockRelayPacketEvent event;
    event.packet=bedrock::VersionedPacketCodec::forVersion(version).makePacketByName("container_open",w.take());
    return event;
}
static void verifyNativeIntegration() {
    RelayState state;
    state.version="1.21.100";
    state.loadBlockRegistry("data/minecraft-data/bedrock/1.21.100");
    state.configureBlockRuntimeIds(false);
    const auto air=state.actualAirRuntimeId();
    const auto table=*state.schematicRuntimeId("minecraft:crafting_table");
    const auto chest=*state.schematicRuntimeId("minecraft:chest");
    const auto stone=*state.schematicRuntimeId("minecraft:stone");
    bedrock::ProtoDefWriter w;
    w.varuint64(123); w.f32le(0.5); w.f32le(64.62); w.f32le(0.5);
    w.f32le(0); w.f32le(0); w.f32le(0);
    auto movement=bedrock::VersionedPacketCodec::forVersion("1.21.100").makePacketByName("move_player",w.take());
    state.entityPositions.observeServerbound(movement);
    for(int x=-4;x<=4;++x) for(int y=60;y<=68;++y) for(int z=-4;z<=4;++z)
        state.schematicBlockOverrides[{0,x,y,z}]={air,0,0};
    state.schematicBlockOverrides[{0,2,63,0}]={table,0,0};
    state.schematicBlockOverrides[{0,-2,63,0}]={chest,0,0};
    auto nearby=state.autoCraftNearbyTargets();
    require(nearby.size()==2,"find exposed table and chest from actual block registry");
    for(int y=60;y<=68;++y) for(int z=-4;z<=4;++z)
        state.schematicBlockOverrides[{0,1,y,z}]={stone,0,0};
    const auto blocked=state.autoCraftNearbyTargets();
    require(blocked.size()==1 && blocked.front().chest,"wall excludes unreachable table");
    for(int y=60;y<=68;++y) for(int z=-4;z<=4;++z)
        state.schematicBlockOverrides[{0,1,y,z}]={air,0,0};
    for(const auto& version:{std::string("1.21.2"),std::string("1.21.100")}) {
        state.version=version; state.itemProtocolVariables->setVariable("ShieldItemID",513);
        auto& slots=state.shulkerDeposit; auto& m=state.autoCraftStore;
        setup(slots,m,version);
        m.start(slots,nearby,0.5,64.62,0.5,steadyMilliseconds());
        bedrock::BedrockRelayPacketEvent event; event.packet=movement; event.canceled=true;
        state.maybeInjectAutoCraft(version,event);
        require(event.replacements.empty() && m.stage==A::Stage::Table,"canceled movement cannot trigger automation");
        event.canceled=false; state.maybeInjectAutoCraft(version,event);
        require(event.replacements.size()==3,"production emits movement, equipment, block click");
        bedrock::BedrockRelayPacketEvent click; click.packet=event.replacements.back();
        bedrock::RelayPacketEvent decoded(version,click,state.itemProtocolVariables,true,true);
        require(decoded.value("transaction.actions")->arrayValue.empty(),"opening spends no inventory items");
        require(decoded.getInt("transaction.transaction_data.block_position.x",-99)==2,"opens actual target coordinates");
        require(decoded.getInt("transaction.transaction_data.block_runtime_id",-99)==table,"uses actual server block runtime");
        m.stop(slots,"stop",steadyMilliseconds());
        auto response=containerOpen(version,255,1,2,63,0);
        state.observeDecodedGameplayPacket(version,response,false);
        require(m.window==255 && m.stage==A::Stage::Closing,"native accepts late Nukkit workbench after stop");
        event.replacements.clear(); state.maybeInjectAutoCraft(version,event);
        require(event.replacements.empty() && state.autoCraftClientClose.has_value(),
            "production sends late workbench close to Minecraft, not upstream");
        bedrock::BedrockRelayPacketEvent close; close.packet=*state.autoCraftClientClose;
        bedrock::RelayPacketEvent closeDecoded(version,close,state.itemProtocolVariables,true,true);
        require(close.packet.payload.front()==255 && closeDecoded.getString("window_id","")=="none" &&
            closeDecoded.getBool("server",false),"client close request preserves ID and is server initiated");
        const auto closePlan=state.autoCraftClosePackets(close.packet,m.target);
        require(closePlan.restoreTable && closePlan.packets.size()==2 && closePlan.packets[0].name=="container_close",
            "initial workbench close sends air without same-batch restoration");
        auto closeBundle=closePlan.packets;
        closeBundle.push_back(state.autoCraftRestorePacket(m.target));
        for(size_t i=1;i<closeBundle.size();++i) {
            bedrock::BedrockRelayPacketEvent visual; visual.packet=closeBundle[i];
            bedrock::RelayPacketEvent block(version,visual,{},true,true);
            require(visual.packet.name=="update_block" && block.getInt("position.x",-99)==2 &&
                block.getInt("position.y",-99)==63 && block.getInt("position.z",-99)==0 &&
                block.getInt("block_runtime_id",-99)==(i==1 ? air : table) && block.getInt("layer",-99)==0,
                "visual workbench refresh uses exact position and registry runtimes");
        }
        require(state.worldBlockSample(2,63,0).runtimeId==table,
            "closing refresh never changes the stored world");
        auto chestClose=close.packet; chestClose.payload[0]=7; chestClose.payload[1]=0;
        const auto chestPlan=state.autoCraftClosePackets(chestClose,m.target);
        require(chestPlan.packets.size()==1 && !chestPlan.restoreTable,
            "physical chest closure never changes its visual block or contents");
        // Exact four-byte packets from the user's manual workbench log.
        // Client sends NONE (-9), server replies CONTAINER (0), not WORKBENCH.
        // Their different types/directions are NOT interchangeable requests.
        bedrock::BedrockRelayPacketEvent clientClose, serverClose;
        clientClose.packet=bedrock::VersionedPacketCodec::forVersion(version).makePacketByName("container_close",{255,247,0});
        serverClose.packet=bedrock::VersionedPacketCodec::forVersion(version).makePacketByName("container_close",{255,0,0});
        require(packetHash(clientClose.packet.fullPacket)==0xd5699ef89b836ea6ULL &&
            packetHash(serverClose.packet.fullPacket)==0xd74200f89d14c5e9ULL,
            "manual close fixtures match user log hashes exactly");
        bedrock::RelayPacketEvent clientDecoded(version,clientClose,{},true,true);
        require(clientDecoded.getString("window_type","")=="none" && !clientDecoded.getBool("server",true),
            "manual client closure decodes NONE type with client-initiated flag");
        state.autoCraftTableRestore=RelayState::AutoCraftTableRestore{m.target,"test",10500,1,false};
        require(!state.autoCraftTableRestore->ready(10000) && !state.autoCraftTableRestore->ready(10499) &&
            state.autoCraftTableRestore->ready(10500),"restore waits 500 ms without blocking any thread");
        state.observeDecodedGameplayPacket(version,serverClose,false);
        require(m.stage==A::Stage::Closing && !m.clientClosed && state.minecraftUiBlocked.load(),
            "server echo alone cannot acknowledge a still-open Minecraft GUI");
        require(!state.autoCraftTableRestore->ready(10001),"server echo cannot shorten the visual refresh delay");
        state.observeDecodedGameplayPacket(version,clientClose,true);
        require(!m.busy() && !state.minecraftUiBlocked.load(),"real client close plus server echo finishes stop");
        require(state.autoCraftTableRestore->ready(10002) && state.autoCraftBusy(),
            "genuine client close allows early restoration but retains automation ownership until queued");
        state.schematicBlockOverrides[{0,2,63,0}]={stone,0,0};
        auto restoreEvent=bedrock::BedrockRelayPacketEvent{};
        restoreEvent.packet=state.autoCraftRestorePacket(state.autoCraftTableRestore->target);
        bedrock::RelayPacketEvent latestBlock(version,restoreEvent,{},true,true);
        require(latestBlock.getInt("block_runtime_id",-99)==stone,"delayed restoration uses latest known server block");
        state.schematicBlockOverrides[{0,2,63,0}]={table,0,0};
        state.autoCraftTableRestore.reset();
        m.reset(slots);

        setup(slots,m,version); m.start(slots,nearby,0.5,64.62,0.5,steadyMilliseconds());
        m.poll(slots,steadyMilliseconds());
        state.observeDecodedGameplayPacket(version,response,false);
        require(m.running && m.stage==A::Stage::Craft && m.window==255,"native workbench response advances to craft");
        state.autoCraftTableRestore=RelayState::AutoCraftTableRestore{m.target,"test",0,2,true};
        event.replacements.clear(); state.maybeInjectAutoCraft(version,event);
        require(event.replacements.empty() && m.stage==A::Stage::Craft,
            "no new automated action may overtake an unqueued restoration");
        state.autoCraftTableRestore.reset();
        const auto nextAt=m.nextAt;
        state.observeDecodedGameplayPacket(version,response,false);
        require(m.nextAt==nextAt && m.stage==A::Stage::Craft,"duplicate native open is idempotent");
        require(m.poll(slots,nextAt).kind==A::Action::CraftOne && slots.player[0].wire->size()>128*1024,
            "real Nukkit window enables craft with selected opaque NBT");
        for(uint8_t type : {uint8_t(0),uint8_t(1)}) {
            bedrock::BedrockRelayPacketEvent sync;
            sync.packet=bedrock::VersionedPacketCodec::forVersion(version).makePacketByName(
                "inventory_transaction",{0,type,0});
            state.observeDecodedGameplayPacket(version,sync,true);
            require(m.running && m.stage==A::Stage::Craft && !sync.canceled && sync.replacements.empty(),
                "empty normal/mismatch request is forwarded without a false manual stop");
        }
        // A real move with two opaque descriptors must still stop and close.
        S::Plan manual; manual.window=7; manual.item=item(777,1); manual.source=0; manual.destination=0;
        auto manualPacket=bedrock::VersionedPacketCodec::forVersion(version).makePacketByName(
            "inventory_transaction",S::legacyPayload(manual));
        state.observeDepositPacket(manualPacket,true);
        require(!m.running && m.stage==A::Stage::Closing && m.window==255 &&
            m.status.find("Ручное действие")!=std::string::npos,
            "real manual item move still stops and retains GUI ownership for closure");
        m.closed(slots,255,nextAt+1,true);
        require(!m.busy() && !m.running,"manual close recognizes wire 255");
        m.reset(slots);

        // Real outgoing item_use, then matching server open: no chunk required.
        state.resetMiniMapWorld(0);
        state.observeDecodedGameplayPacket(version,click,true);
        require(state.autoCraftKnownTargets.empty(),"manual click alone is not proof of an open");
        state.observeDecodedGameplayPacket(version,response,false);
        auto remembered=state.autoCraftNearbyTargets();
        require(remembered.size()==1 && remembered[0].x==2 && !remembered[0].chest,
            "matching real response remembers manually opened table without terrain cache");
        require(remembered[0].face==decoded.getInt("transaction.transaction_data.face",-1),"clicked face preserved");
        state.recordDepositClick({-2,63,0,static_cast<uint32_t>(chest),1});
        auto chestOpen=containerOpen(version,7,0,-2,63,0);
        state.observeDecodedGameplayPacket(version,chestOpen,false);
        require(state.autoCraftNearbyTargets().size()==2,"manual chest confirmation completes fallback targets");
        state.recordDepositClick({3,63,0,static_cast<uint32_t>(table),1});
        auto wrong=containerOpen(version,255,1,4,63,0);
        state.observeDecodedGameplayPacket(version,wrong,false);
        require(state.autoCraftKnownTargets.size()==2 && !state.autoCraftClickedTarget,"unrelated response is not remembered");
        for(int y=60;y<=68;++y) for(int z=-4;z<=4;++z)
            state.schematicBlockOverrides[{0,1,y,z}]={stone,0,0};
        require(state.autoCraftNearbyTargets().size()==1,"remembered target cannot bypass a known wall");
        state.resetMiniMapWorld(0);
        require(state.autoCraftKnownTargets.empty() && state.autoCraftNearbyTargets().empty(),"world reset clears remembered targets");
        // Restore world samples for the next protocol iteration.
        for(int x=-4;x<=4;++x) for(int y=60;y<=68;++y) for(int z=-4;z<=4;++z)
            state.schematicBlockOverrides[{0,x,y,z}]={air,0,0};
        state.schematicBlockOverrides[{0,2,63,0}]={table,0,0};
        state.schematicBlockOverrides[{0,-2,63,0}]={chest,0,0};
    }
    for(int i=0;i<40;++i) state.rememberAutoCraftTarget({i,63,0,table,1,false,0});
    require(state.autoCraftKnownTargets.size()==32,"manual target memory bounded to 32 coordinates");
    state.autoCraftKnownTargets.front().confirmedAt=steadyMilliseconds()-A::TargetLifetimeMs-1;
    require(!state.rememberedAutoCraftTarget(state.autoCraftKnownTargets.front()),"remembered targets expire");

    // Drive the production queue deterministically without its worker thread.
    { std::lock_guard lock(state.miniMapMutex); state.miniMapStopping=true; }
    state.miniMapCondition.notify_all(); state.miniMapWorker.join();
    state.miniMapStopping=false; state.resetMiniMapWorld(0);
    state.autoCraftWorldTracking=true; state.schematicWorldTrackingActive=true;
    state.schematicEnabled=false; state.areaFillEnabled=false; state.miniMapEnabled=false;
    bedrock::BedrockSubChunkPacket sub;
    sub.originX=3; sub.originY=4;
    bedrock::BedrockSubChunkPacketEntry entry;
    entry.dx=-3; entry.result=bedrock::BedrockSubChunkResult::SuccessAllAir;
    sub.entries.push_back(entry); entry.dx=0; sub.entries.push_back(entry);
    auto subPacket=bedrock::VersionedPacketCodec::forVersion("1.21.100").makePacketByName("subchunk",
        bedrock::BedrockSubChunkPacketCodec::encodePacketPayload(sub,"1.21.100"));
    state.enqueueMiniMapChunk("1.21.100",subPacket);
    require(state.miniMapJobs.size()==1,"far subchunk origin with nearby relative entries is queued");
    auto job=std::move(state.miniMapJobs.front()); state.miniMapJobs.pop_front();
    state.cacheSchematicSubChunkJob(job);
    require(state.worldBlockSample(0,64,0).known && state.worldBlockSample(0,64,0).air,
        "nearby relative air section becomes available to target line of sight");
    require(!state.worldBlockSample(48,64,0).known,"far relative section is not cached for Auto 2");
    state.autoCraftTableRestore=RelayState::AutoCraftTableRestore{{},"old-session",0,3,false};
    bedrock::VersionedGamePacket dimension; dimension.name="change_dimension";
    state.observeDepositPacket(dimension,false);
    require(!state.autoCraftTableRestore,"dimension transition drops old-world delayed restoration");
    state.autoCraftTableRestore=RelayState::AutoCraftTableRestore{{},"old-session",0,4,false};
    state.clearGameplayTelemetry();
    require(!state.autoCraftTableRestore,"disconnect clears delayed restoration without carrying it into a new session");
}
static void verifyPlatformIntegration() {
    for(const auto& version:{std::string("1.21.2"),std::string("1.21.100")}) {
        RelayState state;state.version=version;state.loadBlockRegistry("data/minecraft-data/bedrock/1.21.100");state.configureBlockRuntimeIds(false);
        state.itemProtocolVariables->setVariable("ShieldItemID",513);
        const auto air=state.actualAirRuntimeId(),quartz=*state.schematicRuntimeId("minecraft:quartz_block"),glow=*state.schematicRuntimeId("minecraft:glowstone");
        for(int x=-4;x<=4;++x)for(int y=61;y<=67;++y)for(int z=-4;z<=4;++z)
            state.schematicBlockOverrides[{0,x,y,z}]={y==62?(x==0?glow:quartz):air,0,0};
        bedrock::ProtoDefWriter seed;seed.varuint64(123);seed.f32le(.5f);seed.f32le(64.62f);seed.f32le(.5f);seed.f32le(0);seed.f32le(0);seed.f32le(0);
        auto codec=bedrock::VersionedPacketCodec::forVersion(version);
        state.entityPositions.observeServerbound(codec.makePacketByName("move_player",seed.take()));
        auto& b=state.platformBuilder;b.version(version);b.palette({{1,"quartz_block"},{2,"glowstone"},{3,"chest"},{4,"crafting_table"}});
        bedrock::ProtoDefWriter inv;inv.varuint32(0);inv.varuint32(36);
        for(int i=0;i<36;++i) {if(i<4)inv.bytes(*item(i+1,64).wire);else inv.zigzag32(0);}
        if(b.modern){inv.u8(12);inv.u8(0);inv.zigzag32(0);}b.inventory(inv.take(),true,0);
        b.start(state.platformCamera(),[&](auto p){return state.platformBlock(p);},steadyMilliseconds());
        require(b.busy(),"platform starts with native camera/world");
        auto vec2=[](double x,double z){return V::object({{"x",V::floating(x)},{"z",V::floating(z)}});};
        auto input=V::object({{"pitch",V::floating(0)},{"yaw",V::floating(0)},{"head_yaw",V::floating(0)},
            {"position",RelayState::areaVec3(.5,64.62,.5)},{"move_vector",vec2(0,0)},{"input_data",V::object({
                {"item_interact",V::boolean(false)},{"block_action",V::boolean(false)},{"item_stack_request",V::boolean(false)},{"client_predicted_vehicle",V::boolean(false)}})},
            {"input_mode",V::string("mouse")},{"play_mode",V::string("normal")},{"interaction_model",V::string("classic")},
            {"interact_rotation",vec2(0,0)},{"tick",V::uinteger(100)},{"delta",RelayState::areaVec3(0,0,0)},
            {"analogue_move_vector",vec2(0,0)},{"camera_orientation",RelayState::areaVec3(-1,0,0)},{"raw_move_vector",vec2(0,0)}});
        bedrock::ProtoDefPacketEncoder encoder(version,state.itemProtocolVariables);
        const auto packet=codec.makePacketByName("player_auth_input",encoder.encodePacket("player_auth_input",input));
        bool moved=false;
        for(int i=0;i<10 && !moved;++i) {
            bedrock::BedrockRelayPacketEvent event;event.packet=packet;
            require(state.injectPlatform(event),"platform owns movement while menus may be open");
            require(!event.replacements.empty(),"production movement encoded");
            for(auto& p:event.replacements)bedrock::ProtoDefPacketDecoder(version,state.itemProtocolVariables).validatePacketStrict(p.name,p.payload);
            for(auto& p:state.platformClientPackets) {
                bedrock::ProtoDefPacketDecoder(version,state.itemProtocolVariables).validatePacketStrict(p.name,p.payload);
                if(p.name=="move_player")moved=true;
            }
            state.platformClientPackets.clear();
        }
        require(moved && b.busy(),"native walking emits valid local camera sync in both protocols");
        input.objectValue["input_data"].objectValue["block_action"]=V::boolean(true);
        input.objectValue["block_action"]=V::array({});
        bedrock::BedrockRelayPacketEvent manual;manual.packet=codec.makePacketByName("player_auth_input",encoder.encodePacket("player_auth_input",input));
        state.injectPlatform(manual);require(!b.busy() && manual.replacements.empty(),"manual action detected before automation poll/send");
    }
}
int main() {
    try {
        for (const auto& version : {std::string("1.21.2"),std::string("1.21.100")}) {
            S s; A m; setup(s,m,version);
            const auto frozen=m.preparedResult.wire;
            m.start(s,targets(),0,64,0,1000); require(m.running,"starts"); openTable(m,s,1000);
            m.preparedResult=item(777,1); // A later selection cannot mix a running batch.
            require(m.poll(s,1200).kind==A::Action::None,"opening grace");
            auto craft=m.poll(s,1500); require(craft.kind==A::Action::CraftOne,"craft emitted");
            require(craft.transactions.size()==2,"fill and result pair");
            verifyTransaction(version,craft.transactions[0],6); verifyTransaction(version,craft.transactions[1],2);
            require(s.player[30].item.count==2 && s.player[31].item.count==1,"consume two shells and chest");
            require(s.player[0].item.id==777 && s.player[0].item.count==1,"NBT result placed");
            require(s.player[0].wire==frozen && frozen->size()>128*1024,"frozen opaque template shared, not reparsed");
            bedrock::BedrockRelayPacketEvent resultEvent;
            resultEvent.packet=bedrock::VersionedPacketCodec::forVersion(version).makePacketByName("inventory_transaction",craft.transactions[1]);
            auto variables=bedrock::makeProtoDefVariableStore(); variables->setVariable("ShieldItemID",513);
            bedrock::RelayPacketEvent resultDecoded(version,resultEvent,variables,true,true);
            const auto* resultActions=resultDecoded.value("transaction.actions");
            const auto* resultNbt=resultActions->arrayValue[0].get("old_item")->get("extra")->get("nbt")->get("nbt");
            const auto* destNbt=resultActions->arrayValue[1].get("new_item")->get("extra")->get("nbt")->get("nbt");
            require(resultNbt->bytesValue==templateExtra().get("nbt")->get("nbt")->bytesValue,"template preserved in source");
            require(destNbt->bytesValue==resultNbt->bytesValue,"identical NBT on both sides");
            require(m.poll(s,1600).kind==A::Action::None,"no catchup burst");
            require(m.poll(s,2500).kind==A::Action::CraftOne,"continuous second craft");
            require(s.player[1].wire==frozen,"same selected template on all crafts");
            require(m.poll(s,3500).kind==A::Action::Close,"materials exhausted closes table");
            require(m.finishing,"final batch must unload"); completeClose(m,s,1,3600);
            auto open=m.poll(s,3900); require(open.kind==A::Action::Open && open.target.chest,"opens nearest chest");
            m.opened(s,2,0,2,63,1,3901); chestReady(s);
            auto move=m.poll(s,4400); require(move.kind==A::Action::DepositOne,"deposit first");
            verifyTransaction(version,S::legacyPayload(*move.deposit),2);
            require(m.poll(s,4401).kind==A::Action::None,"deposit interval");
            require(m.poll(s,5400).kind==A::Action::DepositOne,"deposit second");
            m.poll(s,6400); require(!m.running && m.stage==A::Stage::Closing,"complete after final unload");
            require(m.poll(s,6400).kind==A::Action::Close,"closes final chest");
            completeClose(m,s,2,6450); require(!m.busy() && !s.enabled && !s.includeHotbar,"restores settings");
            require(m.status.find("пополните ресурсы")!=std::string::npos,"final unloaded batch asks to replenish materials");

            setup(s,m,version,3,1); s.player[30]=item(601,1); s.player[32]=item(601,1);
            m.start(s,targets(),0,64,0,7000); openTable(m,s,7000);
            require(m.poll(s,7500).kind==A::Action::CraftOne,"shells from two stacks");
            require(!s.player[30].item.present() && !s.player[32].item.present(),"both shell stacks consumed");
            S::Inventory correction; correction.window=0; correction.slot=0; correction.items={item(0,0).item};
            m.inventory(s,correction,7600); require(!m.running,"server correction stops"); m.reset(s);

            setup(s,m,version); m.start(s,targets(),0,64,0,7700); openTable(m,s,7700);
            m.poll(s,8200);
            auto stripped=item(777,1); stripped.item.blockRuntimeId=123;
            s.player[0]=stripped; correction.items={stripped.item};
            m.inventory(s,correction,8250);
            require(!m.running && m.status.find("NBT")!=std::string::npos,"same item with stripped NBT stops"); m.reset(s);
            setup(s,m,version); m.preparedResult={}; m.start(s,targets(),0,64,0,8300);
            require(!m.running && !m.busy(),"cannot start without selected template");
            s.enabled=true; s.includeHotbar=true; m.stop(s,"idle stop",8350);
            require(s.enabled && s.includeHotbar,"idle stop does not overwrite deposit settings");
            setup(s,m,version); s.intervalMs=30; m.start(s,targets(),0,64,0,8400);
            require(s.intervalMs==30,"starting preserves user deposit speed");
            m.stage=A::Stage::OpeningChest; m.target=targets()[1];
            m.opened(s,2,0,2,63,1,8500);
            require(s.intervalMs==30,"chest opening preserves 30 ms speed"); m.reset(s); s.intervalMs=1000;

            setup(s,m,version); m.start(s,targets(),0,64,0,8000); m.poll(s,8000);
            m.stop(s,"stop",8001); require(!m.running && m.busy(),"stop retains pending open");
            m.opened(s,4,1,1,63,1,8100); require(m.poll(s,8100).kind==A::Action::Close,"late open is closed");
            completeClose(m,s,4,8150); require(!m.busy(),"stopped cleanup complete");

            setup(s,m,version); m.start(s,targets(),0,64,0,9000); m.poll(s,9000);
            m.poll(s,18000); require(!m.busy() && !m.running,"timeout never retries");
            setup(s,m,version); m.start(s,targets(),0,64,0,9000); m.poll(s,9000);
            require(!m.opened(s,255,1,8,63,1,9050) && !m.busy(),"unrelated workbench open releases pending request");
            const auto rejectedStatus=m.status; m.poll(s,18000);
            require(m.status==rejectedStatus,"rejected response cannot produce a second spurious timeout");
            setup(s,m,version); m.start(s,targets(),0,64,0,9000);
            m.stage=A::Stage::OpeningChest; m.target=targets()[1];
            require(!m.opened(s,255,0,2,63,1,9050) && !m.busy(),"window 255 is not accepted as a physical chest");
            setup(s,m,version); s.authoritative=true; m.start(s,targets(),0,64,0,19000);
            require(!m.running,"authoritative mode fails closed without recipe IDs");
            s.authoritative=false; m.start(s,{},0,64,0,19000); require(!m.running,"missing nearby blocks fails closed");
            setup(s,m,version); m.start(s,targets(),0,64,0,20000); openTable(m,s,20000);
            for(auto& slot:s.player) if(!slot.item.present()) slot=item(777,1);
            require(m.poll(s,20500).kind==A::Action::Close,"full inventory switches to unloading");
            completeClose(m,s,1,20600); m.poll(s,21000); m.opened(s,2,0,2,63,1,21001); chestReady(s);
            for(auto& slot:s.chest) slot=item(99,64);
            m.poll(s,21500); require(m.poll(s,21501).kind==A::Action::Close,"full chest closes");
            completeClose(m,s,2,21600); auto next=m.poll(s,22000);
            require(next.kind==A::Action::Open && next.target.x==3,"tries next chest");
            m.opened(s,3,0,3,63,1,22001); chestReady(s); for(auto& slot:s.chest) slot=item(99,64);
            m.poll(s,22500); m.poll(s,22501); completeClose(m,s,3,22600); m.poll(s,23000);
            require(!m.running && !m.busy(),"all full ends without loop");
            require(m.status.find("Все доступные сундуки заполнены")!=std::string::npos,
                "all full has an explicit terminal reason");

            // A full first batch unloads, then resumes the same frozen template.
            setup(s,m,version); s.enabled=false; s.includeHotbar=false;
            for(int i=0;i<29;++i) s.player[i]=item(99,1);
            for(int i=32;i<36;++i) s.player[i]=item(99,1);
            std::weak_ptr<const A::Wire> releasedTemplate=m.preparedResult.wire;
            m.start(s,targets(),0,64,0,30000); openTable(m,s,30000);
            require(m.poll(s,30500).kind==A::Action::CraftOne,"single remaining slot crafted");
            require(m.poll(s,31500).kind==A::Action::Close && !m.finishing,"full batch with remaining materials closes table");
            completeClose(m,s,1,31600); m.poll(s,32000); m.opened(s,2,0,2,63,1,32001); chestReady(s);
            require(m.poll(s,32500).kind==A::Action::DepositOne,"batch deposited");
            m.poll(s,33500); require(m.poll(s,33501).kind==A::Action::Close,"unloaded chest closes before restart");
            completeClose(m,s,2,33600); openTable(m,s,34000);
            require(m.poll(s,34500).kind==A::Action::CraftOne && m.crafted==2 && m.stored==1,"next batch resumes crafting");
            m.reset(s); s.player={};
            require(releasedTemplate.expired(),"reset releases frozen template after inventory clears");

            // Full chest memory survives craft/unload cycles, unlike visited.
            setup(s,m,version,8,4);
            for(int i=0;i<29;++i) s.player[i]=item(99,1);
            for(int i=32;i<36;++i) s.player[i]=item(99,1);
            m.start(s,targets(),0,64,0,35000); openTable(m,s,35000);
            m.poll(s,35500); m.poll(s,36500); completeClose(m,s,1,36600);
            m.poll(s,37000); m.opened(s,2,0,2,63,1,37001); chestReady(s);
            for(auto& slot:s.chest) slot=item(99,64);
            s.chest[26]=item(0,0);
            require(m.poll(s,37500).kind==A::Action::DepositOne,"last free chest slot receives last batch item");
            m.poll(s,38500);
            require(m.chestFull(targets()[1].key()),"last destination remembered full even when inventory is now empty");
            m.poll(s,38501); completeClose(m,s,2,38600); openTable(m,s,39000);
            m.poll(s,39500); m.poll(s,40500); completeClose(m,s,1,40600);
            auto unvisited=m.poll(s,41000);
            require(unvisited.kind==A::Action::Open && unvisited.target.x==3,
                "new craft batch skips remembered full chest"); m.reset(s);

            // Prove the two halves only from different open coordinates AND
            // 54 slots; never mark an unrelated neighbouring single chest.
            setup(s,m,version); m.start(s,targets(),0,64,0,42000);
            m.stage=A::Stage::Chest; m.poll(s,42000);
            m.opened(s,51,0,3,63,1,42001);
            s.chest.assign(54,item(99,64)); s.chestReady=true; s.player[0]=item(777,1);
            m.poll(s,42500);
            require(m.chestFull(targets()[1].key()) && m.chestFull(targets()[2].key()) && m.fullChests.size()==1,
                "requested and returned halves share one full chest memory");
            m.poll(s,42501); completeClose(m,s,51,42600); m.poll(s,43000);
            require(!m.busy() && !m.running && m.status.find("Все доступные сундуки заполнены")!=std::string::npos,
                "all full double chest stops without opening its other half");
            setup(s,m,version); m.start(s,targets(),0,64,0,44000);
            require(m.fullChests.empty() && m.chestAliases.empty(),"explicit new start rechecks capacity");
            m.stage=A::Stage::Chest; m.poll(s,44000); m.opened(s,52,0,2,63,1,44001);
            chestReady(s); for(auto& slot:s.chest) slot=item(99,64); s.player[0]=item(777,1);
            m.poll(s,44500);
            require(m.chestFull(targets()[1].key()) && !m.chestFull(targets()[2].key()),
                "adjacent independent single chest is not wrongly marked full"); m.reset(s);

            // Last free slot of the only chest fills with materials remaining:
            // stop here, not after crafting an extra inventory that cannot fit.
            setup(s,m,version); auto only=targets(); only.resize(2);
            m.start(s,only,0,64,0,46000); m.stage=A::Stage::Chest; m.poll(s,46000);
            m.opened(s,53,0,2,63,1,46001); chestReady(s);
            for(auto& slot:s.chest) slot=item(99,64); s.chest[26]=item(0,0); s.player[0]=item(777,1);
            m.poll(s,46500); m.poll(s,47500);
            require(!m.running && m.stage==A::Stage::Closing && m.crafted==0,
                "last available storage filled stops before crafting another batch");
            m.poll(s,47501); completeClose(m,s,53,47600);
            require(!m.busy() && m.status.find("Все доступные сундуки заполнены")!=std::string::npos,
                "all full terminal message survives both close acknowledgements");
            m.reset(s); require(m.fullChests.empty() && m.chestAliases.empty(),"world/session reset releases chest memory");

            // Closing is a two-sided handshake, never a delay-based guess.
            setup(s,m,version); m.configureTiming(100,700);
            m.start(s,targets(),0,64,0,40000); openTable(m,s,40000);
            require(m.poll(s,40700).kind==A::Action::None,"configured open pause is respected");
            require(m.poll(s,40701).kind==A::Action::CraftOne,"craft begins after configured open pause");
            require(m.poll(s,40800).kind==A::Action::None,"configured craft interval is respected");
            require(m.poll(s,40801).kind==A::Action::CraftOne,"100ms setting allows next craft, no catchup");
            require(m.poll(s,40901).kind==A::Action::Close,"last craft starts GUI closure");
            m.closed(s,1,40950,false);
            require(m.stage==A::Stage::Closing && m.poll(s,42000).kind==A::Action::None,
                "server-only close never opens chest");
            m.closed(s,0,42001,true);
            require(!m.clientClosed,"inventory window zero cannot acknowledge table close");
            m.closed(s,1,42002,true);
            require(m.stage==A::Stage::Chest && m.poll(s,42701).kind==A::Action::None,"pause starts after BOTH confirmations");
            m.closed(s,0,42701,false); m.closed(s,1,42701,false);
            require(m.running && m.stage==A::Stage::Chest,"late duplicates and window zero do not abort transition");
            require(m.poll(s,42702).kind==A::Action::Open,"chest opens only after closure and transition pause"); m.reset(s);

            setup(s,m,version); m.start(s,targets(),0,64,0,50000); openTable(m,s,50000);
            m.stop(s,"stop",50500); m.poll(s,50500); const auto closeDeadline=m.deadline;
            m.closed(s,1,50501,true);
            require(m.busy() && !m.serverClosed,"client-only acknowledgement waits for server");
            m.stop(s,"stop again",50502);
            require(m.deadline==closeDeadline && m.poll(s,50502).kind==A::Action::None,"repeated stop does not resend or extend close deadline");
            m.poll(s,closeDeadline);
            require(!m.busy() && m.status.find("Сервер не подтвердил")!=std::string::npos,"missing server echo times out without opening another window");
            setup(s,m,version); m.start(s,targets(),0,64,0,60000); openTable(m,s,60000);
            m.stop(s,"stop",60500); m.poll(s,60500); m.closed(s,1,60501,false); m.poll(s,68500);
            require(!m.busy() && m.status.find("Minecraft не подтвердил")!=std::string::npos,"missing client ack leaves GUI warning, not a chest open");
            setup(s,m,version,0,0); m.start(s,targets(),0,64,0,70000); openTable(m,s,70000);
            m.poll(s,71000); m.poll(s,71001); completeClose(m,s,1,71002);
            require(!m.running && !m.busy() && m.status.find("пополните ресурсы")!=std::string::npos,"empty materials asks to replenish after closing");
            m.configureTiming(-1,0); require(m.craftIntervalMs==100 && m.windowPauseMs==300,"native timing lower bounds");
            m.configureTiming(INT_MAX,INT_MAX); require(m.craftIntervalMs==5000 && m.windowPauseMs==3000,"native timing upper bounds");
        }
        // Shared maps adapter: configuration and inactive GUI tracking must not
        // invoke networking or interfere with ordinary manual containers.
        {
            RelayState state;state.version="1.21.100";
            state.mapCommand(bedrock::JsRuntimeValue::object({{"op",bedrock::JsRuntimeValue::string("configure")},{"hold",bedrock::JsRuntimeValue::number(2)},{"transfer",bedrock::JsRuntimeValue::number(0)}}));
            require(state.mapQueue.timing.hold==300&&state.mapQueue.timing.transfer==500,"map timing API clamps unsafe values");
            auto event=containerOpen(state.version,255,1,1,64,1);state.observeMapQueue(event,false);
            require(state.mapScreenWindow==255&&!state.mapBusy(),"map module remembers manually opened workbench without starting");
            event.packet=bedrock::VersionedPacketCodec::forVersion(state.version).makePacketByName("container_close",{255,1,0});
            state.observeMapQueue(event,true);require(state.mapScreenWindow==255,"client-only close keeps map preflight blocked");
            state.observeMapQueue(event,false);require(state.mapScreenWindow==-1,"server close releases map preflight guard");
        }
        verifyNativeIntegration();
        verifyPlatformIntegration();
        std::cout<<"Auto 2: "<<checks<<" checks passed\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
}
