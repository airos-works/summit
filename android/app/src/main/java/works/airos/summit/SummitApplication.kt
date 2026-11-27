package works.airos.summit

import android.content.ComponentCallbacks2
import org.wpewebkit.WPEApplication
import works.airos.summit.browser.Browser

/**
 * The engine's auxiliary processes (web content, network) are services of
 * this app in their own processes; WPEApplication loads the right native
 * libraries for each. Only the main process runs the browser.
 */
class SummitApplication : WPEApplication() {
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
