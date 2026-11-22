# Iris MUC lifecycle regressions; no Jingle dependency.
find_package(Qt${QT_DEFAULT_MAJOR_VERSION} REQUIRED COMPONENTS Core Xml)
add_executable(iris_muc_lifecycle_test lifecycle.cpp)
target_link_libraries(iris_muc_lifecycle_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME iris_muc_lifecycle COMMAND iris_muc_lifecycle_test)
set_tests_properties(iris_muc_lifecycle PROPERTIES TIMEOUT 15)
