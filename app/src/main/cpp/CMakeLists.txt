cmake_minimum_required(VERSION 3.22)
project(hellovr)

find_package(OpenXR REQUIRED CONFIG)

add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue)

add_library(hellovr SHARED main.cpp)

target_link_libraries(hellovr
    native_app_glue
    OpenXR::openxr_loader
    android
    EGL
    GLESv3
    log)

# android_native_app_glue relies on the linker not stripping this symbol,
# since it's only referenced from the NativeActivity's JNI glue.
target_link_options(hellovr PRIVATE "-u" "ANativeActivity_onCreate")
