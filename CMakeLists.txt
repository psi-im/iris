cmake_minimum_required(VERSION 3.10.0)

project(iris
    DESCRIPTION "XMPP network library"
    LANGUAGES C CXX
)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
endif()

# Set 'd' prefix for Windows OS debug builds
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(WIN32)
        set(D "d")
    elseif(APPLE)
        set(D "_debug")
    endif()
endif()

# Check if Iris is a subproject
get_directory_property(IS_SUBPROJECT PARENT_DIRECTORY)

set(IRIS_LIB_VERSION_MAJOR 1)
set(IRIS_LIB_VERSION_MINOR 0)
set(IRIS_LIB_VERSION_PATCH 0)
set(IRIS_LIB_VERSION_STRING
    ${IRIS_LIB_VERSION_MAJOR}.${IRIS_LIB_VERSION_MINOR}.${IRIS_LIB_VERSION_PATCH}
)

list(APPEND CMAKE_MODULE_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules"
    "${CMAKE_SOURCE_DIR}/cmake/modules"
)

include(policyRules)

set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_CXX_STANDARD 11)

# Detect MXE cross-compilation
set(IRIS_DEFAULT_BUNDLED_USRSCTP OFF)
if(NOT DEFINED USE_MXE AND (EXISTS "${CMAKE_TOOLCHAIN_FILE}"))
    option(USE_MXE "Use MXE (cross-compilation build environment for MS Windows)" OFF)
    string(TOLOWER ${CMAKE_TOOLCHAIN_FILE} TOOLCHAIN_FILE)
    string(REGEX MATCH "mxe-conf" MXE_DETECTED "${TOOLCHAIN_FILE}")
    if(MXE_DETECTED)
        message(STATUS "MXE environment detected")
        message(STATUS "MXE toolchain: ${CMAKE_TOOLCHAIN_FILE}")
        message(STATUS "MXE root path: ${CMAKE_PREFIX_PATH}")
        set(USE_MXE ON)
        set(IRIS_DEFAULT_BUNDLED_USRSCTP ON)
    endif()
endif()

set(IRIS_DEFAULT_BUNDLED_QCA OFF)
if(APPLE OR (MSVC OR USE_MXE))
    set(IRIS_DEFAULT_BUNDLED_QCA ON)
endif()

option(IRIS_ENABLE_INSTALL "Enable installation" OFF)
option(IRIS_ENABLE_JINGLE_SCTP "Enable SCTP over ICE Jingle transport / data channels" ON)
option(IRIS_BUNDLED_QCA "Adds: DTLS, Blake2b and other useful for XMPP crypto-stuff" ${IRIS_DEFAULT_BUNDLED_QCA})
option(IRIS_BUNDLED_USRSCTP "Compile compatible UsrSCTP lib (required for datachannel Jingle transport)" ${IRIS_DEFAULT_BUNDLED_USRSCTP})
option(IRIS_BUILD_TOOLS "Build tools and examples" OFF)
option(IRIS_ENABLE_DEBUG "Enable debugging code paths" OFF)

set(CMAKE_CXX_STANDARD 17)

if("${CMAKE_BUILD_TYPE}" STREQUAL "Debug" OR ("${CMAKE_BUILD_TYPE}" STREQUAL "RelWithDebInfo"))
    include(debug-definitions)
endif()

find_package(Qt5 COMPONENTS Core Gui Xml Network REQUIRED)

if(Qt5Core_FOUND)
    message(STATUS "Qt5 found, version ${Qt5Core_VERSION}")
    if(IRIS_BUNDLED_QCA AND ("${Qt5Core_VERSION}" VERSION_LESS "5.6"))
        message(FATAL_ERROR "Minimal Qt version for bundled Qca is 5.6. Please disable IRIS_BUNDLED_QCA option")
    endif()
endif()

# Find QCA package or use bundled if enabled
include(IrisQCA)

set(STDINT_FOUND OFF)
find_file(STDINT_INCLUDE stdint.h)

