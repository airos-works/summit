package works.airos.summit.browser

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.graphics.ImageBitmap
import org.wpewebkit.wpeview.WebPage

/**
 * A tab. Its [page] exists only while the tab is "live": Summit keeps a
 * bounded number of pages and discards the least recently used ones, keeping
 * the address, title, picture and back/forward list ([state]), as air/OS's
 * Summit unloads tabs when memory is low. A discarded tab loads again when it
 * is shown.
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
  var icon by mutableStateOf<ImageBitmap?>(null)
  var playingAudio by mutableStateOf(false)
  /** The page's web process ended while the tab was shown. */
  var crashed by mutableStateOf(false)
  /** The tab whose page opened this one; Back on the first page closes this tab and returns there. */
  var openerId: Long = 0
  /** A blank tab the person opened: the address field takes the focus (and the keyboard comes up). */
  var focusAddress by mutableStateOf(false)
  /** Asks sites for their desktop layout (the desktop user agent). */
  var desktopSite by mutableStateOf(false)

  var page: WebPage? = null
    internal set
  /** The back/forward list of a discarded page. */
  internal var state: ByteArray? = null
  /** Uptime when the tab was last shown, for choosing which pages to discard. */
  var lastShown: Long = 0
    internal set

  val live: Boolean get() = page != null

  val displayTitle: String
    get() = title.ifBlank { url.ifBlank { "New Tab" } }
}
