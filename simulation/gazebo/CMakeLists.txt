cmake_minimum_required(VERSION 3.16)
project(d3k_gazebo_simulator VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(gz-sim8 REQUIRED)
find_package(gz-plugin2 REQUIRED COMPONENTS register)
find_package(Threads REQUIRED)

set(D3K_FIRMWARE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../dwm3000-firmware/UwbNode")

add_library(d3k_gazebo_system SHARED
  src/D3kNodeSystem.cc
  ${D3K_FIRMWARE_DIR}/Protocol.cpp
)
target_include_directories(d3k_gazebo_system PRIVATE
  "${D3K_FIRMWARE_DIR}"
)
target_link_libraries(d3k_gazebo_system PRIVATE
  gz-sim8::gz-sim8
  gz-plugin2::gz-plugin2
  Threads::Threads
  util
)
target_compile_features(d3k_gazebo_system PRIVATE cxx_std_17)
set_target_properties(d3k_gazebo_system PROPERTIES
  OUTPUT_NAME d3k_gazebo_system
  POSITION_INDEPENDENT_CODE ON
)

include(GNUInstallDirs)
install(TARGETS d3k_gazebo_system
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

include(CTest)
if(BUILD_TESTING)
  add_executable(d3k_protocol_codec_test
    tests/ProtocolCodecTest.cpp
    ${D3K_FIRMWARE_DIR}/Protocol.cpp
  )
  target_include_directories(d3k_protocol_codec_test PRIVATE "${D3K_FIRMWARE_DIR}")
  target_compile_features(d3k_protocol_codec_test PRIVATE cxx_std_17)
  add_test(NAME d3k_protocol_codec_test COMMAND d3k_protocol_codec_test)
endif()
