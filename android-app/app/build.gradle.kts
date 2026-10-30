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
        // CI (.github/workflows/android-app.yml) passes -PappVersionCode /
        // -PappVersionName when cutting a release; plain local builds stay
        // at 1 / 1.0.
        versionCode = (findProperty("appVersionCode") as String?)?.toInt() ?: 1
        versionName = (findProperty("appVersionName") as String?) ?: "1.0"
    }

    // Release signing: a stable signature so CI-built releases install over
    // one another (Android refuses signature changes on upgrade). Default is
    // the committed "open" key in keys/ — see docs/SIGNING-KEY.md for what that
    // does and does not give you. Override with OT_KEYSTORE_FILE /
    // OT_STORE_PASSWORD / OT_KEY_ALIAS / OT_KEY_PASSWORD env vars (e.g. a
    // private key stored as repo secrets).
    val ksFile = System.getenv("OT_KEYSTORE_FILE")
    val ksPass = System.getenv("OT_STORE_PASSWORD") ?: "opentag"
    val ksAlias = System.getenv("OT_KEY_ALIAS") ?: "opentag"
    val ksKeyPass = System.getenv("OT_KEY_PASSWORD") ?: ksPass

    signingConfigs {
        create("release") {
            storeFile = if (ksFile != null) file(ksFile)
                        else rootProject.file("keys/opentag-release.p12")
            storePassword = ksPass
            keyAlias = ksAlias
            keyPassword = ksKeyPass
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("release")
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
