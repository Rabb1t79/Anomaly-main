cmake_minimum_required(VERSION 3.22)
project(NteAttackReplayNoVS LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "NTE Attack Replay requires Windows x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT DEFINED ANOMALY_SOURCE_ROOT)
    get_filename_component(ANOMALY_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
endif()

find_package(AnomalySDK CONFIG REQUIRED)

set(PACKAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/package/NteAttackReplay")

anomaly_add_plugin(anomaly_nte_attack_replay
    SOURCES "${ANOMALY_SOURCE_ROOT}/plugins/NteAttackReplay/plugin.cpp"
    MANIFEST "${ANOMALY_SOURCE_ROOT}/plugins/NteAttackReplay/manifest.json"
    PACKAGE_NAME NteAttackReplay
    OUTPUT_DIRECTORY "${PACKAGE_DIR}")

target_compile_definitions(anomaly_nte_attack_replay PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(anomaly_nte_attack_replay PRIVATE
        -fexceptions -frtti -Wall -Wextra -Wno-unused-parameter)
endif()
 
# Normal-attack replay uses the Anomaly UE5 reflection/signature services and
# ProcessEvent path; it does not depend on Win32 input injection.
