// 根构建脚本：只声明插件版本，不在这里配依赖（依赖统一放 :app）。
plugins {
    id("com.android.application") version "8.4.2" apply false
    id("org.jetbrains.kotlin.android") version "1.9.24" apply false
    // Paparazzi：用 layoutlib 在 JVM 里把 XML 布局渲染成 PNG，不需要模拟器/真机。
    // 为什么加它：安卓界面要跟鸿蒙版逐像素对齐，靠「改完让你装上看」这种闭环太慢。
    id("app.cash.paparazzi") version "1.3.5" apply false
}
