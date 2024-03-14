cmake_minimum_required( VERSION 3.10 )
#Set automoc and autouic policy
if(NOT POLICY_SET)
    if(POLICY CMP0071)
        cmake_policy(SET CMP0071 NEW)
        message(STATUS "CMP0071 policy set to NEW")
    endif()
    if(POLICY CMP0074)
        cmake_policy(SET CMP0074 NEW)
        message(STATUS "CMP0074 policy set to NEW")
    endif()
    set(POLICY_SET ON)
endif()

