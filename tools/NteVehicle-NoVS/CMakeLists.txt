cmake_minimum_required(VERSION 3.22)
project(NteVehicleNoVS LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "NTE Vehicle requires Windows x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT DEFINED ANOMALY_SOURCE_ROOT)
    get_filename_component(ANOMALY_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
endif()

find_package(AnomalySDK CONFIG REQUIRED)

set(NTE_VEHICLE_PACKAGE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/package/NteVehicle")

anomaly_add_plugin(anomaly_nte_vehicle
    SOURCES "${ANOMALY_SOURCE_ROOT}/plugins/NteVehicle/plugin.cpp"
    MANIFEST "${ANOMALY_SOURCE_ROOT}/plugins/NteVehicle/manifest.json"
    PACKAGE_NAME NteVehicle
    OUTPUT_DIRECTORY "${NTE_VEHICLE_PACKAGE_DIRECTORY}")

target_compile_definitions(anomaly_nte_vehicle PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(anomaly_nte_vehicle PRIVATE
        -fexceptions -frtti -Wall -Wextra -Wno-unused-parameter)
endif()
