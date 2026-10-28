plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "opentag.app"
    compileSdk = 34

    defaultConfig {
        applicationId = "opentag.app"
        minSdk = 23
        targetSdk = 34
        versionCode = 1
        versionName = "1.0"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}

// No external dependencies: the app uses only platform APIs
// (android.bluetooth, android.widget, org.json), so it builds offline once
// the Android Gradle plugin itself has been fetched by Android Studio.
dependencies {}
