find_package(glfw3 3.3 CONFIG REQUIRED)
find_package(OpenGL REQUIRED)
if(NOT IS_DIRECTORY "${IMGUI_SOURCE_DIR}" OR NOT IS_DIRECTORY "${IMPLOT_SOURCE_DIR}")
  message(FATAL_ERROR "Set IMGUI_SOURCE_DIR to Dear ImGui v1.91.9b-docking and IMPLOT_SOURCE_DIR to ImPlot v0.16")
endif()
add_library(pl64-imgui STATIC
  ${IMGUI_SOURCE_DIR}/imgui.cpp ${IMGUI_SOURCE_DIR}/imgui_draw.cpp
  ${IMGUI_SOURCE_DIR}/imgui_widgets.cpp ${IMGUI_SOURCE_DIR}/imgui_tables.cpp
  ${IMGUI_SOURCE_DIR}/backends/imgui_impl_glfw.cpp ${IMGUI_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
  ${IMPLOT_SOURCE_DIR}/implot.cpp ${IMPLOT_SOURCE_DIR}/implot_items.cpp)
target_include_directories(pl64-imgui SYSTEM PUBLIC ${IMGUI_SOURCE_DIR} ${IMGUI_SOURCE_DIR}/backends ${IMPLOT_SOURCE_DIR})
target_link_libraries(pl64-imgui PUBLIC glfw OpenGL::GL)
add_executable(pl64-control gui.cpp)
target_link_libraries(pl64-control PRIVATE pl64-service-common pl64-imgui)
target_compile_options(pl64-control PRIVATE -Wall -Wextra -Wpedantic)
if(PL64_FONT_PATH)
  target_compile_definitions(pl64-control PRIVATE PL64_FONT_PATH="${PL64_FONT_PATH}")
endif()
install(TARGETS pl64-control RUNTIME DESTINATION bin)
