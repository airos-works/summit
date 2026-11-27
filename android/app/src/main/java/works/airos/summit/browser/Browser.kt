package works.airos.summit.browser

import android.app.Activity
import android.app.ActivityManager
import android.content.ComponentCallbacks2
import android.content.Context
import android.graphics.Bitmap
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.graphics.asImageBitmap
import org.json.JSONArray
import org.json.JSONObject
import org.wpewebkit.wpeview.WebChromeClient
import org.wpewebkit.wpeview.WebContext
import org.wpewebkit.wpeview.WebView
import org.wpewebkit.wpeview.WebViewClient
import works.airos.summit.core.SummitCore

/**
 * The browser: tabs, the engine contexts they run in, and how much of the
 * device they may use.
 *
 * Resource policy (the "lean" part):
 * - At most [maxLivePages] pages exist at once, chosen from the device's RAM;
 *   opening or showing another discards the least recently shown page.
 * - Only the shown page has a surface; the engine treats the others as
 *   hidden (no painting, throttled timers and animation frames).
 * - Memory pressure from Android discards every page but the shown one, and
 *   when the app has been in the background for [BACKGROUND_DISCARD_MS], it
 *   discards those too, so an idle Summit costs nothing.
 * - Private tabs live in an ephemeral engine context that is destroyed with
 *   the last private tab, taking their cookies, storage and cache with it.
 */
class Browser private constructor(private val appContext: Context) {
  companion object {
    private const val TAG = "Summit"
    const val BACKGROUND_DISCARD_MS = 5 * 60 * 1000L
    private const val PICTURE_WIDTH = 480

    @Volatile private var instance: Browser? = null

    fun get(context: Context): Browser =
      instance ?: synchronized(this) { instance ?: Browser(context.applicationContext).also { instance = it } }

    fun instanceOrNull(): Browser? = instance
  }

  val tabs = mutableStateListOf<Tab>()
  var selected by mutableStateOf<Tab?>(null)
    private set
  /** Which list the tab switcher shows. */
  var showingPrivate by mutableStateOf(false)

  /** The activity whose window shows pages; WebViews need it for dialogs and the keyboard. */
  private var host: Activity? = null
  private var normalContext: WebContext? = null
  private var privateContext: WebContext? = null
  private var nextId = 1L
  private val handler = Handler(Looper.getMainLooper())
  private val backgroundDiscard = Runnable { discardBackgroundPages(keepSelected = false) }

  val maxLivePages: Int = run {
    val am = appContext.getSystemService(ActivityManager::class.java)
    val info = ActivityManager.MemoryInfo().also { am.getMemoryInfo(it) }
    val gb = info.totalMem / (1024.0 * 1024 * 1024)
    when {
      am.isLowRamDevice || gb < 3 -> 2
      gb < 5 -> 3
      gb < 7 -> 4
      gb < 11 -> 6
      else -> 8
    }
  }

  init {
    val result = JSONObject(SummitCore.open(appContext.filesDir.resolve("profile").absolutePath))
    if (result.optString("error").isNotEmpty()) Log.w(TAG, "profile: ${result.optString("error")}")
    Log.i(TAG, "keeping at most $maxLivePages pages")
  }

  // ---------------------------------------------------------------- hosting

  fun attach(activity: Activity) {
    host = activity
    handler.removeCallbacks(backgroundDiscard)
  }

  fun detach(activity: Activity) {
    if (host !== activity) return
    // The pages belong to the activity's window; without it they go.
    for (tab in tabs) discard(tab)
    host = null
  }

  /** The app went to the background: save, and let go of pages after a while. */
  fun onBackground() {
    saveSession()
    handler.removeCallbacks(backgroundDiscard)
    handler.postDelayed(backgroundDiscard, BACKGROUND_DISCARD_MS)
  }

  fun onForeground() {
    handler.removeCallbacks(backgroundDiscard)
    selected?.let { show(it) }
  }

