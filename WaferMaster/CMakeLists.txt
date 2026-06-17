cmake_minimum_required(VERSION 3.20)
project(WaferMaster VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 启用 Qt 自动化 (AUTOMOC 用于 Q_OBJECT; UIC 由 qt_wrap_ui 显式处理)
set(CMAKE_AUTOMOC ON)
# set(CMAKE_AUTOUIC ON)  # 关闭：使用 qt_wrap_ui 替代，避免 .ui 文件路径查找问题
set(CMAKE_AUTORCC ON)

# 配置 OpenCV 路径 (D盘)
set(OpenCV_DIR "D:/opencv/opencv/build") 
find_package(OpenCV REQUIRED)

# 查找 Qt 6 组件
set(CMAKE_PREFIX_PATH "E:/Qt/6.10.2/msvc2022_64")
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)

# 引入 spdlog (本地源码，v1.14.1)
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
)

set(HEADERS
    include/Common.h
    include/WaferAlgorithm.h
    include/FrameProducer.h
    include/MainWindow.h
    include/RoiViewerDialog.h
)

set(UIS
    ui/WaferMaster.ui
)

# 预生成 ui_*.h 头文件（Configure 阶段生成，IDE 可识别）
qt_wrap_ui(GENERATED_UI_HDRS ${UIS})

add_executable(WaferMaster ${SOURCES} ${HEADERS} ${GENERATED_UI_HDRS})

# 将 CMake 构建目录加入 include 路径，使 IDE/IntelliSense 能找到自动生成的 ui_*.h
target_include_directories(WaferMaster PRIVATE ${CMAKE_BINARY_DIR})

# 链接库
target_link_libraries(WaferMaster
    PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    ${OpenCV_LIBS}
    spdlog::spdlog
)

# 编译后自动拷贝 OpenCV DLL 到输出目录
set(OPENCV_BIN_DIR "D:/opencv/opencv/build/x64/vc16/bin")
add_custom_command(TARGET WaferMaster POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${OPENCV_BIN_DIR}/opencv_world4120d.dll"
        "${OPENCV_BIN_DIR}/opencv_world4120.dll"
        $<TARGET_FILE_DIR:WaferMaster>
    COMMENT "Copying OpenCV DLLs to output directory"
)

# 编译后自动部署 Qt 运行时 DLL（windeployqt）
set(QT_DEPLOY_EXE "E:/Qt/6.10.2/msvc2022_64/bin/windeployqt.exe")
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
