import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
}

// Release signing: ../../signing/keystore.properties on your PC (gitignored, see README), or the ANDROID_KEYSTORE_*
// environment variables the release workflow sets from the repository's secrets. Without either, release is unsigned.
val signingProps = Properties().apply {
    val f = rootProject.file("../signing/keystore.properties")
    if (f.isFile) f.inputStream().use { load(it) }
}
fun signingValue(env: String, prop: String): String? = System.getenv(env) ?: signingProps.getProperty(prop)
val keystorePath = System.getenv("ANDROID_KEYSTORE_PATH")
    ?: signingProps.getProperty("storeFile")?.let { rootProject.file("../signing/$it").path }

android {
    namespace = "com.lscardinali.bf2mc"
    compileSdk {
        version = release(37)
    }

    defaultConfig {
        applicationId = "com.lscardinali.bf2mc"
        minSdk = 31
        targetSdk = 37
        // the release workflow numbers builds from the tag (v1.2.3) and the run
        versionCode = System.getenv("VERSION_CODE")?.toIntOrNull() ?: 1
        versionName = System.getenv("VERSION_NAME") ?: "1.0"

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
            java.directories.add("../../third_party/rexglue-sdk/thirdparty/sdl3/android-project/app/src/main/java")
        }
    }

    signingConfigs {
        if (keystorePath != null && file(keystorePath).isFile) {
            create("release") {
                storeFile = file(keystorePath)
                storePassword = signingValue("ANDROID_KEYSTORE_PASSWORD", "storePassword")
                keyAlias = signingValue("ANDROID_KEY_ALIAS", "keyAlias")
                keyPassword = signingValue("ANDROID_KEY_PASSWORD", "keyPassword")
            }
        }
    }

    buildTypes {
        release {
            signingConfig = signingConfigs.findByName("release")
            // R8: shrinks, optimizes and obfuscates the Java/Kotlin code (rules in src/main/keepRules), and drops
            // unused resources
            optimization {
                enable = true
            }
            isShrinkResources = true
            isDebuggable = false
            isJniDebuggable = false
            ndk {
                debugSymbolLevel = "NONE"  // the native libraries ship stripped
            }
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
}

// Debug builds keep the recompiled functions' names in libmain.so, for the stack dumps (src/thread_dump.cpp). Release
// strips them: about 75 MB less.
androidComponents {
    onVariants(selector().withBuildType("debug")) { variant ->
        variant.packaging.jniLibs.keepDebugSymbols.add("**/libmain.so")
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.activity)
}
