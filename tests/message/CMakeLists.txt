find_package(Qt${QT_DEFAULT_MAJOR_VERSION} REQUIRED COMPONENTS Core Xml)

add_executable(message_occupantids_test occupantids.cpp)
target_link_libraries(message_occupantids_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME message_occupantids COMMAND message_occupantids_test)
set_tests_properties(message_occupantids PROPERTIES TIMEOUT 10)

add_executable(message_stanzaids_test stanzaids.cpp)
target_link_libraries(message_stanzaids_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME message_stanzaids COMMAND message_stanzaids_test)
set_tests_properties(message_stanzaids PROPERTIES TIMEOUT 10)

add_executable(message_delay_test delay.cpp)
target_link_libraries(message_delay_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME message_delay COMMAND message_delay_test)
set_tests_properties(message_delay PROPERTIES TIMEOUT 10)

add_executable(message_replies_test replies.cpp)
target_link_libraries(message_replies_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME message_replies COMMAND message_replies_test)
set_tests_properties(message_replies PROPERTIES TIMEOUT 10)
