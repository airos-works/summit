# The engine's native code calls into these classes through JNI.
-keep class org.wpewebkit.** { *; }
-keep class org.freedesktop.gstreamer.** { *; }
-keep class works.airos.summit.core.SummitCore { *; }
# Summit's extensions bridge: libsummit calls ExtensionsNative.onEvent by name,
# and its native methods are found by their names.
-keep class works.airos.summit.extensions.ExtensionsNative { *; }
