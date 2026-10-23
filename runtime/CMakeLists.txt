cmake_minimum_required(VERSION 3.20)
project(ProStreetRecomp CXX OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(PPC_DIR ${ROOT}/ppc)
set(SIMDE_DIR ${ROOT}/tools/XenonRecomp/thirdparty/simde)

# Código recompilado del juego (generado por XenonRecomp en ppc/; no está en el repositorio).
file(GLOB PPC_SOURCES CONFIGURE_DEPENDS ${PPC_DIR}/ppc_recomp.*.cpp ${PPC_DIR}/ppc_func_mapping.cpp)
if(NOT PPC_SOURCES)
    message(FATAL_ERROR "No hay código en ${PPC_DIR}: ejecuta XenonRecomp primero (ver README).")
endif()

add_library(ppc_recomp STATIC ${PPC_SOURCES})
target_include_directories(ppc_recomp PUBLIC ${PPC_DIR} ${SIMDE_DIR})
# -ffp-contract=off: no fusionar mul+add que el juego hace por separado (cambiaría resultados).
# -fno-strict-aliasing: el código generado accede a la misma memoria con tipos distintos.
target_compile_options(ppc_recomp PUBLIC -fno-strict-aliasing -ffp-contract=off)
target_compile_options(ppc_recomp PRIVATE -w -g0 -include ${CMAKE_CURRENT_SOURCE_DIR}/ppc_overrides.h)

add_executable(prostreet
    main.cpp
    memory.cpp
    debug/hooks.cpp
    kernel/filesystem.cpp
    kernel/imports.cpp
    kernel/input.cpp
    kernel/objects.cpp
    kernel/sync.cpp
    kernel/threads.cpp
    kernel/user.cpp
    kernel/video.cpp
    kernel/xam.cpp
    gpu/command_processor.cpp
    gpu/presenter.cpp
    gpu/renderer.mm
    gpu/shader_translator.cpp
    platform/window.mm
    platform/input.mm
    kernel/xboxkrnl.cpp
    kernel/xex.cpp
    kernel/xma.cpp
    generated/imports.cpp
)
target_include_directories(prostreet PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(prostreet PRIVATE -g)
target_link_libraries(prostreet PRIVATE ppc_recomp
    "-framework Cocoa" "-framework Metal" "-framework QuartzCore" "-framework ImageIO"
    "-framework CoreGraphics" "-framework UniformTypeIdentifiers" "-framework GameController")
set_source_files_properties(platform/window.mm platform/input.mm gpu/renderer.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
