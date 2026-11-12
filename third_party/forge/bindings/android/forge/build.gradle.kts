// dev.forge:forge — AAR with libforge.so + libforge_jni.so (built from the repo root CMakeLists via the NDK).
plugins { id("com.android.library"); id("org.jetbrains.kotlin.android") }
android {
    namespace = "dev.forge"
    compileSdk = 34
    ndkVersion = "27.2.12479018"
    defaultConfig {
        minSdk = 26
        ndk { abiFilters += listOf("arm64-v8a", "x86_64") }
        externalNativeBuild { cmake { arguments += listOf("-DFORGE_JNI=ON", "-DFORGE_BUILD_TESTS=OFF", "-DANDROID_STL=c++_shared"); targets += listOf("forge_jni") } }
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }
    externalNativeBuild { cmake { path = file("../../../CMakeLists.txt"); version = "3.22.1+" } }
    sourceSets["main"].java.srcDirs("src/main/kotlin")
    sourceSets["test"].java.srcDirs("src/test/kotlin")
    sourceSets["androidTest"].java.srcDirs("src/test/kotlin")
    compileOptions { sourceCompatibility = JavaVersion.VERSION_17; targetCompatibility = JavaVersion.VERSION_17 }
    kotlinOptions { jvmTarget = "17" }
}
dependencies {
    testImplementation("junit:junit:4.13.2"); testImplementation(kotlin("test"))
    androidTestImplementation("androidx.test:runner:1.6.2"); androidTestImplementation("junit:junit:4.13.2"); androidTestImplementation(kotlin("test"))
}
