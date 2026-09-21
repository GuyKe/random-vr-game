plugins {
    id("com.android.application")
}

android {
    namespace = "com.randomvrgame.helloworld"
    compileSdk = 34
    ndkVersion = "26.1.10909125"

    defaultConfig {
        applicationId = "com.randomvrgame.helloworld"
        // Meta Quest requires a minimum of API 29 (Android 10).
        minSdk = 29
        targetSdk = 32
        versionCode = 1
        versionName = "1.0"

        // All shipping Quest headsets are arm64-v8a only.
        ndk {
            abiFilters += "arm64-v8a"
        }

        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++17"
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    // Required for Gradle to expose the openxr_loader_for_android AAR's
    // headers and .so to our CMake build via find_package(OpenXR).
    buildFeatures {
        prefab = true
    }
}

dependencies {
    implementation("org.khronos.openxr:openxr_loader_for_android:1.1.42")
}