  fun onTrimMemory(level: Int) {
    Log.i(TAG, "trim memory level $level")
    when {
      level >= ComponentCallbacks2.TRIM_MEMORY_COMPLETE -> discardBackgroundPages(keepSelected = false)
      level >= ComponentCallbacks2.TRIM_MEMORY_BACKGROUND -> discardBackgroundPages(keepSelected = true)
      level >= ComponentCallbacks2.TRIM_MEMORY_RUNNING_LOW -> discardBackgroundPages(keepSelected = true)
    }
  }

  // ---------------------------------------------------------------- tabs

  fun restoreSession() {
    if (tabs.isNotEmpty()) return
    val session = JSONObject(SummitCore.session())
    if (session.optBoolean("restore", true)) {
      val list = session.optJSONArray("tabs") ?: JSONArray()
      for (i in 0 until list.length()) {
        val t = list.getJSONObject(i)
        tabs += Tab(nextId++, false, t.optString("url"), t.optString("title"))
      }
      tabs.getOrNull(session.optInt("selected"))?.let { select(it) }
    }
    if (tabs.isEmpty()) newTab(homeUrl())
  }

  fun saveSession() {
    val normal = tabs.filter { !it.private }
    val session = JSONObject()
    session.put("tabs", JSONArray().apply {
      for (tab in normal) put(JSONObject().put("url", tab.url).put("title", tab.title))
    })
    session.put("selected", normal.indexOf(selected).coerceAtLeast(0))
    SummitCore.setSession(session.toString())
    val error = SummitCore.save()
    if (error.isNotEmpty()) Log.w(TAG, "saving the profile: $error")
  }

  fun homeUrl(): String = SummitCore.setting("homeURL").ifBlank { "about:blank" }

  fun newTab(url: String = "", private: Boolean = false, select: Boolean = true, after: Tab? = null): Tab {
    val tab = Tab(nextId++, private, url)
    val index = after?.let { tabs.indexOf(it) + 1 } ?: tabs.size
    tabs.add(index.coerceIn(0, tabs.size), tab)
    if (select) select(tab) else if (url.isNotEmpty()) Unit
    return tab
  }

  fun select(tab: Tab) {
    val previous = selected
    if (previous === tab) {
      show(tab)
      return
    }
    previous?.let { capturePicture(it) }
    selected = tab
    showingPrivate = tab.private
    show(tab)
  }

  fun close(tab: Tab) {
    val index = tabs.indexOf(tab)
    if (index < 0) return
    discard(tab)
    tabs.removeAt(index)
    if (selected === tab) {
      // The neighbour of the same kind, as Chrome does.
      val sameKind = tabs.filter { it.private == tab.private }
      val next = sameKind.getOrNull((sameKind.indexOfFirst { tabs.indexOf(it) >= index }).takeIf { it >= 0 } ?: (sameKind.size - 1))
        ?: tabs.lastOrNull()
      selected = null
      if (next != null) select(next) else newTab(homeUrl())
    }
    if (tabs.none { it.private }) endPrivateBrowsing()
  }

  fun closeAll(private: Boolean) {
    for (tab in tabs.filter { it.private == private }.toList()) close(tab)
  }

  fun move(from: Int, to: Int) {
    if (from !in tabs.indices || to !in tabs.indices) return
    tabs.add(to, tabs.removeAt(from))
  }

  // ---------------------------------------------------------------- navigation

  /** Goes where the address field text says: an address, a bookmark keyword or a search. */
  fun go(text: String, tab: Tab? = selected) {
    val target = tab ?: newTab()
    val resolved = SummitCore.resolve(text.trim())
    val url = resolved.url.ifEmpty { return }
    target.url = url
    val view = target.webView
    if (view != null) view.loadUrl(url) else if (target === selected) show(target)
  }

  // ---------------------------------------------------------------- pages

  /** Makes the tab's page live (creating it if it was discarded) and keeps within the budget. */
  private fun show(tab: Tab) {
    tab.lastShown = SystemClock.uptimeMillis()
    if (host == null) return
    if (tab.webView == null) createPage(tab)
    enforceBudget(keep = tab)
  }

