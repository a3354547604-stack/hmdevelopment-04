// 单模块工程：演示用的最小配网 App。多模块拆分对 1 个 module 只增加维护成本。
pluginManagement {
    repositories {
        // 阿里云镜像前置：国内网络访问 dl.google.com / repo1.maven.org 经常超时；
        // 镜像缺包时会自动回落到后面的官方仓库，不会掩盖依赖缺失。
        maven("https://maven.aliyun.com/repository/gradle-plugin")
        maven("https://maven.aliyun.com/repository/google")
        maven("https://maven.aliyun.com/repository/public")
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.PREFER_SETTINGS)
    repositories {
        maven("https://maven.aliyun.com/repository/google")
        maven("https://maven.aliyun.com/repository/public")
        google()
        mavenCentral()
    }
}

rootProject.name = "eldercare-provision"
include(":app")
