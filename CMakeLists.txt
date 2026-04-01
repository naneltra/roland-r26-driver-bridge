cmake_minimum_required(VERSION 3.20)
project(R26USBDriver VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0")

# Find libusb via pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)

# ============================================================================
# r26d - USB bridge daemon
# ============================================================================

add_executable(r26d
    src/daemon/main.c
    src/daemon/R26USB.c
)

target_include_directories(r26d PRIVATE
    ${LIBUSB_INCLUDE_DIRS}
    src/shared
)

target_link_directories(r26d PRIVATE
    ${LIBUSB_LIBRARY_DIRS}
)

target_link_libraries(r26d PRIVATE
    ${LIBUSB_LIBRARIES}
)

# The daemon links against homebrew libusb which is native arch only
set_target_properties(r26d PROPERTIES OSX_ARCHITECTURES "${CMAKE_SYSTEM_PROCESSOR}")

# ============================================================================
# R26Audio.driver - CoreAudio AudioServerPlugIn (universal binary)
# ============================================================================

add_library(R26Audio MODULE
    src/driver/R26AudioDriver.c
)

set_target_properties(R26Audio PROPERTIES
    OSX_ARCHITECTURES "arm64;x86_64"
    BUNDLE TRUE
    BUNDLE_EXTENSION "driver"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/src/driver/Info.plist"
    OUTPUT_NAME "R26Audio"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

target_link_libraries(R26Audio PRIVATE
    "-framework CoreAudio"
    "-framework CoreFoundation"
)

# Ensure Info.plist is in the bundle
add_custom_command(TARGET R26Audio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/src/driver/Info.plist"
        "$<TARGET_BUNDLE_DIR:R26Audio>/Contents/Info.plist"
    COMMENT "Copying Info.plist into driver bundle"
)

# ============================================================================
# Install targets
# ============================================================================

install(TARGETS r26d DESTINATION bin)
# The driver bundle is installed via the install.sh script to
# /Library/Audio/Plug-Ins/HAL/ which requires root.
