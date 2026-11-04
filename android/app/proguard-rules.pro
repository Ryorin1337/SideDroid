# Called from native code by name.
-keep class com.sidedroid.app.NativeBridge { *; }
-keep interface com.sidedroid.app.NativeBridge$Listener { *; }
-keepclasseswithmembernames class * { native <methods>; }
