cmake_minimum_required(VERSION 3.20)
project(ToyRenderGraph LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ------------------------------------------------------------------
# 依赖（全部 vendored，除 Vulkan SDK 外零系统依赖）
# ------------------------------------------------------------------
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(external/glfw)

add_library(glm INTERFACE)
target_include_directories(glm INTERFACE external)

find_package(Vulkan REQUIRED)

if (MSVC)
    # 源码为 UTF-8（含中文注释），MSVC 默认按系统代码页解析会导致乱码/语法错误
    add_compile_options(/utf-8 /EHsc)
endif()

# ------------------------------------------------------------------
# Shaders：构建期用 glslc 编译为 SPIR-V
# ------------------------------------------------------------------
find_program(GLSLC_EXECUTABLE glslc HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)

set(SHADER_SOURCES
    shaders/shadow.vert
    shaders/shadow.frag
    shaders/scene.vert
    shaders/scene.frag
    shaders/fullscreen.vert
    shaders/blur.frag
    shaders/present.frag
)

set(SPIRV_OUTPUTS "")
foreach(shader IN LISTS SHADER_SOURCES)
    get_filename_component(shader_name "${shader}" NAME)
    set(spv "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader_name}.spv")
    add_custom_command(
        OUTPUT "${spv}"
        COMMAND ${GLSLC_EXECUTABLE} -O "${CMAKE_CURRENT_SOURCE_DIR}/${shader}" -o "${spv}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${shader}"
        COMMENT "Compiling shader: ${shader_name}"
    )
    list(APPEND SPIRV_OUTPUTS "${spv}")
endforeach()
add_custom_target(shaders DEPENDS ${SPIRV_OUTPUTS})

# ------------------------------------------------------------------
# 主程序
# ------------------------------------------------------------------
add_executable(ToyRenderGraph
    src/main.cpp
    src/vk/vk_device.cpp
    src/vk/vk_resources.cpp
    src/vk/vk_pipeline.cpp
    src/vk/vk_backend.cpp
)

target_include_directories(ToyRenderGraph PRIVATE src external)
target_link_libraries(ToyRenderGraph PRIVATE glfw glm Vulkan::Vulkan)

add_dependencies(ToyRenderGraph shaders)

# 把 .spv 复制到可执行文件旁的 shaders/ 目录（运行时按 exe 相对路径加载）
foreach(spv IN LISTS SPIRV_OUTPUTS)
    add_custom_command(TARGET ToyRenderGraph POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:ToyRenderGraph>/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${spv}" "$<TARGET_FILE_DIR:ToyRenderGraph>/shaders/"
    )
endforeach()

# ------------------------------------------------------------------
# 图核心单元测试（无 Vulkan 依赖，可跑 ctest）
# ------------------------------------------------------------------
enable_testing()
add_executable(graph_compile_test tests/graph_compile_test.cpp)
target_include_directories(graph_compile_test PRIVATE src)
add_test(NAME graph_compile_test COMMAND graph_compile_test)

add_executable(swap_extent_test tests/swap_extent_test.cpp src/vk/vk_device.cpp)
target_include_directories(swap_extent_test PRIVATE src external)
target_link_libraries(swap_extent_test PRIVATE glfw Vulkan::Vulkan)
add_test(NAME swap_extent_test COMMAND swap_extent_test)
