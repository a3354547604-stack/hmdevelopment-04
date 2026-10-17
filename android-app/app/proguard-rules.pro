# 演示包未开启 minify（见 app/build.gradle.kts 的 isMinifyEnabled）。
# 若将来打开 release 混淆，以下 keep 规则是必需的：
-keep class com.hmprogram.eldercare.provision.data.model.** { *; }
-keepattributes Signature
-keepattributes *Annotation*
-dontwarn org.bouncycastle.**
-dontwarn org.conscrypt.**
-dontwarn org.openjsse.**
