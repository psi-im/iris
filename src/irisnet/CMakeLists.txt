cmake_minimum_required(VERSION 3.10.0)

list(APPEND CMAKE_MODULE_PATH
    "${CMAKE_SOURCE_DIR}/cmake/modules"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules"
)

add_library(irisnet STATIC)

target_sources(irisnet PRIVATE
    corelib/irisnetglobal.cpp
    corelib/irisnetplugin.cpp

    noncore/icetransport.cpp
    noncore/stunmessage.cpp
    noncore/stuntypes.cpp
    noncore/stunutil.cpp

    noncore/cutestuff/bytestream.cpp
    noncore/cutestuff/httpconnect.cpp
    noncore/cutestuff/httppoll.cpp
    noncore/cutestuff/socks.cpp

    noncore/legacy/ndns.cpp
    noncore/legacy/srvresolver.cpp

    corelib/addressresolver.cpp
    corelib/netavailability.cpp
    corelib/netinterface.cpp
    corelib/netnames.cpp
    corelib/objectsession.cpp
    corelib/netinterface_qtname.cpp
    corelib/netinterface_qtnet.cpp

    noncore/iceagent.cpp
    noncore/ice176.cpp
    noncore/icecomponent.cpp
    noncore/icelocaltransport.cpp
    noncore/iceturntransport.cpp
    noncore/processquit.cpp
    noncore/stunallocate.cpp
    noncore/stunbinding.cpp
    noncore/stuntransaction.cpp
    noncore/turnclient.cpp
    noncore/udpportreserver.cpp
    noncore/tcpportreserver.cpp
    noncore/dtls.cpp

    noncore/cutestuff/bsocket.cpp
)

if(UNIX)
    target_sources(irisnet PRIVATE
        corelib/netinterface_unix.cpp
    )
endif()

if(IRIS_ENABLE_JINGLE_SCTP)
    target_sources(irisnet PRIVATE
        noncore/sctp/SctpAssociation.cpp
        noncore/sctp/DepUsrSCTP.cpp
    )
endif()

if(IRIS_BUNDLED_QCA)
    add_dependencies(irisnet QcaProject)
endif()

if(IRIS_ENABLE_JINGLE_SCTP AND IRIS_BUNDLED_USRSCTP)
    add_dependencies(irisnet UsrSCTPProject)
endif()

target_include_directories(irisnet
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include/iris
        ${CMAKE_SOURCE_DIR}/src
        ${Qca_INCLUDE_DIR}
    PUBLIC
        ${USRSCTP_INCLUDES}
)

target_compile_definitions(irisnet PRIVATE HAVE_QTNET)

if(WIN32)
    target_link_libraries(irisnet PUBLIC ws2_32 iphlpapi)
endif()

# UsrSCTP requires pthread
if(NOT WIN32)
    find_package(Threads)
    target_link_libraries(irisnet PUBLIC Threads::Threads)
endif()

target_link_libraries(irisnet PUBLIC
    ${Qca_LIBRARY} ${USRSCTP_LIBRARY}
    Qt5::Core Qt5::Network Qt5::Xml
)
