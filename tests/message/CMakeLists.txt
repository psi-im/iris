find_package(Qt${QT_DEFAULT_MAJOR_VERSION} REQUIRED COMPONENTS Core Xml)

add_executable(message_stanzaids_test stanzaids.cpp)
target_link_libraries(message_stanzaids_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME message_stanzaids COMMAND message_stanzaids_test)
set_tests_properties(message_stanzaids PROPERTIES TIMEOUT 10)
