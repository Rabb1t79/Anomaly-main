cmake_minimum_required(VERSION 3.22)

project(NteVehicle LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "NTE Vehicle targets Windows x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(AnomalySDK CONFIG REQUIRED)

set(NTE_VEHICLE_PACKAGE_DIRECTORY "\${CMAKE_BINARY_DIR}/package/NteVehicle")

anomaly_add_plugin(anomaly_nte_vehicle
    SOURCES plugin.cpp
    MANIFEST manifest.json
    PACKAGE_NAME NteVehicle
    OUTPUT_DIRECTORY "\${NTE_VEHICLE_PACKAGE_DIRECTORY}")

target_compile_definitions(anomaly_nte_vehicle PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
