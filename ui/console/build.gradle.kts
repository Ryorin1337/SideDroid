import org.jetbrains.compose.desktop.application.dsl.TargetFormat

plugins {
    kotlin("jvm") version "2.4.21"
    id("org.jetbrains.kotlin.plugin.compose") version "2.4.21"
    id("org.jetbrains.compose") version "1.12.2"
}

group = "com.sidedroid"
version = "0.2.0"

repositories {
    mavenCentral()
    google()
}

// The source directory shared with the Android app.
sourceSets["main"].kotlin.srcDir("../design/src")

dependencies {
    implementation(compose.desktop.currentOs)
    // Paired with Compose Multiplatform 1.12.2. Expressive APIs are alpha upstream.
    implementation("org.jetbrains.compose.material3:material3:1.12.0-alpha03")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-swing:1.10.2")
    implementation("org.jetbrains.kotlinx:kotlinx-serialization-json:1.9.0")
    testImplementation(kotlin("test"))
}

kotlin {
    jvmToolchain(17)
}

tasks.test {
    useJUnitPlatform()
}

// Draws every screen to PNG with no window and no host, for visual checks.
tasks.register<JavaExec>("renderScreens") {
    classpath = sourceSets["main"].runtimeClasspath
    mainClass.set("com.sidedroid.console.RenderScreensKt")
    args(layout.buildDirectory.dir("screens").get().asFile.absolutePath)
    systemProperty("java.awt.headless", "true")
}

compose.desktop {
    application {
        mainClass = "com.sidedroid.console.MainKt"
        nativeDistributions {
            targetFormats(TargetFormat.AppImage)
            packageName = "SideDroid"
            packageVersion = "0.2.0"
            description = "SideDroid console"
            vendor = "SideDroid"
            // The console only reads pipes and draws: keep the bundled runtime small.
            modules("java.base", "java.desktop", "java.logging", "java.instrument", "jdk.unsupported")
            windows {
                iconFile.set(project.file("icons/sidedroid.ico"))
            }
        }
        buildTypes.release.proguard {
            isEnabled.set(false)
        }
    }
}
