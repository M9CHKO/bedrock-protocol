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

find_package(ZLIB REQUIRED)
find_package(OpenSSL REQUIRED)

add_library(bedrock_protocol
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
    src/client/BedrockNetworkClient.cpp
    src/client/RakNetClient.cpp
    src/events/BedrockPacketEventAdapter.cpp
    src/generated/GeneratedProtocolTables.cpp
    src/nbt/BedrockNbt.cpp
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
    src/world/BedrockChunk.cpp
    src/world/BedrockSubChunkPacket.cpp
)
add_library(BedrockProtocol::bedrock_protocol ALIAS bedrock_protocol)
add_library(Bedrock::bedrock ALIAS bedrock_protocol)

# Backward compatible target name for old local tests.
add_library(bedrock_core ALIAS bedrock_protocol)

set_target_properties(bedrock_protocol PROPERTIES
    OUTPUT_NAME bedrock_protocol
    EXPORT_NAME bedrock_protocol
)

target_include_directories(bedrock_protocol
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(bedrock_protocol PUBLIC ZLIB::ZLIB OpenSSL::Crypto)
target_compile_definitions(bedrock_protocol PUBLIC _DEFAULT_SOURCE)

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

    add_executable(snappy-codec-smoke tools/snappy_codec_smoke.cpp)
    target_link_libraries(snappy-codec-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(nbt-codec-smoke tools/nbt_codec_smoke.cpp)
    target_link_libraries(nbt-codec-smoke PRIVATE BedrockProtocol::bedrock_protocol)

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

    add_executable(raknet-client-smoke tools/raknet_client_smoke.cpp)
    target_link_libraries(raknet-client-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(network-client-smoke tools/network_client_smoke.cpp)
    target_link_libraries(network-client-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(live-relay-smoke tools/live_relay_smoke.cpp)
    target_link_libraries(live-relay-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(live-relay-multi-client-smoke tools/live_relay_multi_client_smoke.cpp)
    target_link_libraries(live-relay-multi-client-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(live-relay-options-smoke tools/live_relay_options_smoke.cpp)
    target_link_libraries(live-relay-options-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(chunk-smoke tools/chunk_smoke.cpp)
    target_link_libraries(chunk-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(public-header-smoke tools/public_header_smoke.cpp)
    target_link_libraries(public-header-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(public-header-reverse-smoke tools/public_header_reverse_smoke.cpp)
    target_link_libraries(public-header-reverse-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    add_executable(xbox-proof-key-smoke tools/xbox_proof_key_smoke.cpp)
    target_link_libraries(xbox-proof-key-smoke PRIVATE BedrockProtocol::bedrock_protocol)

    if(BUILD_TESTING)
        add_test(NAME protocol-roundtrip COMMAND protocol-roundtrip)
        add_test(NAME snappy-codec-smoke COMMAND snappy-codec-smoke)
        add_test(NAME nbt-codec-smoke COMMAND nbt-codec-smoke)
        add_test(NAME protodef-nbt-smoke COMMAND protodef-nbt-smoke)
        add_test(NAME protodef-variables-smoke COMMAND protodef-variables-smoke)
        add_test(NAME protodef-structured-smoke COMMAND protodef-structured-smoke)
        add_test(NAME subchunk-packet-smoke COMMAND subchunk-packet-smoke)
        add_test(NAME msal-native-smoke COMMAND msal-native-smoke)
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
        add_test(NAME raknet-server-smoke COMMAND raknet-server-smoke)
        add_test(NAME raknet-connection-lifecycle-smoke COMMAND raknet-connection-lifecycle-smoke)
        add_test(NAME server-login-verifier-smoke COMMAND server-login-verifier-smoke)
        add_test(NAME raknet-client-smoke COMMAND raknet-client-smoke)
        add_test(NAME network-client-smoke COMMAND network-client-smoke)
        add_test(NAME live-relay-smoke COMMAND live-relay-smoke)
        add_test(NAME live-relay-multi-client-smoke COMMAND live-relay-multi-client-smoke)
        add_test(NAME live-relay-options-smoke COMMAND live-relay-options-smoke)
        add_test(NAME public-header-smoke COMMAND public-header-smoke)
        add_test(NAME public-header-reverse-smoke COMMAND public-header-reverse-smoke)
        add_test(NAME xbox-proof-key-smoke COMMAND xbox-proof-key-smoke)
        set_tests_properties(
            protocol-roundtrip
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
            raknet-server-smoke
            raknet-connection-lifecycle-smoke
            server-login-verifier-smoke
            raknet-client-smoke
            network-client-smoke
            live-relay-smoke
            live-relay-multi-client-smoke
            live-relay-options-smoke
            public-header-smoke
            public-header-reverse-smoke
            xbox-proof-key-smoke
            PROPERTIES
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                TIMEOUT 120
        )
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
        install(TARGETS bedrock bedrock-session login xbox-login ping protocol-roundtrip msal-native-smoke live-token-manager-smoke bedrock-token-managers-smoke xbox-token-manager-smoke microsoft-auth-flow-smoke ping-api-smoke create-server-factory-smoke server-outbound-queue-smoke create-client-factory-smoke legacy-encryption-smoke raknet-server-smoke raknet-connection-lifecycle-smoke server-login-verifier-smoke raknet-client-smoke network-client-smoke live-relay-smoke live-relay-multi-client-smoke live-relay-options-smoke chunk-smoke
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
