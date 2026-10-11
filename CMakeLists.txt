cmake_minimum_required(VERSION 3.25)
project(Varlera C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# --- Vulkan: vcpkg first (Windows), system SDK/distro package otherwise ---
if(DEFINED VCPKG_INSTALLED_DIR)
  set(_VCPKG_INC "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include")
  set(_VCPKG_LIB "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib")
  find_path(VULKAN_INCLUDE_DIR vulkan/vulkan.h PATHS "${_VCPKG_INC}" NO_DEFAULT_PATH)
  find_library(VULKAN_LIBRARY NAMES vulkan-1 vulkan PATHS "${_VCPKG_LIB}" NO_DEFAULT_PATH)
endif()
if(NOT VULKAN_INCLUDE_DIR OR NOT VULKAN_LIBRARY)
  find_package(Vulkan REQUIRED)          # Linux: libvulkan-dev / vulkan-headers
  set(VULKAN_INCLUDE_DIR ${Vulkan_INCLUDE_DIRS})
  set(VULKAN_LIBRARY ${Vulkan_LIBRARIES})
endif()
find_package(glfw3 CONFIG REQUIRED)      # Linux: libglfw3-dev ships glfw3Config.cmake

# --- shaders ---
find_program(GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin" REQUIRED)
set(SHADER_SRC_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(SHADER_BIN_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_BIN_DIR})
set(SHADERS world.vert world_i.vert world.frag grid.frag shadow.vert ui.vert ui.frag sky.vert sky.frag)
set(SPV_FILES "")
foreach(S ${SHADERS})
  set(IN  ${SHADER_SRC_DIR}/${S})
  set(OUT ${SHADER_BIN_DIR}/${S}.spv)
  add_custom_command(
    OUTPUT ${OUT}
    COMMAND ${GLSLC} ${IN} -o ${OUT}
    DEPENDS ${IN}
    COMMENT "glslc ${S}")
  list(APPEND SPV_FILES ${OUT})
endforeach()
add_custom_target(shaders ALL DEPENDS ${SPV_FILES})

add_executable(varlera WIN32
  src/main.c
  src/rk.c
  src/rk_vk.c
  src/meshgen.c
  src/world.c
  src/camera.c
  src/edit.c
  src/panel.c
  src/model.c
  src/tex.c
  src/art.c
  src/sculpt.c
  src/marching.c
  src/prof.c
  src/toast.c
  src/bundle.c
  src/log.c
  src/settings.c
)
add_dependencies(varlera shaders)
target_include_directories(varlera PRIVATE src ${VULKAN_INCLUDE_DIR})
target_link_libraries(varlera PRIVATE glfw ${VULKAN_LIBRARY})
if(WIN32)
  target_link_libraries(varlera PRIVATE dbghelp)   # crash minidump
endif()
if(UNIX AND NOT APPLE)
  target_link_libraries(varlera PRIVATE m dl pthread)
endif()
target_compile_definitions(varlera PRIVATE
  SHADER_DIR="${SHADER_BIN_DIR}"
  SHADER_SRC="${SHADER_SRC_DIR}"
  GLSLC="${GLSLC}")

# Copy shaders next to the exe so it runs from any cwd
add_custom_command(TARGET varlera POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${SHADER_BIN_DIR} $<TARGET_FILE_DIR:varlera>/shaders)

# --- unit tests (link only what they need; no Vulkan/glfw) ---
enable_testing()
add_executable(world_v4_test tests/world_v4.c src/world.c)
target_include_directories(world_v4_test PRIVATE src)
if(MSVC)
  target_compile_options(world_v4_test PRIVATE /W4 /wd4201)
endif()
if(UNIX AND NOT APPLE)
  target_link_libraries(world_v4_test PRIVATE m)
endif()
add_test(NAME world_v4 COMMAND world_v4_test
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

if(MSVC)
  target_compile_options(varlera PRIVATE /W4 /wd4201)
  # GUI subsystem but keep main() (stderr still works when redirected)
  target_link_options(varlera PRIVATE /ENTRY:mainCRTStartup)
  set_property(TARGET varlera PROPERTY VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:varlera>)
  if(VARLERA_ASAN)
    target_compile_options(varlera PRIVATE /fsanitize=address)
    target_link_options(varlera PRIVATE /fsanitize=address /INFERASANLIBS)
  endif()
endif()
