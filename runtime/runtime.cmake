# The ndskit runtime, included by the generated CMakeLists.txt.
#   ndskit_core  main RAM, the TCMs, the ARM7's WRAM, the processor modes, dispatch over the modules
#   ndskit_stub  no hardware: tests that only call the game's own code
# A target links ndskit_core, ndskit_stub, then recomp.
set(_rt ${CMAKE_CURRENT_LIST_DIR})
add_library(ndskit_core OBJECT ${_rt}/core.cpp)
add_library(ndskit_stub OBJECT ${_rt}/services_stub.cpp)
foreach(t ndskit_core ndskit_stub)
  target_include_directories(${t} PUBLIC ${_rt})
endforeach()
