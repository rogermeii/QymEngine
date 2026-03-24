file(GLOB_RECURSE EDITOR_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)
add_executable(QymEditor ${EDITOR_SOURCES})
target_link_libraries(QymEditor PRIVATE QymEngineLib imgui d3d12 d3d11 dxgi)
target_include_directories(QymEditor PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "C:/Program Files/RenderDoc"
)
