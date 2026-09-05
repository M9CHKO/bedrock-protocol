cmake_minimum_required(VERSION 3.16)
project(bedrock-protocol-cpp VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BEDROCK_PROTOCOL_CPP_BUILD_TOOLS "Build command-line helpers: bedrock, bedrock-session, login, ping" ON)
option(BEDROCK_PROTOCOL_CPP_BUILD_BOT "Build local bot that links bedrock_protocol" ON)
option(BEDROCK_PROTOCOL_CPP_BUILD_EXAMPLES "Build example bots" ON)
option(BEDROCK_PROTOCOL_CPP_INSTALL "Enable install rules" ON)

if(ANDROID)
    # Android's NDK exposes zlib as a platform library. OpenSSL is supplied by
    # the consuming Gradle/Prefab project because it is not an NDK API.
    find_library(BEDROCK_ANDROID_ZLIB_LIBRARY z REQUIRED)
    add_library(bedrock_android_zlib INTERFACE)
    target_link_libraries(
        bedrock_android_zlib
        INTERFACE ${BEDROCK_ANDROID_ZLIB_LIBRARY}
    )
    add_library(ZLIB::ZLIB ALIAS bedrock_android_zlib)

    find_package(openssl REQUIRED CONFIG)
    if(NOT TARGET openssl::crypto)
        message(FATAL_ERROR "Android OpenSSL Prefab package has no crypto module")
    endif()
    add_library(bedrock_android_crypto INTERFACE)
    target_link_libraries(bedrock_android_crypto INTERFACE openssl::crypto)
    add_library(OpenSSL::Crypto ALIAS bedrock_android_crypto)
else()
    find_package(ZLIB REQUIRED)
    find_package(OpenSSL REQUIRED)
endif()
find_package(Threads REQUIRED)

