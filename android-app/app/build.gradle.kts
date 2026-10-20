plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.hmprogram.eldercare.provision"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.hmprogram.eldercare.provision"
        // 26 = Android 8.0：WifiNetworkSpecifier(29+)/WifiNetworkSuggestion(29+) 都做了分支降级，
        // 现场演示机普遍是 10+，但保留 26 可以直接装在旧备用机上。
        minSdk = 26
        targetSdk = 34
        versionCode = 2
        // 0.2.0：补齐 F4–F13（登录/绑定/首页/告警/报表/设置/Mock 自检），与鸿蒙 App 功能对齐
        versionName = "0.2.0"

        // 服务器地址默认指向演示笔记本上的 Nginx（契约 §6.1：http://{host}/api）。
        // 运行时可在 App「开发者设置」里改，不需要重新打包——现场 IP 一定会变。
        buildConfigField("String", "DEFAULT_SERVER_BASE", "\"http://192.168.1.100/api\"")

        // 首启是否直接进 mock 模式（真机演示时也可在设置里手动打开）
        buildConfigField("boolean", "DEFAULT_MOCK_MODE", "false")

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }

    buildTypes {
        release {
            // 演示包不混淆：R8 会拖长构建、且一旦 keep 规则漏了就出现「界面能开、点按钮崩溃」，
            // 对一次性的现场演示包没有收益。见根 gradle.properties 的说明。
            isMinifyEnabled = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
        debug {
            applicationIdSuffix = ".debug"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
    buildFeatures {
        viewBinding = true
        buildConfig = true
    }
    packaging {
        resources.excludes += setOf("META-INF/DEPENDENCIES", "META-INF/LICENSE*", "META-INF/NOTICE*")
    }
    testOptions {
        unitTests.isReturnDefaultValues = true
    }
    lint {
        // lint 失败会中断 assemble；这里只报告不阻断（构建说明里让用户单独跑 lint）
        abortOnError = false
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("com.google.android.material:material:1.12.0")
    implementation("androidx.constraintlayout:constraintlayout:2.1.4")
    implementation("androidx.recyclerview:recyclerview:1.3.2")
    implementation("androidx.activity:activity-ktx:1.9.0")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.3")
    implementation("androidx.lifecycle:lifecycle-viewmodel-ktx:2.8.3")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")

    // 网络：契约统一响应体 {code,message,data,ts} 里 data 是各接口异构的 object，
    // 所以用 OkHttp 手写请求 + Gson 解析 data，而不是给 18 个接口写 18 个 Retrofit interface。
    implementation("com.squareup.okhttp3:okhttp:4.12.0")
    implementation("com.google.code.gson:gson:2.11.0")

    // 扫码添加设备：用 ZXing 嵌入式（不依赖 Google Play 服务，内网/离线演示也能扫）
    implementation("com.journeyapps:zxing-android-embedded:4.3.0")

    testImplementation("junit:junit:4.13.2")
    // android.jar 里的 org.json 是抛异常的空壳，单测必须换成真实现
    testImplementation("org.json:json:20231013")
}
