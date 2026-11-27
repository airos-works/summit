package works.airos.summit.browser

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.graphics.ImageBitmap
import org.wpewebkit.wpeview.WebView

/**
 * A tab. Its page ([webView]) exists only while the tab is "live": Summit
 * keeps a bounded number of pages and discards the least recently used ones
 * (keeping the address, title and a picture), as air/OS's Summit unloads tabs
 * when memory is low. A discarded tab loads again when it is shown.
 */
class Tab(val id: Long, val private: Boolean, url: String, title: String = "") {
  var url by mutableStateOf(url)
  var title by mutableStateOf(title)
  var progress by mutableIntStateOf(0)
  var loading by mutableStateOf(false)
  var canGoBack by mutableStateOf(false)
  var canGoForward by mutableStateOf(false)
  /** The page as it was last seen, for the tab switcher. */
  var picture by mutableStateOf<ImageBitmap?>(null)
  /** Whether the page asked for (and was given) the whole screen. */
  var fullscreen by mutableStateOf(false)
  /** Asks sites for their desktop layout (the desktop user agent). */
  var desktopSite by mutableStateOf(false)

  var webView: WebView? = null
    internal set
  /** Uptime when the tab was last shown, for choosing which pages to discard. */
  var lastShown: Long = 0
    internal set

  val live: Boolean get() = webView != null

  val displayTitle: String
    get() = title.ifBlank { url.ifBlank { "New Tab" } }
}
