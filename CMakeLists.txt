cmake_minimum_required(VERSION 3.21)
if (MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()

project(SyncPlay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(LibDataChannel CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_path(MINIAUDIO_INCLUDE_DIRS "miniaudio.h")

set(MPV_ROOT "C:/packages/mpv" CACHE PATH "mpv root")

# Win32 + D3D11 ImGui migration target (Windows only)
if (WIN32)
    add_executable(SyncPlay
            src/main.cpp
            SyncPlay.rc
            platform/platform.cpp
            render/render_sw.cpp
            media/mpv_helpers.cpp
            ui/app_state.cpp
            ui/ui_helpers.cpp
            ui/panel_utils.cpp
            ui/panels.cpp
            platform/file_dialog.cpp
            core/utf.cpp
            sync/sync_session.cpp
            sync/sync_manager.cpp
            network/signaling_client.cpp
            network/signaling_server.cpp
            network/relay_voice.cpp
            )
endif()

set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")
add_library(imgui_vendor
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/imgui_demo.cpp
        )
target_include_directories(imgui_vendor PUBLIC ${IMGUI_DIR})
target_compile_definitions(imgui_vendor PUBLIC IMGUI_USE_WCHAR32)

if (WIN32)
    target_sources(SyncPlay PRIVATE
            ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
            ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
            )
endif()

if (WIN32)
    set_source_files_properties(SyncPlay.rc PROPERTIES
            OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/SyncPlay.ico")

    target_link_libraries(SyncPlay
            PRIVATE
            imgui_vendor
            LibDataChannel::LibDataChannel
            nlohmann_json::nlohmann_json
            ${MPV_ROOT}/lib/mpv.lib
            d3d11
            dxgi
            d2d1
            dwrite
            comdlg32
            ole32
            shell32
            user32
            gdi32
            iphlpapi
            dwmapi
            windowscodecs
            )
endif()

if (WIN32)
    target_compile_definitions(SyncPlay PRIVATE RTC_STATIC NOMINMAX IMGUI_USE_WCHAR32)
endif()

if (WIN32)
    target_include_directories(SyncPlay
            PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${MPV_ROOT}/include
            ${IMGUI_DIR}
            ${MINIAUDIO_INCLUDE_DIRS}
            )
endif()

if (WIN32)
    set_target_properties(SyncPlay PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

if (WIN32)
    add_custom_command(TARGET SyncPlay POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:SyncPlay>/assets
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/SyncPlay.png
            $<TARGET_FILE_DIR:SyncPlay>/assets/SyncPlay.png
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/SyncPlay.ico
            $<TARGET_FILE_DIR:SyncPlay>/assets/SyncPlay.ico)
endif()
