add_executable(QymEditor main.cpp ImGuiLayer.cpp)
target_link_libraries(QymEditor PRIVATE QymEngineLib imgui)
target_include_directories(QymEditor PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
