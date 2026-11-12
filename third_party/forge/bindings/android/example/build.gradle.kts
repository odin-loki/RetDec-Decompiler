plugins { id("com.android.application"); id("org.jetbrains.kotlin.android") }
android {
    namespace = "dev.forge.example"
    compileSdk = 34
    defaultConfig { applicationId = "dev.forge.example"; minSdk = 26; targetSdk = 34; versionCode = 1; versionName = "1.0" }
    sourceSets["main"].java.srcDirs("src/main/kotlin")
    compileOptions { sourceCompatibility = JavaVersion.VERSION_17; targetCompatibility = JavaVersion.VERSION_17 }
    kotlinOptions { jvmTarget = "17" }
}
dependencies { implementation(project(":forge")) }
