# Add project specific ProGuard rules here.
# You can control the set of applied configuration files using the
# proguardFiles setting in build.gradle.
#
# For more details, see
#   http://developer.android.com/guide/developing/tools/proguard.html

# If your project uses WebView with JS, uncomment the following
# and specify the fully qualified class name to the JavaScript interface
# class:
#-keepclassmembers class fqcn.of.javascript.interface.for.webview {
#   public *;
#}

# Uncomment this to preserve the line number information for
# debugging stack traces.
#-keepattributes SourceFile,LineNumberTable

# If you keep the line number information, uncomment this to
# hide the original source file name.
#-renamesourcefileattribute SourceFile

-keep class com.arm.aichat.* { *; }
-keep class com.arm.aichat.gguf.* { *; }

# pdfbox-android's JPXFilter optionally uses com.gemalto.jp2.JP2Decoder for JPEG2000 images,
# a dependency it doesn't bundle -- we don't need JPEG2000 support, only plain text extraction
-dontwarn com.gemalto.jp2.**

-assumenosideeffects class android.util.Log {
    public static int v(...);
    public static int d(...);
}