# bedrock-protocol's JavaScript implementation delegates RakNet reliability,
# ordering, fragmentation, retransmission and congestion control to
# raknet-native. Build the same RakNet ReliabilityLayer into the C++ library
# instead of maintaining a second wire implementation here.
file(GLOB BEDROCK_RAKNET_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/raknet/Source/*.cpp"
)
add_library(bedrock_raknet_objects OBJECT ${BEDROCK_RAKNET_SOURCES})
set_target_properties(bedrock_raknet_objects PROPERTIES
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED ON
    # The upstream RakNet snapshot predates strict ISO feature-test defaults
    # and expects POSIX declarations such as gethostname() to be visible.
    CXX_EXTENSIONS ON
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(bedrock_raknet_objects PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/raknet/Source
)
target_compile_definitions(bedrock_raknet_objects PRIVATE
    _RAKNET_LIB
    MAXIMUM_NUMBER_OF_INTERNAL_IDS=20
)
if(WIN32)
    # DR_SHA1's legacy compiler check recognizes MSVC but not MinGW. Use the
    # platform TCHAR header instead of defining a macro that collides with
    # Windows SDK typedefs.
    target_compile_definitions(bedrock_raknet_objects PRIVATE SHA1_HAS_TCHAR)
endif()

add_library(bedrock_protocol
    $<TARGET_OBJECTS:bedrock_raknet_objects>
    src/BinaryStream.cpp
    src/RakNetPing.cpp
    src/RakNetConnect.cpp
    src/BedrockFramer.cpp
    src/LoginPacket.cpp
    src/BedrockEncryption.cpp
    src/BedrockKeyExchange.cpp
    src/auth/BedrockAuthJwt.cpp
    src/auth/BedrockClientDataBuilder.cpp
    src/auth/FileAuthCache.cpp
    src/auth/JsPromise.cpp
    src/auth/JsRuntimeValue.cpp
    src/auth/LiveTokenManager.cpp
    src/auth/BedrockTokenManagers.cpp
    src/auth/MicrosoftAuthFlow.cpp
    src/auth/MsaTokenManager.cpp
    src/auth/MsalCachePlugin.cpp
    src/auth/MsalError.cpp
    src/auth/MsalHttpClient.cpp
    src/auth/MsalRequestParameterBuilder.cpp
    src/auth/MsalSerializableTokenCache.cpp
    src/auth/MsalServerTelemetryManager.cpp
    src/auth/MsalTokenResponseHandler.cpp
    src/auth/NativeBedrockAuthflow.cpp
    src/auth/NativeMsalPublicClientApplication.cpp
    src/auth/XboxLiveAuth.cpp
    src/auth/XboxProofKey.cpp
    src/auth/XboxTokenManager.cpp
    src/chat/BedrockChat.cpp
    src/client/BedrockNetworkClient.cpp
    src/client/RakNetClient.cpp
    src/events/BedrockPacketEventAdapter.cpp
    src/generated/GeneratedProtocolTables.cpp
    src/generated/GeneratedProtocolTypes.cpp
    src/item/BedrockItem.cpp
    src/nbt/BedrockNbt.cpp
    src/nbt/BedrockNbtFile.cpp
    src/registry/BedrockDefaultSkin.cpp
    src/registry/BedrockFeatureRegistry.cpp
    src/registry/BedrockGameplayRegistry.cpp
    src/registry/BedrockLootRegistry.cpp
    src/registry/BedrockRegistry.cpp
    src/protodef/ProtoDefNbt.cpp
    src/protocol/BatchCodec.cpp
    src/protocol/BedrockPacketIO.cpp
    src/protocol/GamePacketCodec.cpp
    src/protocol/PacketDispatcher.cpp
    src/protocol/PacketFactory.cpp
    src/protocol/PacketPayloadReader.cpp
    src/protocol/PacketRegistry.cpp
    src/protocol/SnappyCodec.cpp
    src/protocol/TypedPacketDispatcher.cpp
    src/protocol/VersionedMcpeCodec.cpp
    src/realms/BedrockRealms.cpp
    src/relay/BedrockLiveRelay.cpp
    src/server/BedrockLoginVerifier.cpp
    src/server/ServerAdvertisement.cpp
    src/server/RakNetServer.cpp
    src/util/BinaryStream.cpp
    src/util/VarInt.cpp
    src/util/XxHash64.cpp
    src/world/BedrockBlockRegistry.cpp
    src/world/BedrockChunk.cpp
    src/world/BedrockLegacyChunk.cpp
    src/world/BedrockSubChunkPacket.cpp
    src/world/WorldIterators.cpp
)
add_library(BedrockProtocol::bedrock_protocol ALIAS bedrock_protocol)
add_library(Bedrock::bedrock ALIAS bedrock_protocol)

# Backward compatible target name for old local tests.
add_library(bedrock_core ALIAS bedrock_protocol)

set_target_properties(bedrock_protocol PROPERTIES
    OUTPUT_NAME bedrock_protocol
    EXPORT_NAME bedrock_protocol
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(bedrock_protocol
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_include_directories(bedrock_protocol PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/raknet/Source
)

target_link_libraries(bedrock_protocol
    PUBLIC
        ZLIB::ZLIB
        OpenSSL::Crypto
        Threads::Threads
)
if(WIN32)
    target_link_libraries(bedrock_protocol PUBLIC ws2_32)
endif()
target_compile_definitions(bedrock_protocol
    PUBLIC _DEFAULT_SOURCE
    PRIVATE MAXIMUM_NUMBER_OF_INTERNAL_IDS=20
)

find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_custom_target(generate-protocol-artifacts
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_protocol_artifacts.py
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Regenerating Bedrock protocol C++ and TSV artifacts"
        VERBATIM
    )
    add_custom_target(check-protocol-artifacts
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_protocol_artifacts.py
            --check
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Checking generated Bedrock protocol artifacts"
        VERBATIM
    )
endif()

if(BEDROCK_PROTOCOL_CPP_BUILD_TOOLS)
    add_executable(bedrock tools/bedrock.cpp)
    target_link_libraries(bedrock PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(login tools/login.cpp)
    target_link_libraries(login PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(xbox-login tools/xbox-login.cpp)
    target_link_libraries(xbox-login PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(ping tools/bedrock_ping.cpp)
    target_link_libraries(ping PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(protocol-roundtrip tools/protocol_roundtrip.cpp)
    target_link_libraries(protocol-roundtrip PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(
        take-item-entity-schema-smoke
        tools/take_item_entity_schema_smoke.cpp
    )
    target_link_libraries(
        take-item-entity-schema-smoke
        PRIVATE BedrockProtocol::bedrock_protocol
    )

    add_executable(
        debug-renderer-packet-smoke
        tools/debug_renderer_packet_smoke.cpp
    )
    target_link_libraries(
        debug-renderer-packet-smoke
        PRIVATE BedrockProtocol::bedrock_protocol
    )

    add_executable(
        falling-block-entity-packet-smoke
        tools/falling_block_entity_packet_smoke.cpp
    )
    target_link_libraries(
        falling-block-entity-packet-smoke
        PRIVATE BedrockProtocol::bedrock_protocol
    )

    add_executable(snappy-codec-smoke tools/snappy_codec_smoke.cpp)
    add_executable(area-fill-geometry-smoke tools/area_fill_geometry_smoke.cpp)
    target_link_libraries(area-fill-geometry-smoke PRIVATE BedrockProtocol::bedrock_protocol)
    target_link_libraries(snappy-codec-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(nbt-codec-smoke tools/nbt_codec_smoke.cpp)
    target_link_libraries(nbt-codec-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(bedrock-nbt-file-smoke tools/bedrock_nbt_file_smoke.cpp)
    target_link_libraries(bedrock-nbt-file-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(protodef-nbt-smoke tools/protodef_nbt_smoke.cpp)
    target_link_libraries(protodef-nbt-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(protodef-variables-smoke tools/protodef_variables_smoke.cpp)
    target_link_libraries(protodef-variables-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(protodef-structured-smoke tools/protodef_structured_smoke.cpp)
    target_link_libraries(protodef-structured-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(subchunk-packet-smoke tools/subchunk_packet_smoke.cpp)
    target_link_libraries(subchunk-packet-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(msal-native-smoke tools/msal_native_smoke.cpp)
    target_link_libraries(msal-native-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(
        native-auth-transport-smoke
        tools/native_auth_transport_smoke.cpp
    )
    target_link_libraries(
        native-auth-transport-smoke
        PRIVATE BedrockProtocol::bedrock_protocol
    )

    add_executable(live-token-manager-smoke tools/live_token_manager_smoke.cpp)
    target_link_libraries(live-token-manager-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(bedrock-token-managers-smoke tools/bedrock_token_managers_smoke.cpp)
    target_link_libraries(bedrock-token-managers-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(bedrock-realms-smoke tools/bedrock_realms_smoke.cpp)
    target_link_libraries(bedrock-realms-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(xbox-token-manager-smoke tools/xbox_token_manager_smoke.cpp)
    target_link_libraries(xbox-token-manager-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(microsoft-auth-flow-smoke tools/microsoft_auth_flow_smoke.cpp)
    target_link_libraries(microsoft-auth-flow-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(ping-api-smoke tools/ping_api_smoke.cpp)
    target_link_libraries(ping-api-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(create-server-factory-smoke tools/create_server_factory_smoke.cpp)
    target_link_libraries(create-server-factory-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(server-outbound-queue-smoke tools/server_outbound_queue_smoke.cpp)
    target_link_libraries(server-outbound-queue-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(create-client-factory-smoke tools/create_client_factory_smoke.cpp)
    target_link_libraries(create-client-factory-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(legacy-encryption-smoke tools/legacy_encryption_smoke.cpp)
    target_link_libraries(legacy-encryption-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(raknet-server-smoke tools/raknet_server_smoke.cpp)
    target_link_libraries(raknet-server-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(raknet-connection-lifecycle-smoke tools/raknet_connection_lifecycle_smoke.cpp)
    target_link_libraries(raknet-connection-lifecycle-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(server-login-verifier-smoke tools/server_login_verifier_smoke.cpp)
    target_link_libraries(server-login-verifier-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(server-protocol-version-smoke tools/server_protocol_version_smoke.cpp)
    target_link_libraries(server-protocol-version-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(raknet-client-smoke tools/raknet_client_smoke.cpp)
    target_link_libraries(raknet-client-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(
        raknet-ordered-encryption-flood-smoke
        tools/raknet_ordered_encryption_flood_smoke.cpp
    )
    target_link_libraries(
        raknet-ordered-encryption-flood-smoke
        PRIVATE BedrockProtocol::bedrock_protocol
    )

    add_executable(network-client-smoke tools/network_client_smoke.cpp)
    target_link_libraries(network-client-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(live-relay-smoke tools/live_relay_smoke.cpp)
    target_link_libraries(live-relay-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(live-relay-multi-client-smoke tools/live_relay_multi_client_smoke.cpp)
    target_link_libraries(live-relay-multi-client-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(live-relay-options-smoke tools/live_relay_options_smoke.cpp)
    target_link_libraries(live-relay-options-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(
        live-relay-regression-smoke
        tools/live_relay_regression_smoke.cpp
    )
    target_link_libraries(
        live-relay-regression-smoke
        PRIVATE BedrockProtocol::bedrock_protocol
    )

    add_executable(chunk-smoke tools/chunk_smoke.cpp)
    target_link_libraries(chunk-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(
        level-chunk-section-fallback-smoke
        tools/level_chunk_section_fallback_smoke.cpp
    )
    target_link_libraries(
        level-chunk-section-fallback-smoke
        PRIVATE BedrockProtocol::bedrock_protocol
    )

    add_executable(
        chunk-publisher-retention-smoke
        tools/chunk_publisher_retention_smoke.cpp
    )
    target_link_libraries(
        chunk-publisher-retention-smoke
        PRIVATE BedrockProtocol::bedrock_protocol
    )

    add_executable(
        level-chunk-retention-cache-smoke
        tools/level_chunk_retention_cache_smoke.cpp
    )
    target_link_libraries(
        level-chunk-retention-cache-smoke
        PRIVATE BedrockProtocol::bedrock_protocol
    )

    add_executable(
        entity-position-tracker-smoke
        tools/entity_position_tracker_smoke.cpp
    )
    target_link_libraries(
        entity-position-tracker-smoke
        PRIVATE BedrockProtocol::bedrock_protocol
    )

    add_executable(world-iterators-smoke tools/world_iterators_smoke.cpp)
    target_link_libraries(world-iterators-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(map-item-data-smoke tools/map_item_data_smoke.cpp)
    target_link_libraries(map-item-data-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(bedrock-block-registry-smoke tools/bedrock_block_registry_smoke.cpp)
    target_link_libraries(bedrock-block-registry-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(bedrock-item-smoke tools/bedrock_item_smoke.cpp)
    target_link_libraries(bedrock-item-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(bedrock-registry-smoke tools/bedrock_registry_smoke.cpp)
    target_link_libraries(bedrock-registry-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(bedrock-gameplay-registry-smoke tools/bedrock_gameplay_registry_smoke.cpp)
    target_link_libraries(bedrock-gameplay-registry-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(bedrock-feature-registry-smoke tools/bedrock_feature_registry_smoke.cpp)
    target_link_libraries(bedrock-feature-registry-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(bedrock-loot-skin-smoke tools/bedrock_loot_skin_smoke.cpp)
    target_link_libraries(bedrock-loot-skin-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(bedrock-dynamic-registry-smoke tools/bedrock_dynamic_registry_smoke.cpp)
    target_link_libraries(bedrock-dynamic-registry-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(bedrock-block-entity-smoke tools/bedrock_block_entity_smoke.cpp)
    target_link_libraries(bedrock-block-entity-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(bedrock-legacy-chunk-smoke tools/bedrock_legacy_chunk_smoke.cpp)
    target_link_libraries(bedrock-legacy-chunk-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(bedrock-chat-smoke tools/bedrock_chat_smoke.cpp)
    target_link_libraries(bedrock-chat-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(public-header-smoke tools/public_header_smoke.cpp)
    target_link_libraries(public-header-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(public-header-reverse-smoke tools/public_header_reverse_smoke.cpp)
    target_link_libraries(public-header-reverse-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(xbox-proof-key-smoke tools/xbox_proof_key_smoke.cpp)
    target_link_libraries(xbox-proof-key-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    if(BUILD_TESTING)
        add_test(NAME protocol-roundtrip COMMAND protocol-roundtrip)
        add_test(
            NAME take-item-entity-schema-smoke
            COMMAND take-item-entity-schema-smoke
        )
        add_test(
            NAME debug-renderer-packet-smoke
            COMMAND debug-renderer-packet-smoke
        )
        add_test(
            NAME falling-block-entity-packet-smoke
            COMMAND falling-block-entity-packet-smoke
        )
        if(Python3_Interpreter_FOUND)
            add_test(
                NAME generated-protocol-consistency
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_protocol_artifacts.py
                    --check
            )
        endif()
        add_test(NAME snappy-codec-smoke COMMAND snappy-codec-smoke)
        add_test(NAME area-fill-geometry-smoke COMMAND area-fill-geometry-smoke)
        add_test(NAME nbt-codec-smoke COMMAND nbt-codec-smoke)
        add_test(NAME bedrock-nbt-file-smoke COMMAND bedrock-nbt-file-smoke)
        add_test(NAME protodef-nbt-smoke COMMAND protodef-nbt-smoke)
        add_test(NAME protodef-variables-smoke COMMAND protodef-variables-smoke)
        add_test(NAME protodef-structured-smoke COMMAND protodef-structured-smoke)
        add_test(NAME subchunk-packet-smoke COMMAND subchunk-packet-smoke)
        add_test(NAME msal-native-smoke COMMAND msal-native-smoke)
        add_test(
            NAME native-auth-transport-smoke
            COMMAND native-auth-transport-smoke
        )
        add_test(NAME live-token-manager-smoke COMMAND live-token-manager-smoke)
        add_test(NAME bedrock-token-managers-smoke COMMAND bedrock-token-managers-smoke)
        add_test(NAME bedrock-realms-smoke COMMAND bedrock-realms-smoke)
        add_test(NAME xbox-token-manager-smoke COMMAND xbox-token-manager-smoke)
        add_test(NAME microsoft-auth-flow-smoke COMMAND microsoft-auth-flow-smoke)
        add_test(NAME ping-api-smoke COMMAND ping-api-smoke)
        add_test(NAME create-server-factory-smoke COMMAND create-server-factory-smoke)
        add_test(NAME server-outbound-queue-smoke COMMAND server-outbound-queue-smoke)
        add_test(NAME create-client-factory-smoke COMMAND create-client-factory-smoke)
        add_test(NAME legacy-encryption-smoke COMMAND legacy-encryption-smoke)
        add_test(NAME chunk-smoke COMMAND chunk-smoke)
        add_test(
            NAME level-chunk-section-fallback-smoke
            COMMAND level-chunk-section-fallback-smoke
        )
        add_test(
            NAME chunk-publisher-retention-smoke
            COMMAND chunk-publisher-retention-smoke
        )
        add_test(
            NAME level-chunk-retention-cache-smoke
            COMMAND level-chunk-retention-cache-smoke
        )
        add_test(
            NAME entity-position-tracker-smoke
            COMMAND entity-position-tracker-smoke
        )
        add_test(NAME world-iterators-smoke COMMAND world-iterators-smoke)
        add_test(NAME map-item-data-smoke COMMAND map-item-data-smoke)
        add_test(NAME bedrock-block-registry-smoke COMMAND bedrock-block-registry-smoke)
        add_test(NAME bedrock-item-smoke COMMAND bedrock-item-smoke)
        add_test(NAME bedrock-registry-smoke COMMAND bedrock-registry-smoke)
        add_test(NAME bedrock-gameplay-registry-smoke COMMAND bedrock-gameplay-registry-smoke)
        add_test(NAME bedrock-feature-registry-smoke COMMAND bedrock-feature-registry-smoke)
        add_test(NAME bedrock-loot-skin-smoke COMMAND bedrock-loot-skin-smoke)
        add_test(NAME bedrock-dynamic-registry-smoke COMMAND bedrock-dynamic-registry-smoke)
        add_test(NAME bedrock-block-entity-smoke COMMAND bedrock-block-entity-smoke)
        add_test(NAME bedrock-legacy-chunk-smoke COMMAND bedrock-legacy-chunk-smoke)
        add_test(NAME bedrock-chat-smoke COMMAND bedrock-chat-smoke)
        add_test(NAME raknet-server-smoke COMMAND raknet-server-smoke)
        add_test(NAME raknet-connection-lifecycle-smoke COMMAND raknet-connection-lifecycle-smoke)
        add_test(NAME server-login-verifier-smoke COMMAND server-login-verifier-smoke)
        add_test(NAME server-protocol-version-smoke COMMAND server-protocol-version-smoke)
        add_test(NAME raknet-client-smoke COMMAND raknet-client-smoke)
        add_test(
            NAME raknet-ordered-encryption-flood-smoke
            COMMAND raknet-ordered-encryption-flood-smoke
        )
        add_test(NAME network-client-smoke COMMAND network-client-smoke)
        add_test(NAME live-relay-smoke COMMAND live-relay-smoke)
        add_test(NAME live-relay-multi-client-smoke COMMAND live-relay-multi-client-smoke)
        add_test(NAME live-relay-options-smoke COMMAND live-relay-options-smoke)
        add_test(NAME live-relay-regression-smoke COMMAND live-relay-regression-smoke)
        add_test(NAME public-header-smoke COMMAND public-header-smoke)
        add_test(NAME public-header-reverse-smoke COMMAND public-header-reverse-smoke)
        add_test(NAME xbox-proof-key-smoke COMMAND xbox-proof-key-smoke)
        set_tests_properties(
            protocol-roundtrip
            take-item-entity-schema-smoke
            debug-renderer-packet-smoke
            falling-block-entity-packet-smoke
            bedrock-nbt-file-smoke
            protodef-nbt-smoke
            protodef-variables-smoke
            protodef-structured-smoke
            msal-native-smoke
            live-token-manager-smoke
            bedrock-token-managers-smoke
            bedrock-realms-smoke
            xbox-token-manager-smoke
            microsoft-auth-flow-smoke
            ping-api-smoke
            create-server-factory-smoke
            server-outbound-queue-smoke
            create-client-factory-smoke
            legacy-encryption-smoke
            chunk-smoke
            level-chunk-section-fallback-smoke
            chunk-publisher-retention-smoke
            level-chunk-retention-cache-smoke
            entity-position-tracker-smoke
            world-iterators-smoke
            map-item-data-smoke
            bedrock-block-registry-smoke
            bedrock-item-smoke
            bedrock-registry-smoke
            bedrock-gameplay-registry-smoke
            bedrock-feature-registry-smoke
            bedrock-loot-skin-smoke
            bedrock-dynamic-registry-smoke
            bedrock-block-entity-smoke
            bedrock-legacy-chunk-smoke
            bedrock-chat-smoke
            raknet-server-smoke
            raknet-connection-lifecycle-smoke
            server-login-verifier-smoke
            server-protocol-version-smoke
            raknet-client-smoke
            raknet-ordered-encryption-flood-smoke
            network-client-smoke
            live-relay-smoke
            live-relay-multi-client-smoke
            live-relay-options-smoke
            live-relay-regression-smoke
            public-header-smoke
            public-header-reverse-smoke
            xbox-proof-key-smoke
            PROPERTIES
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                TIMEOUT 120
        )
        if(TEST generated-protocol-consistency)
            set_tests_properties(
                generated-protocol-consistency
                PROPERTIES
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                    TIMEOUT 180
            )
        endif()
    endif()

    # Runtime helper used by the public API and the bedrock launcher.
    add_executable(bedrock-session tools/session.cpp)
    target_link_libraries(bedrock-session PRIVATE BedrockProtocol::bedrock_protocol)
endif()

if(BEDROCK_PROTOCOL_CPP_BUILD_BOT)
    add_executable(bot bots/bot.cpp)
    target_link_libraries(bot PRIVATE BedrockProtocol::bedrock_protocol)
endif()

if(BEDROCK_PROTOCOL_CPP_BUILD_EXAMPLES)
    add_executable(simple-create-client-bot examples/simple_create_client_bot.cpp)
    target_link_libraries(simple-create-client-bot PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(packet-event-bot examples/packet_event_bot.cpp)
    target_link_libraries(packet-event-bot PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(medium-bot examples/medium_bot.cpp)
    target_link_libraries(medium-bot PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(relay-packet-bot examples/relay_packet_bot.cpp)
    target_link_libraries(relay-packet-bot PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(relay-test-server examples/relay_test_server.cpp)
    target_link_libraries(relay-test-server PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(proxy-relay relay/proxy_relay.cpp)
    target_link_libraries(proxy-relay PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(simple-server examples/simple_server.cpp)
    target_link_libraries(simple-server PRIVATE BedrockProtocol::bedrock_protocol)
endif()

if(BEDROCK_PROTOCOL_CPP_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(TARGETS bedrock_protocol
        EXPORT BedrockProtocolTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    if(BEDROCK_PROTOCOL_CPP_BUILD_TOOLS)
        install(TARGETS bedrock bedrock-session login xbox-login ping protocol-roundtrip msal-native-smoke live-token-manager-smoke bedrock-token-managers-smoke xbox-token-manager-smoke microsoft-auth-flow-smoke ping-api-smoke create-server-factory-smoke server-outbound-queue-smoke create-client-factory-smoke legacy-encryption-smoke raknet-server-smoke raknet-connection-lifecycle-smoke server-login-verifier-smoke server-protocol-version-smoke raknet-client-smoke network-client-smoke live-relay-smoke live-relay-multi-client-smoke live-relay-options-smoke chunk-smoke world-iterators-smoke bedrock-block-registry-smoke bedrock-item-smoke bedrock-registry-smoke bedrock-gameplay-registry-smoke bedrock-feature-registry-smoke bedrock-chat-smoke
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
    endif()

    if(BEDROCK_PROTOCOL_CPP_BUILD_EXAMPLES)
        install(TARGETS simple-create-client-bot packet-event-bot medium-bot relay-packet-bot relay-test-server proxy-relay simple-server
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
    endif()

    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(DIRECTORY docs DESTINATION ${CMAKE_INSTALL_DATADIR}/bedrock-protocol-cpp OPTIONAL)
    install(FILES data/client_data_template.json DESTINATION ${CMAKE_INSTALL_DATADIR}/bedrock-protocol-cpp/data)
    install(DIRECTORY data/minecraft-data DESTINATION ${CMAKE_INSTALL_DATADIR}/bedrock-protocol-cpp/data OPTIONAL)
    install(DIRECTORY data/generated DESTINATION ${CMAKE_INSTALL_DATADIR}/bedrock-protocol-cpp/data OPTIONAL)

    install(EXPORT BedrockProtocolTargets
        FILE BedrockProtocolTargets.cmake
        NAMESPACE BedrockProtocol::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/BedrockProtocol
    )

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BedrockProtocolConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/BedrockProtocolConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/BedrockProtocol
    )

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/BedrockProtocolConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/BedrockProtocolConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/BedrockProtocolConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/BedrockProtocol
    )
endif()
