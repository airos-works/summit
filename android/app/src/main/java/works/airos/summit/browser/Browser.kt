package works.airos.summit.browser

import android.app.Activity
import android.app.ActivityManager
import android.content.ComponentCallbacks2
import android.content.Context
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
import org.wpewebkit.wpeview.WPEDownload
import org.wpewebkit.wpeview.WPENotification
import org.wpewebkit.wpeview.WPEPermissionRequest
import org.wpewebkit.wpeview.WPEResourceError
import org.wpewebkit.wpeview.WPEResourceRequest
import org.wpewebkit.wpeview.WebChromeClient
import org.wpewebkit.wpeview.WebContext
import org.wpewebkit.wpeview.WebPage
import org.wpewebkit.wpeview.WebPageChromeClient
import org.wpewebkit.wpeview.WebPageClient
import org.wpewebkit.wpeview.WebView
import works.airos.summit.core.SummitCore

/**
 * The browser: tabs, the engine contexts their pages run in, and how much of
 * the device they may use.
 *
 * Each tab owns a [WebPage]; one [WebView] (the activity's) shows the selected
 * tab's page. Pages need no activity, so they survive the activity being
 * rebuilt.
 *
 * Resource policy (the "lean" part):
 * - At most [maxLivePages] pages exist at once, chosen from the device's RAM;
 *   showing another discards the least recently shown page. A discarded tab
 *   keeps its back/forward list ([WebPage.saveState]), title and picture, and
 *   loads again when shown.
 * - Only the shown page is in a window; the engine treats the others as
 *   hidden (no painting, throttled timers and animation frames).
 * - Android's memory pressure discards every page but the shown one, and
 *   after [BACKGROUND_DISCARD_MS] in the background, that one too, so an idle
 *   Summit holds no web processes. Pages playing sound are kept.
 * - Private tabs live in an ephemeral engine context, destroyed with the last
 *   private tab, taking their cookies, storage and cache with it.
 */
