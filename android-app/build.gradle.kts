// 根构建脚本：只声明插件版本，不在这里配依赖（依赖统一放 :app）。
plugins {
    id("com.android.application") version "8.4.2" apply false
    id("org.jetbrains.kotlin.android") version "1.9.24" apply false
}
