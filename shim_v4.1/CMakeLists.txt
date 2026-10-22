cmake_minimum_required(VERSION 3.18)
project(panvk_shim C)

set(CMAKE_C_STANDARD 11)

# Produce libvulkan_panvk_shim.so
add_library(vulkan_panvk_shim SHARED src/panvk_shim.c)
target_compile_options(vulkan_panvk_shim PRIVATE -Wall -Wextra -fvisibility=hidden)
# 1 = build de diagnostico (rastreo + PROF + log a stderr); 0 = build limpia.
set(PANVK_SHIM_DIAG 1 CACHE STRING "Diagnostico del shim: 1 o 0")
target_compile_definitions(vulkan_panvk_shim PRIVATE PANVK_SHIM_DIAG=${PANVK_SHIM_DIAG})
target_link_libraries(vulkan_panvk_shim PRIVATE log dl)

# Prueba aislada para adb shell
add_executable(test_panvk_shim test/test_shim.c)
target_compile_options(test_panvk_shim PRIVATE -Wall -Wextra)
target_link_libraries(test_panvk_shim PRIVATE dl)