class Browser private constructor(private val appContext: Context) {
  companion object {
    private const val EXTENSION_SCHEME = "webkit-extension:"
    private const val TAG = "Summit"
    const val BACKGROUND_DISCARD_MS = 5 * 60 * 1000L
    private const val PICTURE_WIDTH = 480
    private const val PICTURE_HEIGHT = 960

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
  /** Whether the shown page has an element in fullscreen. */
  var fullscreen by mutableStateOf(false)
    private set

  val downloads = Downloads(appContext)
  /** Summit's WebExtensions (the same engine code as air/OS's). */
  val extensions = works.airos.summit.extensions.Extensions(appContext, this)
  private val notifications = PageNotifications(appContext)

  /** The view of the activity's window, which shows the selected tab's page. */
  var view: WebView? = null
    private set
  private var host: Activity? = null
  private var normalContext: WebContext? = null
  private var privateContext: WebContext? = null
  private var nextId = 1L
  private val handler = Handler(Looper.getMainLooper())
  private val backgroundDiscard = Runnable { discardPages(keepSelected = false) }

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

  /** The activity's window: makes the view that shows pages. */
  fun attach(activity: Activity): WebView {
    host = activity
    handler.removeCallbacks(backgroundDiscard)
    val v = WebView.createEmpty(activity)
    v.webChromeClient = object : WebChromeClient() {
      override fun onEnterFullscreen(view: WebView) {
        fullscreen = true
      }

      override fun onExitFullscreen(view: WebView) {
        fullscreen = false
      }
    }
    view = v
    selected?.let { show(it) }
    return v
  }

  fun detach(activity: Activity) {
    if (host !== activity) return
    view?.setPage(null)
    view?.destroy()
    view = null
    host = null
    fullscreen = false
  }

  fun exitFullscreen() {
    view?.exitFullscreen()
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
    if (!SummitCore.flag("unloadWhenMemoryIsLow") && level < ComponentCallbacks2.TRIM_MEMORY_COMPLETE) return
    when {
      level >= ComponentCallbacks2.TRIM_MEMORY_COMPLETE -> discardPages(keepSelected = host != null)
      level >= ComponentCallbacks2.TRIM_MEMORY_BACKGROUND -> discardPages(keepSelected = true)
      level >= ComponentCallbacks2.TRIM_MEMORY_RUNNING_LOW -> discardPages(keepSelected = true)
    }
  }

  // ---------------------------------------------------------------- tabs

  /** Reopens the saved tabs; [openHome] adds the home page when there are none. */
  fun restoreSession(openHome: Boolean = true) {
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
    if (tabs.isEmpty() && openHome) newTab(homeUrl())
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
    tab.focusAddress = select && (url.isEmpty() || url == "about:blank")
    val index = after?.let { tabs.indexOf(it) + 1 } ?: tabs.size
    tabs.add(index.coerceIn(0, tabs.size), tab)
    if (select) select(tab)
    return tab
  }

  fun select(tab: Tab) {
    val previous = selected
    if (previous !== tab) {
      previous?.let { capturePicture(it) }
      selected = tab
      showingPrivate = tab.private
    }
    show(tab)
  }

  fun close(tab: Tab) {
    val index = tabs.indexOf(tab)
    if (index < 0) return
    if (view?.page === tab.page && tab.page != null) view?.setPage(null)
    destroyPage(tab, keepState = false)
    tabs.removeAt(index)
    notifications.closeFor(tab.id)
    if (selected === tab) {
      selected = null
      // The neighbour of the same kind, as Chrome does.
      val sameKind = tabs.filter { it.private == tab.private }
      val next = sameKind.firstOrNull { tabs.indexOf(it) >= index } ?: sameKind.lastOrNull() ?: tabs.lastOrNull()
      if (next != null) select(next) else newTab(homeUrl())
    }
    if (tabs.none { it.private }) endPrivateBrowsing()
    extensions.syncTabs()
  }

  fun closeAll(private: Boolean) {
    for (tab in tabs.filter { it.private == private }.toList()) close(tab)
  }

  /** Back when the page cannot go back: a tab a page opened closes and shows its opener. */
  fun closeToOpener(tab: Tab): Boolean {
    val opener = tabs.firstOrNull { it.id == tab.openerId } ?: return false
    close(tab)
    select(opener)
    return true
  }

  // ---------------------------------------------------------------- navigation

  /** Goes where the address field text says: an address, a bookmark keyword or a search. */
  fun go(text: String, tab: Tab? = selected) {
    val target = tab ?: newTab()
    val url = SummitCore.resolve(text.trim()).url.ifEmpty { return }
    target.focusAddress = false
    target.url = url
    target.state = null
    var page = target.page
    // Pages made for an extension show only its pages, and only they can: going
    // between the two takes a new page (as air/OS's separate extension views).
    if (page != null && (target.extensionPage || url.startsWith(EXTENSION_SCHEME))) {
      if (view?.page === page) view?.setPage(null)
      destroyPage(target, keepState = false)
      page = null
    }
    if (page != null) page.loadUrl(url) else if (target === selected) show(target)
  }

  /** Gives the page view the keyboard focus (from the address field); false when it could not take it. */
  fun focusPage(): Boolean = view?.requestFocus() == true

  /** A page for an extension's action popup, shown in a sheet rather than as a tab. */
  fun createPopupPage(url: String): WebPage {
    extensions.preparePage(url, popup = true)
    return WebPage(contextFor(false))
  }

  /** A new tab showing url (an extension's tabs.create); its page is made even when it is not selected. */
  fun openTab(url: String, select: Boolean): Tab {
    val tab = newTab(url, select = select)
    if (!select && tab.page == null && url.isNotEmpty()) createPage(tab)
    return tab
  }

  // ---------------------------------------------------------------- pages

  /** Makes the tab's page live (again, if it was discarded), shows it and keeps within the budget. */
  private fun show(tab: Tab) {
    tab.lastShown = SystemClock.uptimeMillis()
    val v = view ?: return
    val page = tab.page ?: createPage(tab)
    if (v.page !== page) v.setPage(page)
    handler.post { extensions.syncTabs() }
    // Later, not now: this may run inside an engine callback (a page opening
    // a window) whose own page must not be destroyed under it.
    handler.post { if (selected === tab) enforceBudget(keep = tab) }
  }

  private fun createPage(tab: Tab): WebPage {
    // An extension's own page (its popup or options in a tab) gets that
    // extension's configuration; private tabs' pages never do.
    tab.extensionPage = extensions.preparePage(if (tab.private) "" else tab.url)
    val page = WebPage(contextFor(tab.private))
    configure(tab, page)
    val state = tab.state
    tab.state = null
    if (state != null && page.restoreState(state)) {
      Log.i(TAG, "restored tab ${tab.id} (${tab.url})")
    } else if (tab.url.isNotEmpty() && tab.url != "about:blank") {
      page.loadUrl(tab.url)
    }
    return page
  }

  private fun configure(tab: Tab, page: WebPage) {
    tab.page = page
    applySettings(tab)
    page.setClient(object : WebPageClient() {
      override fun onPageStarted(page: WebPage, url: String) {
        tab.loading = true
        tab.crashed = false
        tab.url = url
      }

      override fun onPageFinished(page: WebPage, url: String) {
        tab.loading = false
        tab.progress = 100
        if (!tab.private && url.startsWith("http")) {
          SummitCore.visit(url, tab.title)
          extensions.historyVisited(url)
        }
      }

      override fun doUpdateVisitedHistory(page: WebPage, url: String, isReload: Boolean) {
        tab.url = url
        tab.canGoBack = page.canGoBack()
        tab.canGoForward = page.canGoForward()
      }

      override fun onReceivedError(page: WebPage, request: WPEResourceRequest, error: WPEResourceError) {
        if (request.isForMainFrame) showLoadError(tab, page, request.url.toString(), error)
      }

      override fun onDownloadStart(page: WebPage, download: WPEDownload) {
        downloads.start(download)
      }

      override fun onWebProcessTerminated(page: WebPage, reason: Int) {
        Log.w(TAG, "web process of tab ${tab.id} ended ($reason)")
        tab.loading = false
        // A hidden tab whose process Android ended to free memory just loads
        // again when shown; the shown one says what happened.
        if (tab === selected) tab.crashed = true else destroyPage(tab, keepState = true)
      }
    })
    page.setChromeClient(object : WebPageChromeClient() {
      override fun onProgressChanged(page: WebPage, newProgress: Int) {
        tab.progress = newProgress
        tab.loading = newProgress in 0..99
      }

      override fun onReceivedTitle(page: WebPage, title: String) {
        tab.title = title
        tab.canGoBack = page.canGoBack()
        tab.canGoForward = page.canGoForward()
      }

      override fun onFaviconChanged(page: WebPage) {
        tab.icon = page.getFavicon(64)?.asImageBitmap()
      }

      override fun onPlayingAudioChanged(page: WebPage, isPlayingAudio: Boolean) {
        tab.playingAudio = isPlayingAudio
      }

      override fun onCloseWindow(page: WebPage) {
        close(tab)
      }

      override fun onCreateWindow(opener: WebPage, request: WindowRequest) {
        val created = Tab(nextId++, tab.private, "")
        created.openerId = tab.id
        tabs.add(tabs.indexOf(tab) + 1, created)
        configure(created, request.createPage())
        select(created)
      }

      override fun onPermissionRequest(page: WebPage, request: WPEPermissionRequest) {
        handlePermission(tab, request)
      }

      override fun onPermissionRequestCanceled(page: WebPage, request: WPEPermissionRequest) {
        if (permissionPrompt?.request === request) permissionPrompt = null
      }

      override fun onShowFileChooser(page: WebPage, fileChooserParams: FileChooserParams,
        filePathCallback: org.wpewebkit.wpeview.WPECallback<Array<String>>): Boolean {
        val picker = filePicker ?: return false
        val multiple = fileChooserParams.mode == FileChooserParams.MODE_OPEN_MULTIPLE
        picker.pick(fileChooserParams.acceptTypes.toList(), multiple) { uris ->
          filePathCallback.onResult(if (uris.isNullOrEmpty()) null else Uploads.copy(appContext, uris).toTypedArray())
        }
        return true
      }

      override fun onShowNotification(page: WebPage, notification: WPENotification) {
        notifications.show(tab, notification)
      }

      override fun onCloseNotification(page: WebPage, notification: WPENotification) {
        notifications.close(notification)
      }
    })
  }

  /** The error page, with the words air/OS's Summit uses (core/LoadError). */
  private fun showLoadError(tab: Tab, page: WebPage, url: String, error: WPEResourceError) {
    val kind = when (error.errorCode) {
      WPEResourceError.ERROR_HOST_LOOKUP -> if (Network.online(appContext)) "host" else "offline"
      WPEResourceError.ERROR_CONNECT -> if (Network.online(appContext)) "refused" else "offline"
      WPEResourceError.ERROR_TIMEOUT -> "timeout"
      WPEResourceError.ERROR_FAILED_SSL_HANDSHAKE -> "secure"
      WPEResourceError.ERROR_IO -> "reset"
      else -> "other"
    }
    val text = JSONObject(SummitCore.describeLoadFailure(kind, url, error.description))
    page.loadAlternateHtml(ErrorPage.html(text.optString("title"), text.optString("explanation")), url, url)
    tab.loading = false
  }

  fun applySettings(tab: Tab) {
    val page = tab.page ?: return
    val preset = SummitCore.setting("userAgent")
    val agent = when {
      tab.desktopSite -> desktopUserAgent()
      preset.isNotEmpty() && preset != "default" -> presetAgent(preset)
      else -> null
    } ?: SummitCore.mobileUserAgent()
    page.settings.userAgentString = agent
    page.settings.mediaPlaybackRequiresUserGesture = SummitCore.setting("autoplay") != "allow"
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
    if (private) return privateContext ?: WebContext(appContext, false, true).also { privateContext = it }
    // Normal tabs keep their cookies and storage (wpeview would make an
    // emulator's session ephemeral by default).
    return normalContext ?: WebContext(appContext, false, false).also {
      normalContext = it
      it.isTrackingPreventionEnabled = SummitCore.flag("preventCrossSiteTracking")
      // Before any page is made with the session: pages get the extensions when made.
      extensions.start(it.networkSessionHandle)
    }
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

  private fun discardPages(keepSelected: Boolean) {
    for (tab in tabs.toList()) if (!(keepSelected && tab === selected)) discard(tab)
  }

  /** Destroys the tab's page, keeping its history, title and picture. A page playing sound is kept. */
  fun discard(tab: Tab) {
    if (tab.playingAudio && tab.page != null) return
    if (view?.page === tab.page && tab.page != null) view?.setPage(null)
    destroyPage(tab, keepState = true)
  }

  private fun destroyPage(tab: Tab, keepState: Boolean) {
    val page = tab.page ?: return
    if (keepState) tab.state = runCatching { page.saveState() }.getOrNull()
    tab.page = null
    tab.loading = false
    page.destroy()
    Log.i(TAG, "discarded tab ${tab.id} (${tab.url})")
  }

  /** Keeps a small picture of the page for the tab switcher. */
  fun capturePicture(tab: Tab) {
    val page = tab.page ?: return
    if (page.view == null) return
    page.captureSnapshot(WebPage.SNAPSHOT_REGION_VISIBLE, PICTURE_WIDTH, PICTURE_HEIGHT) { bitmap ->
      if (bitmap != null) tab.picture = bitmap.asImageBitmap()
    }
  }

  // ---------------------------------------------------------------- find in page

  /** The find bar: null when closed, else the matches of [findText] ("" until typed). */
  var findMatches by mutableStateOf<Int?>(null)
    private set
  var findMore by mutableStateOf(false)
    private set

  fun startFind() {
    findMatches = 0
    findMore = false
  }

  fun find(text: String) {
    val page = selected?.page ?: return
    if (text.isEmpty()) {
      page.clearMatches()
      findMatches = 0
      return
    }
    page.findAllAsync(text, 0) { _, count, more ->
      findMatches = count
      findMore = more
    }
  }

  fun findNext(forward: Boolean) {
    selected?.page?.findNext(forward)
  }

  fun endFind() {
    selected?.page?.clearMatches()
    findMatches = null
  }

  // ---------------------------------------------------------------- files

  /** Lets the person pick documents for a file field (the activity's document picker). */
  fun interface FilePicker {
    fun pick(mimeTypes: List<String>, multiple: Boolean, done: (List<android.net.Uri>?) -> Unit)
  }
  var filePicker: FilePicker? = null

  // ---------------------------------------------------------------- permissions

  /** A site's request waiting for an answer, shown by the interface. */
  class PermissionPrompt(val tab: Tab, val request: WPEPermissionRequest, val origin: String, val permissions: List<String>) {
    val site: String get() = SummitCore.shortAddress(origin)
  }

  var permissionPrompt by mutableStateOf<PermissionPrompt?>(null)
    private set

  /** Grants Android runtime permissions (location, notifications) for page permissions. */
  fun interface SystemPermissions {
    fun request(permissions: List<String>, done: (Boolean) -> Unit)
  }
  var systemPermissions: SystemPermissions? = null

  private fun handlePermission(tab: Tab, request: WPEPermissionRequest) {
    val origin = request.origin.toString().trimEnd('/')
    val permissions = request.resources.map {
      when (it) {
        WPEPermissionRequest.RESOURCE_LOCATION -> "geolocation"
        WPEPermissionRequest.RESOURCE_NOTIFICATIONS -> "notifications"
        else -> it
      }
    }
    val saved = permissions.map { SummitCore.sitePermission(it, origin) }
    when {
      !tab.private && saved.isNotEmpty() && saved.all { it == "allow" } ->
        grantSystem(permissions) { granted -> if (granted) request.grant() else request.deny() }
      saved.any { it == "deny" } -> request.deny()
      else -> permissionPrompt = PermissionPrompt(tab, request, origin, permissions)
    }
  }

  /** The interface answered [permissionPrompt]. */
  fun answerPermission(allow: Boolean, remember: Boolean) {
    val prompt = permissionPrompt ?: return
    permissionPrompt = null
    fun save(answer: Boolean) {
      if (remember && !prompt.tab.private) {
        for (p in prompt.permissions) SummitCore.setSitePermission(p, prompt.origin, if (answer) "allow" else "deny")
      }
    }
    if (!allow) {
      save(false)
      prompt.request.deny()
      return
    }
    grantSystem(prompt.permissions) { granted ->
      if (granted) save(true)
      if (granted) prompt.request.grant() else prompt.request.deny()
    }
  }

  private fun grantSystem(permissions: List<String>, done: (Boolean) -> Unit) {
    val system = permissions.mapNotNull {
      when (it) {
        "geolocation" -> android.Manifest.permission.ACCESS_FINE_LOCATION
        "notifications" -> android.Manifest.permission.POST_NOTIFICATIONS
        "camera" -> android.Manifest.permission.CAMERA
        "microphone" -> android.Manifest.permission.RECORD_AUDIO
        else -> null
      }
    }
    val host = systemPermissions
    if (system.isEmpty() || host == null) done(system.isEmpty()) else host.request(system, done)
  }
}
