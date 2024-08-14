# 指定CMake的最小版本要求
cmake_minimum_required(VERSION 3.10)

# 项目名称和语言
project(MyFFmpegProject VERSION 1.0 LANGUAGES C)

# 包含头文件的搜索路径
include_directories(/usr/include/aarch64-linux-gnu)

# 添加编译选项，例如启用C99标准
set(CMAKE_C_STANDARD 99)

# 找到FFmpeg组件
find_package(PkgConfig REQUIRED)
pkg_check_modules(AVUTIL libavutil)
pkg_check_modules(AVCODEC libavcodec)
pkg_check_modules(AVFORMAT libavformat)
pkg_check_modules(SWSCALE libswscale)
pkg_check_modules(SWRESAMPLE libswresample)

# 将FFmpeg的头文件路径添加到包含目录
include_directories(${AVUTIL_INCLUDE_DIRS})
include_directories(${AVCODEC_INCLUDE_DIRS})
include_directories(${AVFORMAT_INCLUDE_DIRS})
include_directories(${SWSCALE_INCLUDE_DIRS})
include_directories(${SWRESAMPLE_INCLUDE_DIRS})

# 定义源文件变量
set(SOURCE_FILES
    main.c
    # 其他源文件路径
)

# 添加可执行文件
add_executable(${PROJECT_NAME} ${SOURCE_FILES})

# 链接FFmpeg库
target_link_libraries(${PROJECT_NAME}
    ${AVUTIL_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${SWSCALE_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
)

# 如果需要，可以添加其他CMake命令，例如设置C编译器标志
# set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall")