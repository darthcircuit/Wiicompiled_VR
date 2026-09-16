import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// Translator output for the disc the user owns (data_sections_init.cpp,
// RuntimeConfig.h, build_shards/shards.cmake). Never checked in.
val mkwGeneratedDir = providers.gradleProperty("mkwGeneratedDir").orNull
// Optional: a directory of already-fetched dependency sources (the installer's
// BuildWorkspace/Dependencies) so the native configure does not download them.
val mkwDependenciesDir = providers.gradleProperty("mkwDependenciesDir").orNull
// Optional: a directory holding a second SDL3 AAR/prefab is not needed; the
// AAR in app/libs is produced by Prepare-QuestDependencies.ps1.
val mkwRepoRoot = rootProject.file("..").canonicalFile

// The runtime's read-only assets travel inside the APK and are unpacked by
// QuestActivity into the app's private storage on first launch.
val runtimeResources = layout.buildDirectory.dir("generated/assets/runtimeResources")
val prepareRuntimeResources by tasks.registering(Copy::class) {
    val assets = File(mkwRepoRoot, "runtime/assets")
    from(File(assets, "wii")) { into("runtime_resources/wii_bootstrap") }
    from(File(assets, "dsp/dsp_coef.bin")) { into("runtime_resources") }
    from(File(assets, "pipeline/initial_pipeline_cache.db")) { into("runtime_resources") }
    into(runtimeResources)
}

android {
    namespace = "org.wiicompiled.quest"
    compileSdk = 36
    ndkVersion = "29.0.14206865"

    defaultConfig {
        applicationId = "org.wiicompiled.quest"
        // Quest 2 ships Android 10 (API 29); AHardwareBuffer/Vulkan 1.1 need 26+.
        minSdk = 29
        targetSdk = 34
        versionCode = 1
        versionName = "0.1.0-quest"

        ndk {
            abiFilters += "arm64-v8a"
        }
        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DANDROID_PLATFORM=android-29",
                    "-DMKW_REPO_ROOT=${mkwRepoRoot.path.replace('\\', '/')}",
                )
                if (mkwGeneratedDir != null) {
                    arguments += "-DMKW_GENERATED_DIR=${File(mkwGeneratedDir).canonicalPath.replace('\\', '/')}"
                }
                if (mkwDependenciesDir != null) {
                    arguments += "-DMKW_DEPENDENCIES_DIR=${File(mkwDependenciesDir).canonicalPath.replace('\\', '/')}"
                }
            }
        }
    }

    flavorDimensions += "profile"
    productFlavors {
        create("base") {
            dimension = "profile"
            buildConfigField("String", "MAIN_LIBRARY", "\"main\"")
            buildConfigField("String", "PROFILE", "\"base\"")
            externalNativeBuild { cmake { targets += "WiiCompiled" } }
        }
        create("retroRewind") {
            dimension = "profile"
            applicationIdSuffix = ".retrorewind"
            versionNameSuffix = "-retro-rewind"
            buildConfigField("String", "MAIN_LIBRARY", "\"main_retro_rewind\"")
            buildConfigField("String", "PROFILE", "\"retro_rewind\"")
            externalNativeBuild { cmake { targets += "RetroRewind" } }
        }
    }

    externalNativeBuild {
        cmake {
            // aurora-main needs CMake 3.25+, newer than the SDK's bundled 3.22.1;
            // Build-Quest.ps1 points cmake.dir in local.properties at a system CMake.
            path = file("src/main/cpp/CMakeLists.txt")
        }
    }

    buildFeatures {
        prefab = true
        buildConfig = true
    }

    sourceSets.named("main") {
        assets.srcDir(runtimeResources)
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    packaging {
        jniLibs {
            useLegacyPackaging = false
        }
    }
    lint {
        disable += setOf("ChromeOsAbiSupport", "DiscouragedApi")
    }
}

tasks.named("preBuild") {
    dependsOn(prepareRuntimeResources)
}

kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_17)
    }
}

dependencies {
    // SDLActivity and libSDL3.so; the AAR is downloaded by Prepare-QuestDependencies.ps1.
    implementation(files("libs/SDL3-3.4.4.aar"))
}
