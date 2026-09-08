cmake_minimum_required(VERSION 3.20)
project(WaferMaster VERSION 0.1.0 LANGUAGES CXX)

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

# 编译后自动拷贝 OpenCV DLL 到输出目录
set(OPENCV_BIN_DIR "F:/opencv/opencv/build/x64/vc16/bin")
add_custom_command(TARGET WaferMaster POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${OPENCV_BIN_DIR}/opencv_world4120d.dll"
        "${OPENCV_BIN_DIR}/opencv_world4120.dll"
        $<TARGET_FILE_DIR:WaferMaster>
    COMMENT "Copying OpenCV DLLs to output directory"
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