  private fun createPage(tab: Tab) {
    val activity = host ?: return
    val context = contextFor(tab.private)
    val view = WebView(activity, context)
    tab.webView = view
    applySettings(tab)
    view.webViewClient = object : WebViewClient() {
      override fun onPageStarted(view: WebView, url: String) {
        tab.loading = true
        tab.url = url
      }

      override fun onPageFinished(view: WebView, url: String) {
        tab.loading = false
        tab.progress = 100
        if (!tab.private && url.startsWith("http")) SummitCore.visit(url, tab.title)
      }

      override fun doUpdateVisitedHistory(view: WebView, url: String, isReload: Boolean) {
        tab.url = url
        tab.canGoBack = view.canGoBack()
        tab.canGoForward = view.canGoForward()
      }
    }
    view.webChromeClient = object : WebChromeClient() {
      override fun onProgressChanged(view: WebView, newProgress: Int) {
        tab.progress = newProgress
        tab.loading = newProgress in 0..99
      }

      override fun onReceivedTitle(view: WebView, title: String) {
        tab.title = title
        tab.canGoBack = view.canGoBack()
        tab.canGoForward = view.canGoForward()
      }

      override fun onCloseWindow(window: WebView) {
        close(tab)
      }
    }
    if (tab.url.isNotEmpty() && tab.url != "about:blank") view.loadUrl(tab.url)
  }

  fun applySettings(tab: Tab) {
    val view = tab.webView ?: return
    val preset = SummitCore.setting("userAgent")
    val agent = when {
      tab.desktopSite -> desktopUserAgent()
      preset.isNotEmpty() && preset != "default" -> presetAgent(preset)
      else -> null
    } ?: SummitCore.mobileUserAgent()
    view.settings.userAgentString = agent
    view.settings.mediaPlaybackRequiresUserGesture = SummitCore.setting("autoplay") != "allow"
  }

  private fun presetAgent(id: String): String? {
    val list = JSONArray(SummitCore.userAgentPresets())
    for (i in 0 until list.length()) {
      val p = list.getJSONObject(i)
      if (p.getString("id") == id) return p.getString("agent").ifEmpty { null }
    }
    return null
  }

  private fun desktopUserAgent(): String = presetAgent("chrome-macos")
    ?: "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15"

  private fun contextFor(private: Boolean): WebContext {
    if (private) {
      return privateContext ?: WebContext(appContext, false, true).also { privateContext = it }
    }
    return normalContext ?: WebContext(appContext).also { normalContext = it }
  }

  private fun endPrivateBrowsing() {
    privateContext?.destroy()
    privateContext = null
    if (showingPrivate && selected?.private != true) showingPrivate = false
  }

  /** Discards the least recently shown pages beyond the budget. */
  private fun enforceBudget(keep: Tab) {
    val live = tabs.filter { it.live && it !== keep }.sortedBy { it.lastShown }
    var excess = live.size + 1 - maxLivePages
    for (tab in live) {
      if (excess <= 0) break
      discard(tab)
      excess--
    }
  }

  private fun discardBackgroundPages(keepSelected: Boolean) {
    for (tab in tabs) if (!(keepSelected && tab === selected)) discard(tab)
  }

  /** Destroys the tab's page, keeping what the tab list needs. */
  fun discard(tab: Tab) {
    val view = tab.webView ?: return
    tab.webView = null
    tab.loading = false
    (view.parent as? android.view.ViewGroup)?.removeView(view)
    view.destroy()
    Log.i(TAG, "discarded tab ${tab.id} (${tab.url})")
  }

  /** Keeps a small picture of the page for the tab switcher. */
  fun capturePicture(tab: Tab) {
    val view = tab.webView ?: return
    view.capturePicture(PICTURE_WIDTH) { bitmap: Bitmap? ->
      if (bitmap != null) tab.picture = bitmap.asImageBitmap()
    }
  }
}
