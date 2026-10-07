# The ndskit runtime, included by the generated CMakeLists.txt.
#   ndskit_core     main RAM, the TCMs, the ARM7's WRAM, the processor modes, dispatch over the modules
#   ndskit_stub     no hardware: tests that only call the game's own code
#   ndskit_machine  the DS: both CPUs in time, threads, interrupts, the BIOS, the I/O and its devices,
#                   the 2D engines, the geometry engine, the renderer and the window (SDL3, OpenGL 4.5)
# A target links ndskit_core, then ndskit_stub or ndskit_machine, then recomp.
set(_rt ${CMAKE_CURRENT_LIST_DIR})
add_library(ndskit_core OBJECT ${_rt}/core.cpp)
add_library(ndskit_stub OBJECT ${_rt}/services_stub.cpp)
add_library(ndskit_machine OBJECT
    ${_rt}/coro.cpp ${_rt}/machine.cpp ${_rt}/bios.cpp ${_rt}/mmio.cpp ${_rt}/system.cpp
    ${_rt}/video.cpp ${_rt}/gpu2d.cpp ${_rt}/gx.cpp ${_rt}/render_gl.cpp ${_rt}/display.cpp ${_rt}/png.cpp
    ${_rt}/cart.cpp ${_rt}/spi.cpp ${_rt}/sound.cpp ${_rt}/input.cpp)
find_package(SDL3 REQUIRED CONFIG)
target_link_libraries(ndskit_machine PUBLIC SDL3::SDL3)
foreach(t ndskit_core ndskit_stub ndskit_machine)
  target_include_directories(${t} PUBLIC ${_rt})
endforeach()
