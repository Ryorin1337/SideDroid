import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.plugin.compose")
}

// The release key and its properties stay under the user's Android profile.
val signingFile = providers.gradleProperty("signingProperties").orNull?.let { file(it) }
    ?: rootProject.file("keystore.properties")

android {
    namespace = "com.sidedroid.app"
    compileSdk = 37
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = "com.sidedroid.app"
        minSdk = 26
        targetSdk = 36
        // Only ever goes up from here: Android refuses to install a lower one over an existing app.
        versionCode = 5
        versionName = "0.1.1"
        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_static", "-DSIDEDROID_BUILD_TESTS=OFF")
                cppFlags += "-std=c++20"
            }
        }
    }

    // One smaller APK per CPU type, and a universal one for anyone unsure which to pick.
    splits {
        abi {
            isEnable = true
            reset()
            include("arm64-v8a", "armeabi-v7a", "x86_64")
            isUniversalApk = true
        }
    }

    // AndroidX ships a 32-bit x86 library but ours isn't built for it. Left in, the
    // universal APK would install on such a device and then crash at startup.
    packaging { jniLibs { excludes += "lib/x86/**" } }

    signingConfigs {
        if (signingFile.exists()) {
            create("release") {
                val p = Properties().apply { signingFile.inputStream().use { load(it) } }
                storeFile = file(p.getProperty("storeFile"))
                storeType = p.getProperty("storeType", "JKS")
                storePassword = p.getProperty("storePassword")
                keyAlias = p.getProperty("keyAlias")
                keyPassword = p.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            // Without the key the APKs come out unsigned, never signed with a debug key.
            signingConfig = signingConfigs.findByName("release")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildFeatures { compose = true }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    // The source directory shared with the Windows console.
    sourceSets["main"].kotlin.directories.add("../../ui/design/src")
}

dependencies {
    implementation(platform("androidx.compose:compose-bom:2026.09.00"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.foundation:foundation")
    // The same Material 3 base as Compose Multiplatform 1.12.2 on the console.
    implementation("androidx.compose.material3:material3:1.5.0-alpha22")
    implementation("androidx.activity:activity-compose:1.11.0")
    implementation("androidx.core:core-ktx:1.17.0")
}
