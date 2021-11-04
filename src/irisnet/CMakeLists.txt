cmake_minimum_required(VERSION 3.2.0)
get_filename_component(ABS_PARENT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include_directories(
    ${CMAKE_CURRENT_BINARY_DIR}
    corelib
    noncore
    noncore/cutestuff
    noncore/legacy
    ${QCA_INCLUDES}
    ${sctpLab_INCLUDES}
)

set( CMAKE_MODULE_PATH
    "${CMAKE_MODULE_PATH}"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules"
    "${ABS_PARENT_DIR}/cmake/modules"
)

set(SOURCES
    corelib/irisnetglobal.cpp
    corelib/irisnetplugin.cpp

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
    corelib/iputil.cpp

    noncore/processquit.cpp

    noncore/ice/icetransport.cpp
    noncore/ice/iceagent.cpp
    noncore/ice/ice176.cpp
    noncore/ice/icecomponent.cpp
    noncore/ice/icelocaltransport.cpp
    noncore/ice/iceturntransport.cpp
    noncore/ice/icecandidate.cpp
    noncore/ice/localaddress.cpp

    noncore/stun/stunmessage.cpp
    noncore/stun/stuntypes.cpp
    noncore/stun/stunutil.cpp
    noncore/stun/stunsession.cpp
    noncore/stun/stunallocate.cpp
    noncore/stun/stunbinding.cpp
    noncore/stun/stuntransaction.cpp
    noncore/stun/turnclient.cpp

    noncore/udpportreserver.cpp
    noncore/tcpportreserver.cpp
    noncore/dtls.cpp

    noncore/cutestuff/bsocket.cpp
)

if(UNIX)
    list(APPEND SOURCES corelib/netinterface_unix.cpp)
endif()

set(HEADERS
    corelib/irisnetexport.h
    corelib/irisnetglobal.h
    corelib/irisnetglobal_p.h

    corelib/addressresolver.h
    corelib/irisnetplugin.h
    corelib/netavailability.h
    corelib/netinterface.h
    corelib/netnames.h
    corelib/objectsession.h
    corelib/iputil.h

    noncore/processquit.h
    noncore/udpportreserver.h
    noncore/tcpportreserver.h
    noncore/dtls.h

    noncore/ice/iceagent.h
    noncore/ice/ice176.h
    noncore/ice/icecomponent.h
    noncore/ice/icelocaltransport.h
    noncore/ice/icetransport.h
    noncore/ice/iceturntransport.h
    noncore/ice/icecandidate.h
    noncore/ice/localaddress.h
    noncore/ice/iceabstractstundisco.h

    noncore/stun/stunmessage.h
    noncore/stun/stuntypes.h
    noncore/stun/stunutil.h
    noncore/stun/stunallocate.h
    noncore/stun/stunbinding.h
    noncore/stun/stuntransaction.h
    noncore/stun/stunsession.h
    noncore/stun/turnclient.h

    noncore/cutestuff/bsocket.h
    noncore/cutestuff/bytestream.h
    noncore/cutestuff/httpconnect.h
    noncore/cutestuff/httppoll.h
    noncore/cutestuff/socks.h

    noncore/legacy/ndns.h
    noncore/legacy/srvresolver.h
    noncore/transportaddress.h
)

if(JINGLE_SCTP)
    list(APPEND SOURCES
        noncore/sctp/SctpAssociation.cpp
        noncore/sctp/DepUsrSCTP.cpp
        )

    list(APPEND HEADERS
        noncore/sctp/Logger.hpp
        noncore/sctp/SctpAssociation.hpp
        noncore/sctp/DepUsrSCTP.hpp
        noncore/sctp/RTC/DataConsumer.hpp
        noncore/sctp/RTC/DataProducer.hpp
        noncore/sctp/RTC/SctpParameters.hpp
        )
endif()

add_library(irisnet STATIC ${SOURCES} ${HEADERS} )

if(WIN32)
    set(EXTRA_LDFLAGS ws2_32 iphlpapi)
endif()

target_compile_definitions(irisnet PRIVATE HAVE_QTNET)
target_link_libraries(irisnet Qt5::Core Qt5::Network Qt5::Xml)
if (BUNDLED_QCA)
    add_dependencies(irisnet QcaProject)
endif()
target_link_libraries(irisnet ${qca_LIB})
target_link_libraries(irisnet ${sctpLab_LIBRARY})
target_link_libraries(irisnet ${EXTRA_LDFLAGS})

# usrsctp requires pthread
if(NOT WIN32)
    find_package(Threads)
    target_link_libraries(irisnet Threads::Threads)
endif()
