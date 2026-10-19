cmake_minimum_required(VERSION 3.22)
project(AnomalyUltimateReview LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(anomaly_ultimate_review SHARED
    plugin.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../../sdk/cpp/plugin_cxx_throw_bridge.cpp")
target_include_directories(anomaly_ultimate_review PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../.."
    "${CMAKE_CURRENT_LIST_DIR}/../../include")
target_compile_definitions(anomaly_ultimate_review PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
    target_compile_options(anomaly_ultimate_review PRIVATE
        /utf-8 /W4 /permissive- /EHsc /Zc:threadSafeInit-)
endif()
set_target_properties(anomaly_ultimate_review PROPERTIES
    OUTPUT_NAME "plugin" PREFIX "")

add_executable(anomaly_ultimate_identity_check identity_check.cpp)
target_include_directories(anomaly_ultimate_identity_check PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../include")
target_compile_definitions(anomaly_ultimate_identity_check PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
    target_compile_options(anomaly_ultimate_identity_check PRIVATE
        /utf-8 /W4 /permissive- /EHsc)
endif()
