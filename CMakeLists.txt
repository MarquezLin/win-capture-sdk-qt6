cmake_minimum_required(VERSION 3.16)
project(win_capture_sdk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
# set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

# ---- NVAPI root: 給整個專案共用 ----
set(NVAPI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nvapi")

add_library(gcapture SHARED
    src/core/capture_manager.cpp
    src/core/frame_converter.cpp
    src/core/c_api.cpp
    src/providers/winmf_provider.cpp
    src/providers/mf_recorder.cpp
    src/providers/dshow_provider.cpp 
    src/audio/audio_manager.cpp
    src/core/exports.def
)

target_include_directories(gcapture PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Windows / Media Foundation
if (WIN32)
  target_compile_definitions(gcapture PRIVATE GCAP_WIN_MF GCAP_WIN_DSHOW)
#   target_compile_definitions(gcapture PRIVATE GCAP_WIN_MF)
  target_compile_definitions(gcapture PRIVATE GCAPTURE_BUILD)

#   # ---- NVAPI 路徑設定 ----
#   target_include_directories(gcapture PRIVATE "${NVAPI_ROOT}/include")

  # ★ 把常見會漏的系統庫一次補齊
  target_link_libraries(gcapture PRIVATE
      mfplat mf mfuuid mfreadwrite      # Media Foundation
      ole32 propsys                     # COM + PROPVAR
      oleaut32                          # 有時會被 propvar/COM 轉到
      uuid                              # 一些 COM/IID GUID
      user32 advapi32                   # 低機率需要，但安全
      setupapi                          #
      dxgi d3d11 d2d1 dwrite            # Direct3D
      D3DCompiler                       # Direct3D
      strmiids                          # DirectShow IIDs
    #   "${NVAPI_ROOT}/lib/x64/nvapi64.lib"   # NVAPI
  )

  # 檔案含中文/UTF-8
  if (MSVC)
    target_compile_options(gcapture PRIVATE /utf-8)
    # ★ 讓連結器印出「哪個符號沒解析」，方便定位
    # target_link_options(gcapture PRIVATE /VERBOSE:UNRESOLVED)
  endif()
endif()

set_target_properties(gcapture PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}   # .dll
  ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}   # .lib（import lib）
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# Demo
add_subdirectory(demos/qt6_viewer)


