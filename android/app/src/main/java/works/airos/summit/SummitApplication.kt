package works.airos.summit

import android.content.ComponentCallbacks2
import android.system.Os
import android.util.Log
import java.io.File
import org.wpewebkit.WPEApplication
import works.airos.summit.browser.Browser

/**
 * The engine's auxiliary processes (web content, network) are services of
 * this app in their own processes; WPEApplication loads the right native
 * libraries for each. Only the main process runs the browser.
 */
class SummitApplication : WPEApplication() {
  override fun onCreate() {
    // Debug builds: the engine's SUMMIT_* switches, as on air/OS, from
    // files/summit.env (KEY=VALUE lines), set before the engine starts.
    if (BuildConfig.DEBUG) {
      runCatching {
        File(filesDir, "summit.env").takeIf { it.isFile }?.readLines()?.forEach { line ->
          val key = line.substringBefore('=', "").trim()
          if (key.isNotEmpty() && !key.startsWith("#")) {
            Os.setenv(key, line.substringAfter('='), true)
            Log.i("Summit", "environment: $key")
          }
        }
      }
    }
    super.onCreate()
  }

  override fun onTrimMemory(level: Int) {
    super.onTrimMemory(level)
    if (processKind == ProcessKind.MAIN) Browser.instanceOrNull()?.onTrimMemory(level)
  }

  @Suppress("OVERRIDE_DEPRECATION")
  override fun onLowMemory() {
    super.onLowMemory()
    if (processKind == ProcessKind.MAIN) Browser.instanceOrNull()?.onTrimMemory(ComponentCallbacks2.TRIM_MEMORY_COMPLETE)
  }
}