if(NOT ${STDINT_INCLUDE} STREQUAL "STDINT_INCLUDE-NOTFOUND")
    message(STATUS "StdInt include found: ${STDINT_INCLUDE}")
    set(STDINT_FOUND ON)
endif()

# Find SCTP package or use bundled if enabled
if(IRIS_ENABLE_JINGLE_SCTP)
    include(IrisSCTP)
endif()

if(NOT IRIS_BUNDLED_QCA)
    find_package(B2 QUIET)
    if(B2_FOUND)
        message(STATUS "Found B2: ${B2_LIBRARY}")
    endif()
endif()

add_subdirectory(3rdparty/stringprep)
add_subdirectory(src/irisnet)
add_subdirectory(src/xmpp)

if(IRIS_BUILD_TOOLS)
    if(NOT IRIS_BUNDLED_QCA)
        message(FATAL_ERROR "Bundled Qca is needed to build tools")
    endif()
    add_subdirectory(tools)
endif()

if(NOT IS_SUBPROJECT)
    include(fix-codestyle)
endif()

if(IRIS_ENABLE_INSTALL)
    include(CMakePackageConfigHelpers)
    include(GNUInstallDirs)

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/IrisConfigVersion.cmake
        VERSION ${IRIS_LIB_VERSION_STRING}
        COMPATIBILITY SameMajorVersion
    )
    configure_file(
        iris.pc.in
        ${CMAKE_CURRENT_BINARY_DIR}/iris.pc
        @ONLY
    )

    install(TARGETS iris
        EXPORT iris
        LIBRARY DESTINATION  ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION  ${CMAKE_INSTALL_LIBDIR}
    )
    install(EXPORT iris
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Iris
        NAMESPACE iris::
        FILE IrisConfig.cmake
    )
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/IrisConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Iris
    )
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/iris.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
    )

    set(IRISNET_CORELIB_HEADERS
        src/irisnet/corelib/addressresolver.h
        src/irisnet/corelib/irisnetexport.h
        src/irisnet/corelib/irisnetglobal.h
        src/irisnet/corelib/irisnetplugin.h
        src/irisnet/corelib/netavailability.h
        src/irisnet/corelib/netinterface.h
        src/irisnet/corelib/netnames.h
        src/irisnet/corelib/objectsession.h
    )
    set(IRISNET_NONCORE_HEADERS
        src/irisnet/noncore/cutestuff/bsocket.h
        src/irisnet/noncore/cutestuff/bytestream.h
        src/irisnet/noncore/cutestuff/httpconnect.h
        src/irisnet/noncore/cutestuff/httppoll.h
        src/irisnet/noncore/cutestuff/socks.h
        src/irisnet/noncore/dtls.h
        src/irisnet/noncore/ice176.h
        src/irisnet/noncore/iceabstractstundisco.h
        src/irisnet/noncore/iceagent.h
        src/irisnet/noncore/legacy/ndns.h
        src/irisnet/noncore/legacy/srvresolver.h
        src/irisnet/noncore/processquit.h
        src/irisnet/noncore/stunallocate.h
        src/irisnet/noncore/stunbinding.h
        src/irisnet/noncore/stunmessage.h
        src/irisnet/noncore/stuntransaction.h
        src/irisnet/noncore/tcpportreserver.h
        src/irisnet/noncore/turnclient.h
        src/irisnet/noncore/udpportreserver.h
    )
    set(XMPP_JID_HEADERS
        src/xmpp/jid/jid.h
    )
    set(XMPP_CORE_HEADERS
        src/xmpp/xmpp-core/xmpp.h
        src/xmpp/xmpp-core/xmpp_clientstream.h
        src/xmpp/xmpp-core/xmpp_stanza.h
        src/xmpp/xmpp-core/xmpp_stream.h
    )
    set(XMPP_IM_HEADERS
        src/xmpp/xmpp-im/filetransfer.h
        src/xmpp/xmpp-im/httpfileupload.h
        src/xmpp/xmpp-im/im.h
        src/xmpp/xmpp-im/jingle-application.h
        src/xmpp/xmpp-im/jingle-ft.h
        src/xmpp/xmpp-im/jingle-ice.h
        src/xmpp/xmpp-im/jingle-nstransportslist.h
        src/xmpp/xmpp-im/jingle-s5b.h
        src/xmpp/xmpp-im/jingle-session.h
        src/xmpp/xmpp-im/jingle-transport.h
        src/xmpp/xmpp-im/jingle.h
        src/xmpp/xmpp-im/s5b.h
        src/xmpp/xmpp-im/xmpp_address.h
        src/xmpp/xmpp-im/xmpp_agentitem.h
        src/xmpp/xmpp-im/xmpp_bitsofbinary.h
        src/xmpp/xmpp-im/xmpp_caps.h
        src/xmpp/xmpp-im/xmpp_captcha.h
        src/xmpp/xmpp-im/xmpp_chatstate.h
        src/xmpp/xmpp-im/xmpp_client.h
        src/xmpp/xmpp-im/xmpp_discoinfotask.h
        src/xmpp/xmpp-im/xmpp_discoitem.h
        src/xmpp/xmpp-im/xmpp_encryption.h
        src/xmpp/xmpp-im/xmpp_encryptionhandler.h
        src/xmpp/xmpp-im/xmpp_features.h
        src/xmpp/xmpp-im/xmpp_hash.h
        src/xmpp/xmpp-im/xmpp_htmlelement.h
        src/xmpp/xmpp-im/xmpp_httpauthrequest.h
        src/xmpp/xmpp-im/xmpp_liveroster.h
        src/xmpp/xmpp-im/xmpp_liverosteritem.h
        src/xmpp/xmpp-im/xmpp_message.h
        src/xmpp/xmpp-im/xmpp_muc.h
        src/xmpp/xmpp-im/xmpp_pubsubitem.h
        src/xmpp/xmpp-im/xmpp_pubsubretraction.h
        src/xmpp/xmpp-im/xmpp_reference.h
        src/xmpp/xmpp-im/xmpp_resource.h
        src/xmpp/xmpp-im/xmpp_resourcelist.h
        src/xmpp/xmpp-im/xmpp_roster.h
        src/xmpp/xmpp-im/xmpp_rosteritem.h
        src/xmpp/xmpp-im/xmpp_rosterx.h
        src/xmpp/xmpp-im/xmpp_serverinfomanager.h
        src/xmpp/xmpp-im/xmpp_status.h
        src/xmpp/xmpp-im/xmpp_subsets.h
        src/xmpp/xmpp-im/xmpp_task.h
        src/xmpp/xmpp-im/xmpp_tasks.h
        src/xmpp/xmpp-im/xmpp_thumbs.h
        src/xmpp/xmpp-im/xmpp_url.h
        src/xmpp/xmpp-im/xmpp_vcard.h
        src/xmpp/xmpp-im/xmpp_xdata.h
        src/xmpp/xmpp-im/xmpp_xmlcommon.h
    )

    set(IRIS_INSTALL_INCLUDEDIR ${CMAKE_INSTALL_INCLUDEDIR}/xmpp/iris)
    install(FILES ${IRISNET_CORELIB_HEADERS}
        DESTINATION ${IRIS_INSTALL_INCLUDEDIR}/irisnet/corelib
    )
    install(FILES ${IRISNET_NONCORE_HEADERS}
        DESTINATION ${IRIS_INSTALL_INCLUDEDIR}/irisnet/noncore
    )
    install(FILES ${XMPP_JID_HEADERS}
        DESTINATION ${IRIS_INSTALL_INCLUDEDIR}/xmpp/jid
    )
    install(FILES ${XMPP_CORE_HEADERS}
        DESTINATION ${IRIS_INSTALL_INCLUDEDIR}/xmpp/xmpp-core
    )
    install(FILES ${XMPP_IM_HEADERS}
        DESTINATION ${IRIS_INSTALL_INCLUDEDIR}/xmpp/xmpp-im
    )
    install(DIRECTORY include/iris
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/xmpp
        FILES_MATCHING PATTERN "*.h"
    )
endif()
