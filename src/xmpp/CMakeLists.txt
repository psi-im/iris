cmake_minimum_required(VERSION 3.10.0)

find_package(ZLIB REQUIRED)

add_library(iris)

target_sources(iris PRIVATE
    xmpp-core/compressionhandler.cpp
    xmpp-core/connector.cpp
    xmpp-core/parser.cpp
    xmpp-core/protocol.cpp
    xmpp-core/sm.cpp
    xmpp-core/stream.cpp
    xmpp-core/tlshandler.cpp
    xmpp-core/xmlprotocol.cpp
    xmpp-core/xmpp_stanza.cpp

    xmpp-im/client.cpp
    xmpp-im/filetransfer.cpp
    xmpp-im/httpfileupload.cpp
    xmpp-im/types.cpp
    xmpp-im/xmpp_bitsofbinary.cpp
    xmpp-im/xmpp_bytestream.cpp
    xmpp-im/xmpp_caps.cpp
    xmpp-im/xmpp_discoinfotask.cpp
    xmpp-im/xmpp_discoitem.cpp
    xmpp-im/xmpp_hash.cpp
    xmpp-im/xmpp_ibb.cpp
    xmpp-im/xmpp_reference.cpp
    xmpp-im/xmpp_serverinfomanager.cpp
    xmpp-im/xmpp_subsets.cpp
    xmpp-im/xmpp_task.cpp
    xmpp-im/xmpp_tasks.cpp
    xmpp-im/xmpp_vcard.cpp
    xmpp-im/xmpp_xdata.cpp
    xmpp-im/xmpp_xmlcommon.cpp
    xmpp-im/xmpp_encryption.cpp
    xmpp-im/xmpp_externalservicediscovery.cpp
    xmpp-im/stundisco.cpp

    xmpp-im/jingle.cpp
    xmpp-im/jingle-connection.cpp
    xmpp-im/jingle-session.cpp
    xmpp-im/jingle-application.cpp
    xmpp-im/jingle-transport.cpp
    xmpp-im/jingle-nstransportslist.cpp
    xmpp-im/jingle-ft.cpp
    xmpp-im/jingle-ice.cpp
    xmpp-im/jingle-s5b.cpp
    xmpp-im/jingle-ibb.cpp
    xmpp-im/jingle-file.cpp

    base/randomnumbergenerator.cpp
    base/timezone.cpp

    zlib/zlibcompressor.cpp
    zlib/zlibdecompressor.cpp

    blake2/blake2qt.cpp

    jid/jid.cpp

    sasl/digestmd5proplist.cpp
    sasl/digestmd5response.cpp
    sasl/plainmessage.cpp
    sasl/scramsha1message.cpp
    sasl/scramsha1response.cpp
    sasl/scramsha1signature.cpp

    xmpp-core/securestream.cpp
    xmpp-core/simplesasl.cpp

    xmpp-im/s5b.cpp
    xmpp-im/xmpp_features.cpp
)

if(IRIS_ENABLE_JINGLE_SCTP)
    target_compile_definitions(iris PRIVATE JINGLE_SCTP)
    target_sources(iris PRIVATE
        xmpp-im/jingle-sctp.cpp
        xmpp-im/jingle-sctp-association_p.cpp
        xmpp-im/jingle-webrtc-datachannel_p.cpp
    )
endif()

if(B2_FOUND)
    message(STATUS "Building with system blake2 library")
    target_link_libraries(iris PUBLIC ${B2_LIBRARY})
else()
    if(NOT IRIS_BUNDLED_QCA)
        message(STATUS "No system blake2 and bundled QCA is disabled. Expect slow hashing.")
    endif()
    target_sources(iris PRIVATE
            blake2/blake2b-ref.c
            blake2/blake2s-ref.c
            blake2/blake2.h
            blake2/blake2-impl.h
    )
endif()

target_link_libraries(iris
    PRIVATE
        $<BUILD_INTERFACE:stringprep>
        $<BUILD_INTERFACE:irisnet>
        ZLIB::ZLIB
    PUBLIC
        ${Qca_LIBRARY}
        Qt5::Core Qt5::Gui Qt5::Network Qt5::Xml
)

if(IRIS_BUNDLED_QCA)
    add_dependencies(iris QcaProject)
    target_include_directories(iris PUBLIC ${Qca_INCLUDE_DIR})
    target_link_libraries(iris PUBLIC OpenSSL::SSL)
endif()

if(IRIS_ENABLE_DEBUG)
    target_compile_definitions(iris PRIVATE XMPP_TEST)
endif()

target_compile_definitions(iris PUBLIC IRISNET_STATIC)

target_include_directories(iris
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include/iris
        ${CMAKE_SOURCE_DIR}/src
        ${Qca_INCLUDE_DIR}
)
