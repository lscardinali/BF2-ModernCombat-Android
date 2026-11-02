plugins {
    alias(libs.plugins.android.application)
}

android {
    namespace = "com.lscardinali.bf2mc"
    compileSdk {
        version = release(37)
    }

    defaultConfig {
        applicationId = "com.lscardinali.bf2mc"
        minSdk = 31
        targetSdk = 37
        versionCode = 1
        versionName = "1.0"

        ndk {
            abiFilters += "arm64-v8a"
        }
        externalNativeBuild {
            cmake {
                // librexruntime.so and libmain.so share one C++ runtime
                // Release in every variant: a Debug SDK runs the game at -O0, and names its library librexruntimed.so
                arguments += listOf("-DANDROID_STL=c++_shared", "-DCMAKE_BUILD_TYPE=Release")
                targets += listOf("main", "rexruntime")
            }
        }
    }

    ndkVersion = "30.0.16248370"
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "4.1.2"
        }
    }
    // SDL3's Java side (SDLActivity and friends), from the SDK's SDL checkout tools/fetch_deps.ps1 makes
    sourceSets {
        getByName("main") {
            java.srcDir("../../third_party/rexglue-sdk/thirdparty/sdl3/android-project/app/src/main/java")
        }
    }
    // Keep the recompiled functions' names in the stack dumps (src/thread_dump.cpp)
    packaging {
        jniLibs {
            keepDebugSymbols += "**/libmain.so"
        }
    }

    buildTypes {
        release {
            optimization {
                enable = false
            }
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
}
