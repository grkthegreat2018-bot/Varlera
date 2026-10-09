cmake_minimum_required(VERSION 3.25)
project(Varlera C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# --- vcpkg-provided Vulkan (headers + loader) ---
set(_VCPKG_INC "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include")
set(_VCPKG_LIB "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib")
find_path(VULKAN_INCLUDE_DIR vulkan/vulkan.h PATHS "${_VCPKG_INC}" NO_DEFAULT_PATH REQUIRED)
find_library(VULKAN_LIBRARY NAMES vulkan-1 PATHS "${_VCPKG_LIB}" NO_DEFAULT_PATH REQUIRED)
find_package(glfw3 CONFIG REQUIRED)

# --- shaders ---
find_program(GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)
set(SHADER_SRC_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(SHADER_BIN_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_BIN_DIR})
set(SHADERS world.vert world_i.vert world.frag grid.frag shadow.vert ui.vert ui.frag)
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

add_executable(varlera
  src/main.c
  src/rk.c
  src/world.c
  src/camera.c
  src/edit.c
  src/panel.c
  src/model.c
)
add_dependencies(varlera shaders)
target_include_directories(varlera PRIVATE src ${VULKAN_INCLUDE_DIR})
target_link_libraries(varlera PRIVATE glfw ${VULKAN_LIBRARY})
target_compile_definitions(varlera PRIVATE SHADER_DIR="${SHADER_BIN_DIR}")

# Copy shaders next to the exe so it runs from any cwd
add_custom_command(TARGET varlera POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${SHADER_BIN_DIR} $<TARGET_FILE_DIR:varlera>/shaders)

if(MSVC)
  target_compile_options(varlera PRIVATE /W3 /wd4201)
  set_property(TARGET varlera PROPERTY VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:varlera>)
  if(VARLERA_ASAN)
    target_compile_options(varlera PRIVATE /fsanitize=address)
    target_link_options(varlera PRIVATE /fsanitize=address /INFERASANLIBS)
  endif()
endif()
