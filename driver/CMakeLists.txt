cmake_minimum_required(VERSION 3.16)
project(furimodem_iq_driver CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(SoapySDR REQUIRED)

# Core reused by the module and by the selftest.
add_library(furimodem_core STATIC
    SoapyFurimodemIQ.cpp
    ccb_transport.cpp
    iq_window.cpp
)
target_include_directories(furimodem_core PUBLIC ${SoapySDR_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}
                                                ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_link_libraries(furimodem_core PUBLIC ${SoapySDR_LIBRARIES})
set_target_properties(furimodem_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(furimodemIQ MODULE $<TARGET_OBJECTS:furimodem_core>)
target_link_libraries(furimodemIQ PRIVATE ${SoapySDR_LIBRARIES})
set_target_properties(furimodemIQ PROPERTIES PREFIX "lib" OUTPUT_NAME "furimodemIQ")

enable_testing()

# IQ ring reader on the phone (same class as the driver; root and hardware)
add_executable(iqring_pull tools/iqring_pull.cpp)
target_link_libraries(iqring_pull PRIVATE furimodem_core)

# IQ ring reader tests (simulated memory, no hardware)
add_executable(selftest_iqwindow selftest_iqwindow.cpp)
target_link_libraries(selftest_iqwindow PRIVATE furimodem_core)
add_test(NAME iqwindow COMMAND selftest_iqwindow)

# readSetting("rx_ring"): the three states the app turns into three different messages.
add_executable(selftest_ring_state selftest_ring_state.cpp)
target_link_libraries(selftest_ring_state PRIVATE furimodem_core)
add_test(NAME ring_state COMMAND selftest_ring_state)

# Receive transport selftest (simulated CCB META DL ring)
add_executable(selftest_transport selftest_transport.cpp)
target_link_libraries(selftest_transport PRIVATE furimodem_core)
add_test(NAME transport COMMAND selftest_transport)

install(TARGETS furimodemIQ LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/SoapySDR/modules0.8)
