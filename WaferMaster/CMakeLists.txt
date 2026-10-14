cmake_minimum_required(VERSION 3.20)
project(WaferMaster VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 启用 Qt 自动化 (AUTOMOC 用于 Q_OBJECT; UIC 由 qt_wrap_ui 显式处理)
set(CMAKE_AUTOMOC ON)
# set(CMAKE_AUTOUIC ON)  # 关闭：使用 qt_wrap_ui 替代，避免 .ui 文件路径查找问题
set(CMAKE_AUTORCC ON)

# 配置 OpenCV 路径
set(OpenCV_DIR "F:/opencv/opencv/build")
find_package(OpenCV REQUIRED)

# 查找 Qt 6 组件
set(CMAKE_PREFIX_PATH "G:/qt/6.10.2/msvc2022_64")
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Sql)

# 引入 spdlog (本地源码 v1.14.1)
# spdlog 源码放在项目根目录 spdlog/ 下，cmake 会自动编译成静态库 spdlog::spdlog
# 第二个参数指定外部项目的二进制输出目录，避免污染主构建目录根层
add_subdirectory(${CMAKE_SOURCE_DIR}/spdlog ${CMAKE_BINARY_DIR}/spdlog_build)

# 定义包含路径
include_directories(include ${OpenCV_INCLUDE_DIRS})

# 搜集源文件
set(SOURCES
    src/main.cpp
    src/WaferAlgorithm.cpp
    src/FrameProducer.cpp
    src/MainWindow.cpp
    src/RoiViewerDialog.cpp
    src/Logger.cpp
    src/CommunicationManager.cpp
    src/ResultLogger.cpp
    src/DeepWaferDetector.cpp
)

set(HEADERS
    include/Common.h
    include/WaferAlgorithm.h
    include/FrameProducer.h
    include/MainWindow.h
    include/RoiViewerDialog.h
    include/Logger.h 
    include/CommunicationManager.h
    include/ResultLogger.h
    include/DeepWaferDetector.h
)

set(UIS
    ui/WaferMaster.ui
)

# 预生成 ui_*.h 头文件（Configure 阶段生成，IDE 可识别）
qt_wrap_ui(GENERATED_UI_HDRS ${UIS})

add_executable(WaferMaster ${SOURCES} ${HEADERS} ${GENERATED_UI_HDRS}
    resources/resources.qrc
    resources/app.rc
)

# 将 CMake 构建目录加入 include 路径，使 IDE/IntelliSense 能找到自动生成的 ui_*.h
target_include_directories(WaferMaster PRIVATE ${CMAKE_BINARY_DIR})

# ============================================================================
# ONNX Runtime
# ============================================================================
set(ONNXRUNTIME_ROOT "${CMAKE_SOURCE_DIR}/onnxruntime")
target_include_directories(WaferMaster PRIVATE "${ONNXRUNTIME_ROOT}/include")

# 链接库
target_link_libraries(WaferMaster
    PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    Qt6::Sql
    ${OpenCV_LIBS}
    spdlog::spdlog
    "${ONNXRUNTIME_ROOT}/lib/onnxruntime.lib"
)

# 跟随当前构建配置，只复制实际链接的那套 OpenCV DLL。
add_custom_command(TARGET WaferMaster POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:opencv_world>"
        "$<TARGET_FILE_DIR:WaferMaster>"
    COMMENT "Copying matching OpenCV DLL to output directory"
)

# 编译后自动部署 Qt 运行时 DLL（windeployqt）
set(QT_DEPLOY_EXE "G:/qt/6.10.2/msvc2022_64/bin/windeployqt.exe")
add_custom_command(TARGET WaferMaster POST_BUILD
    COMMAND ${QT_DEPLOY_EXE} --no-translations --no-system-d3d-compiler
            "$<TARGET_FILE:WaferMaster>"
    COMMENT "Deploying Qt DLLs to output directory"
)

# MSVC 编译器设置
if(MSVC)
    target_compile_options(WaferMaster PRIVATE /W4 /utf-8)
    add_definitions(-D_USE_MATH_DEFINES)

    # ONNX Runtime 使用 Release 版 MSVC 运行库。
    # 即使当前是 Debug 构建，也要复制 Release CRT，避免 0xc0000142。
    # 从当前编译器路径找到 CRT，并在构建后复制到 exe 目录。
    get_filename_component(_msvc_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(_msvc_host_dir "${_msvc_bin_dir}" DIRECTORY)
    get_filename_component(_msvc_bin_root "${_msvc_host_dir}" DIRECTORY)
    get_filename_component(_msvc_version_dir "${_msvc_bin_root}" DIRECTORY)
    get_filename_component(_msvc_tools_dir "${_msvc_version_dir}" DIRECTORY)
    get_filename_component(_vc_tools_root "${_msvc_tools_dir}" DIRECTORY)
    get_filename_component(_vc_root "${_vc_tools_root}" DIRECTORY)
    file(GLOB _msvc_crt_candidates LIST_DIRECTORIES true
         "${_vc_root}/Redist/MSVC/*/x64/Microsoft.VC143.CRT")
    if(_msvc_crt_candidates)
        list(SORT _msvc_crt_candidates)
        list(REVERSE _msvc_crt_candidates)
        list(GET _msvc_crt_candidates 0 _msvc_crt_dir)
        foreach(_crt_name IN ITEMS
                msvcp140.dll msvcp140_1.dll
                vcruntime140.dll vcruntime140_1.dll
                concrt140.dll)
            if(EXISTS "${_msvc_crt_dir}/${_crt_name}")
                add_custom_command(TARGET WaferMaster POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${_msvc_crt_dir}/${_crt_name}"
                        "$<TARGET_FILE_DIR:WaferMaster>/${_crt_name}")
            endif()
        endforeach()
    else()
        message(WARNING "未找到 MSVC Release CRT，请安装匹配的 VC++ 运行库")
    endif()
endif()

# 运行时部署 ONNX Runtime DLL + 模型文件
add_custom_command(TARGET WaferMaster POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ONNXRUNTIME_ROOT}/lib/onnxruntime.dll"
        "$<TARGET_FILE_DIR:WaferMaster>"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:WaferMaster>/models"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/models/model_curved_fringe.onnx"
        "$<TARGET_FILE_DIR:WaferMaster>/models/model_curved_fringe.onnx"
    COMMENT "Deploying ONNX Runtime DLL and model")
