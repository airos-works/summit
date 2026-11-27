plugins {
  alias(libs.plugins.android.application)
  alias(libs.plugins.compose.compiler)
}

android {
  namespace = "works.airos.summit"
  compileSdk {
    version = release(37) { minorApiLevel = 1 }
  }
  ndkVersion = "30.0.16248370"

  defaultConfig {
    applicationId = "works.airos.summit"
    // Android 13; targetSdk follows Google Play's requirement
    // (https://developer.android.com/google/play/requirements/target-sdk).
    minSdk = 33
    targetSdk = 36
    versionCode = 1
    versionName = "0.1.0"
    externalNativeBuild {
      cmake {
        arguments += listOf("-DANDROID_STL=c++_shared", "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON")
      }
    }
  }

  externalNativeBuild {
    cmake {
      path = file("src/main/cpp/CMakeLists.txt")
    }
  }

  buildTypes {
    release {
      isMinifyEnabled = true
      proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
    }
  }

  compileOptions {
    sourceCompatibility = JavaVersion.VERSION_17
    targetCompatibility = JavaVersion.VERSION_17
  }

  buildFeatures {
    compose = true
    buildConfig = true
  }

  packaging {
    jniLibs {
      // GStreamer and the engine find their modules on disk.
      useLegacyPackaging = true
    }
    resources {
      excludes += "/META-INF/{AL2.0,LGPL2.1}"
    }
  }

  // One APK per ABI that has a staged engine (tools/android/stage-engine.py).
  val abis = listOf("arm64-v8a", "x86_64").filter {
    rootProject.file("wpeview/src/main/jniLibs/$it").isDirectory
  }
  splits {
    abi {
      isEnable = true
      reset()
      include(*abis.toTypedArray())
      isUniversalApk = false
    }
  }
}

kotlin {
  jvmToolchain(17)
}

dependencies {
  implementation(project(":wpeview"))

  val composeBom = platform(libs.androidx.compose.bom)
  implementation(composeBom)
  implementation(libs.androidx.core.ktx)
  implementation(libs.androidx.activity.compose)
  implementation(libs.androidx.lifecycle.runtime.ktx)
  implementation(libs.androidx.lifecycle.runtime.compose)
  implementation(libs.androidx.lifecycle.process)
  implementation(libs.androidx.compose.ui)
  implementation(libs.androidx.compose.material3)
  implementation(libs.androidx.compose.ui.tooling.preview)
  implementation(libs.androidx.window)
  implementation(libs.kotlinx.coroutines.android)
  debugImplementation(libs.androidx.compose.ui.tooling)

  testImplementation(libs.junit)
  androidTestImplementation(composeBom)
  androidTestImplementation(libs.androidx.test.ext.junit)
  androidTestImplementation(libs.androidx.test.runner)
  androidTestImplementation(libs.androidx.compose.ui.test.junit4)
  debugImplementation(libs.androidx.compose.ui.test.manifest)
}
