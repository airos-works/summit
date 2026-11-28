package works.airos.summit.extensions

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.util.Log
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import org.json.JSONArray
import org.json.JSONObject
import org.wpewebkit.wpeview.WebPage
import org.wpewebkit.wpeview.WebPageChromeClient
import works.airos.summit.browser.Browser
import works.airos.summit.browser.Tab
import works.airos.summit.core.SummitCore
import java.io.File

/** The engine's WebExtensions API and the shared catalog (SummitExtensionsJni.cpp). */
object ExtensionsNative {
  init {
    System.loadLibrary("summit")
  }

  @JvmStatic external fun initialize(session: Long, storage: String, catalogDirectory: String): Boolean
  @JvmStatic external fun request(json: String): String
  @JvmStatic external fun pageId(webView: Long): Long
  @JvmStatic external fun catalog(): String
  @JvmStatic external fun stage(source: String): String
  @JvmStatic external fun installStaged(entry: String): String
  @JvmStatic external fun discardStaged()
  @JvmStatic external fun forget(identifier: String): String
  @JvmStatic external fun identity(manifest: String, localIdentity: String, verifiedCRX: String): String
  @JvmStatic external fun storeItem(url: String): String
  @JvmStatic external fun storeItemName(title: String): String
  @JvmStatic external fun storeOfferScript(offer: String): String
  @JvmStatic external fun parseStoreOfferLink(url: String): String
  @JvmStatic external fun resolveFirefoxItem(url: String): String
  @JvmStatic external fun downloadStoreItem(url: String, temporaryDirectory: String): String
  @JvmStatic external fun cancelStoreDownload()

  /** Engine events (JSON), on the main thread. */
  @JvmStatic fun onEvent(json: String) {
    // An exception would otherwise end silently in the engine's JNI call.
    try {
      Extensions.instance?.onEvent(JSONObject(json))
    } catch (e: Exception) {
      Log.e("Summit", "engine event ${json.take(200)}", e)
    }
  }
}

/**
 * Summit's WebExtensions on Android: the same engine implementation and the
 * same messages as air/OS's Summit (UIProcess/API/haiku/WebKitContext.h
 * documents them), with the app as the browser that answers them.
 */
class Extensions(private val context: Context, private val browser: Browser) {
  companion object {
    private const val TAG = "Summit"
    private const val BROWSER_TARGET = "1"
    private const val ACTION_TARGET = "2"
    // ExtensionActionPopupWPE.h: summitPopupShowWPE ('wxps'), summitPopupCloseWPE ('wxpc').
    private val POPUP_SHOW = fourCharCode("wxps")
    private val POPUP_CLOSE = fourCharCode("wxpc")
    private fun fourCharCode(code: String): Long = code.fold(0L) { value, c -> (value shl 8) or c.code.toLong() }
    private const val REPLY_TARGET = "3"
    // The app has one browser window.
    const val WINDOW = 1L

    var instance: Extensions? = null
      private set
  }

  /** An installed extension, as the catalog records it, and whether it runs. */
  class Installed(val identifier: String, val name: String, val version: String, val path: String, val entry: JSONObject) {
    var loaded by mutableStateOf(false)
    var error by mutableStateOf<String?>(null)
  }

  /** An extension's toolbar action (browser_action, action, page_action). */
  class Action(val extension: String, val name: String, val loadIdentifier: Long, val hasPopup: Boolean, val badge: String)

  /** A package waiting for the person to approve its permissions. */
  class Pending(val token: String, val name: String, val version: String, val permissions: List<String>, val origins: List<String>,
    val details: JSONObject, val pkg: String, val order: Long)

  val installed = mutableStateListOf<Installed>()
  val actions = mutableStateListOf<Action>()
  var pending by mutableStateOf<Pending?>(null)
    private set
  var status by mutableStateOf<String?>(null)

  private var ready = false
  private var nextRequest = 1L
  private val replies = HashMap<Long, (JSONObject) -> Unit>()
  private val handler = Handler(Looper.getMainLooper())

  /** Starts extensions for the session; call before the first page is made with it. */
  fun start(sessionHandle: Long) {
    if (ready) return
    instance = this
    val storage = File(context.filesDir, "extensions/storage").apply { mkdirs() }
    val catalog = File(context.filesDir, "extensions/catalog").apply { mkdirs() }
    ready = ExtensionsNative.initialize(sessionHandle, storage.absolutePath, catalog.absolutePath)
    if (!ready) return
    reloadCatalog()
    // The bookmarks as they are now; their later changes are reported.
    SummitCore.bookmarkChanges()
    for (extension in installed.toList()) loadInstalled(extension)
  }

  private fun reloadCatalog() {
    val list = JSONArray(ExtensionsNative.catalog())
    val previous = installed.associateBy { it.identifier }
    installed.clear()
    for (i in 0 until list.length()) {
      val e = list.getJSONObject(i)
      val id = e.optString("identifier")
      installed += previous[id] ?: Installed(id, e.optString("name"), e.optString("version"), e.optString("path"), e)
    }
  }

  private fun call(request: JSONObject, reply: ((JSONObject) -> Unit)? = null): JSONObject {
    if (reply != null) {
      val id = nextRequest++
      replies[id] = reply
      request.put("id", id.toString())
    }
    return JSONObject(ExtensionsNative.request(request.toString()))
  }

  // ---------------------------------------------------------------- installing

  /** Installs a package (.xpi, .crx or .zip): prepared first, then the person approves its permissions. */
  fun install(source: File) {
    if (!ready) {
      status = "Extensions are not available in this build"
      return
    }
    val staged = JSONObject(ExtensionsNative.stage(source.absolutePath))
    if (staged.optString("staged").isEmpty()) {
      status = "Could not read the package: ${staged.optString("error")}"
      return
    }
    status = "Checking ${source.name}…"
    prepareStaged(staged)
  }

  /**
   * A staged package (stage() or downloadStoreItem()) checked by the engine and
   * offered for approval. A store's package must be the item its page shows,
   * and a Chrome Web Store package must carry a valid CRX3 signature, as
   * air/OS's ExtensionInstaller requires.
   */
  private fun prepareStaged(staged: JSONObject) {
    val path = staged.optString("staged")
    val pkg = staged.optString("package")
    val order = staged.optLong("order")
    val expectedIdentity = staged.optString("expectedIdentity")
    val store = staged.optString("store")
    call(JSONObject().put("op", "prepare").put("path", path)) { reply ->
      fun refuse(message: String) {
        call(JSONObject().put("op", "discard").put("token", reply.string("token")))
        ExtensionsNative.discardStaged()
        status = message
      }
      if (reply.error() != 0) {
        ExtensionsNative.discardStaged()
        status = "Not a valid extension: ${reply.string("description").ifEmpty { "error ${reply.error()}" }}"
        return@call
      }
      val identity = ExtensionsNative.identity(reply.string("manifest_json"), "summit-$pkg", reply.string("verified_crx_id"))
      if (expectedIdentity.isNotEmpty() && identity != expectedIdentity) {
        refuse("The downloaded extension's identity does not match the selected store item.")
        return@call
      }
      if (store == "Chrome Web Store" && reply.string("verified_crx_id").isEmpty()) {
        refuse("The Chrome Web Store package does not have a valid CRX3 signature.")
        return@call
      }
      status = null
      pending = Pending(reply.string("token"), reply.string("name"), reply.string("version"),
        // File access is the person's choice per extension (as on air/OS), not part of adding it.
        reply.strings("permission"), reply.strings("origin").filterNot { it.startsWith("file:") }, reply, pkg, order)
    }
  }

  fun approve(allow: Boolean) {
    val p = pending ?: return
    pending = null
    if (!allow) {
      call(JSONObject().put("op", "discard").put("token", p.token))
      ExtensionsNative.discardStaged()
      return
    }
    // The identity and installation as air/OS's installer gives them.
    val identifier = ExtensionsNative.identity(p.details.string("manifest_json"), "summit-${p.pkg}", p.details.string("verified_crx_id"))
    if (installed.any { it.identifier == identifier }) {
      call(JSONObject().put("op", "discard").put("token", p.token))
      ExtensionsNative.discardStaged()
      status = "${p.name} is already added"
      return
    }
    val load = JSONObject().put("op", "load").put("token", p.token).put("purpose", "approved")
      .put("uniqueIdentifier", identifier).put("expectedFingerprint", p.details.string("fingerprint"))
      .put("permissions", JSONArray(p.permissions)).put("origins", JSONArray(p.origins))
      .put("installation", p.pkg).put("order", p.order.toString())
    call(load) { reply ->
      if (reply.error() != 0) {
        ExtensionsNative.discardStaged()
        status = "${p.name} could not be loaded: ${reply.string("description").ifEmpty { "error ${reply.error()}" }}"
        return@call
      }
      val entry = JSONObject().put("identifier", identifier).put("name", p.name).put("version", p.version)
        .put("package", p.pkg).put("installationOrder", p.order)
        .put("fingerprint", p.details.string("fingerprint")).put("permissions", JSONArray(p.permissions))
        .put("origins", JSONArray(p.origins))
      val error = ExtensionsNative.installStaged(entry.toString())
      if (error.isNotEmpty()) Log.w(TAG, "recording ${p.name}: $error")
      reloadCatalog()
      installed.firstOrNull { it.identifier == identifier }?.loaded = true
      // The stores' cards now offer to manage it.
      browser.tabs.forEach { updateStoreOffer(it) }
      status = "${p.name} was added"
      refreshActions()
    }
  }

  // An installed extension, as air/OS's ExtensionController loads one: its saved
  // state brings back the origins granted; the permissions are the package's
  // current ones, covered by the consent recorded for this fingerprint.
  private fun loadInstalled(extension: Installed) {
    call(JSONObject().put("op", "prepare").put("path", extension.path)) { prepared ->
      val e = extension.entry
      if (prepared.error() != 0) {
        extension.error = "The package could not be read"
        return@call
      }
      if (prepared.string("fingerprint") != e.optString("fingerprint")) {
        call(JSONObject().put("op", "discard").put("token", prepared.string("token")))
        extension.error = "The installed package has changed and requires new approval."
        return@call
      }
      val load = JSONObject().put("op", "load").put("token", prepared.string("token"))
        .put("purpose", "startup")
        .put("uniqueIdentifier", extension.identifier)
        .put("expectedFingerprint", e.optString("fingerprint"))
        .put("permissions", JSONArray(prepared.strings("permission")))
        .put("installation", e.optString("package")).put("order", e.optLong("installationOrder").toString())
        .put("allowFileURLs", e.optBoolean("allowFileURLs"))
        .put("allowPrivateBrowsing", e.optBoolean("allowPrivateBrowsing"))
      call(load) { reply ->
        extension.loaded = reply.error() == 0
        extension.error = if (extension.loaded) null
          else "Could not be loaded: ${reply.string("description").ifEmpty { "error ${reply.error()}" }}"
        refreshActions()
      }
    }
  }

  fun remove(extension: Installed) {
    call(JSONObject().put("op", "unload").put("extension", extension.identifier)) {
      ExtensionsNative.forget(extension.identifier)
      reloadCatalog()
      refreshActions()
    }
  }

  // ---------------------------------------------------------------- tabs and actions

  /** Tells the engine which tabs the window has (BWebKitContext::SetBrowserWindowTabs). */
  fun syncTabs() {
    if (!ready) return
    val pages = JSONArray()
    var active = 0L
    for (tab in browser.tabs.filter { !it.private }) {
      // Tabs without a live page are tabs too (Tab.placeholder).
      browser.ensurePlaceholder(tab)
      val id = pageIdOf(tab)
      if (id == 0L) continue
      // As air/OS's SetBrowserTabDiscarded: a placeholder shows the tab's address and title.
      val discarded = if (tab.page == null) "$id\n${tab.url}\n${tab.title}" else ""
      if (discarded != tab.reportedDiscarded) {
        val reply = call(JSONObject().put("op", "setTabDiscarded").put("page", id.toString()).put("discarded", tab.page == null)
          .put("url", tab.url).put("title", tab.title))
        if (reply.has("error")) Log.w(TAG, "setTabDiscarded: ${reply.optString("error")}")
        tab.reportedDiscarded = discarded
      }
      pages.put(id.toString())
      if (tab === browser.selected) active = id
    }
    call(JSONObject().put("op", "setWindowTabs").put("window", WINDOW.toString()).put("pages", pages)
      .put("active", active.toString()).put("focused", true))
  }

  // ---------------------------------------------------------------- the stores

  /** The secret the store card's links carry, so that only they count (core/StoreOffer). */
  private val storeOfferToken = java.util.UUID.randomUUID().toString().replace("-", "") // hex, as ParseStoreOfferLink wants
  /** Firefox Add-ons slugs and their add-on IDs (null while being looked up). */
  private val firefoxItems = HashMap<String, JSONObject?>()
  private val storeWorker = java.util.concurrent.Executors.newSingleThreadExecutor()

  /**
   * After a page loaded: on an extension's page in the Chrome Web Store or on
   * Firefox Add-ons, the card air/OS's Summit shows (Install, or Manage once
   * installed); elsewhere none.
   */
  fun updateStoreOffer(tab: Tab) {
    val page = tab.page ?: return
    if (!ready || tab.private) return
    val item = ExtensionsNative.storeItem(tab.url).takeIf { it.isNotEmpty() }?.let { JSONObject(it) }
    if (item == null || tab.storeOfferDismissed == tab.url) {
      if (tab.storeOfferShown) page.evaluateJavascript(ExtensionsNative.storeOfferScript(""), null)
      tab.storeOfferShown = false
      return
    }
    var name = ExtensionsNative.storeItemName(tab.title)
    var identity = item.optString("identifier")
    if (item.optString("store") == "firefox") {
      val slug = identity
      if (!firefoxItems.containsKey(slug)) {
        firefoxItems[slug] = null
        val url = tab.url
        storeWorker.execute {
          val resolved = JSONObject(ExtensionsNative.resolveFirefoxItem(url))
          if (resolved.has("error")) Log.w(TAG, "Firefox Add-ons item $slug: ${resolved.optString("error")}")
          handler.post {
            firefoxItems[slug] = resolved
            browser.tabs.filter { it.url == url }.forEach { updateStoreOffer(it) }
          }
        }
      }
      val resolved = firefoxItems[slug] ?: return
      if (resolved.has("error") || !resolved.optBoolean("extension", true)) return
      identity = resolved.optString("identifier")
      name = resolved.optString("name").ifEmpty { name }
    }
    val installedItem = installed.firstOrNull { it.identifier == identity }
    val offer = JSONObject().put("name", name.ifEmpty { installedItem?.name.orEmpty() })
      .put("installed", installedItem != null).put("disabled", false).put("token", storeOfferToken)
    page.evaluateJavascript(ExtensionsNative.storeOfferScript(offer.toString()), null)
    tab.storeOfferShown = true
  }

  /** A link of the card (summit-extension:ACTION?token=…); true when it was one, handled. */
  fun storeOfferChosen(tab: Tab, url: String): Boolean {
    if (!url.startsWith("summit-extension:")) return false
    val choice = ExtensionsNative.parseStoreOfferLink(url).takeIf { it.isNotEmpty() }?.let { JSONObject(it) } ?: return true
    if (choice.optString("token") != storeOfferToken || !tab.storeOfferShown) {
      Log.w(TAG, "store card link ignored (token ${choice.optString("token") == storeOfferToken}, shown ${tab.storeOfferShown})")
      return true
    }
    when (choice.optString("action")) {
      "dismiss" -> {
        tab.storeOfferDismissed = tab.url
        updateStoreOffer(tab)
      }
      "install" -> installFromStore(tab.url)
      else -> showExtensions?.invoke()
    }
    return true
  }

  /** Opens the Extensions screen (the app sets it). */
  var showExtensions: (() -> Unit)? = null

  /** Downloads the extension a store page shows and offers it for approval, as a package from a file. */
  fun installFromStore(pageUrl: String) {
    if (!ready) return
    status = "Downloading the extension…"
    val temporary = File(context.cacheDir, "extension-downloads").absolutePath
    storeWorker.execute {
      val staged = JSONObject(ExtensionsNative.downloadStoreItem(pageUrl, temporary))
      Log.i(TAG, "store download of $pageUrl: ${staged.toString().take(300)}")
      handler.post {
        if (staged.optString("staged").isEmpty()) {
          status = staged.optString("error").ifEmpty { "The extension could not be downloaded." }
          android.widget.Toast.makeText(context, status, android.widget.Toast.LENGTH_LONG).show()
          return@post
        }
        status = "Checking the extension…"
        prepareStaged(staged)
      }
    }
  }

  // ---------------------------------------------------------------- history and bookmarks

  /** A page was visited (history.onVisited), as air/OS's RecordVisit reports it. */
  fun historyVisited(url: String) {
    if (!ready) return
    val item = SummitCore.historyItem(url)
    if (item.isNotEmpty()) call(JSONObject().put("op", "historyChanged").put("event", "visited").put("json", item))
  }

  /** The browser changed the bookmarks: their events to extensions. */
  fun bookmarksChanged() {
    if (ready) forwardBookmarkEvents(JSONArray(SummitCore.bookmarkChanges()))
  }

  private fun forwardBookmarkEvents(events: JSONArray?) {
    if (events == null) return
    for (i in 0 until events.length()) {
      val event = events.getJSONObject(i)
      call(JSONObject().put("op", "bookmarksChanged").put("event", event.optString("event"))
        .put("id", event.optString("id")).put("json", event.optString("json")))
    }
  }

  /**
   * Names the URL the next page made will show (see SummitExtensionsWPE's
   * "pageFor"); true when it is an extension's page, made for that extension.
   */
  fun preparePage(url: String, popup: Boolean = false): Boolean =
    ready && call(JSONObject().put("op", "pageFor").put("url", url).put("purpose", if (popup) "popup" else "tab"))
      .optString("extension").isNotEmpty()

  // ---------------------------------------------------------------- action popups

  /** An extension's action popup, shown in a sheet over the page (ExtensionActionPopupWPE.h). */
  class Popup(val extension: String, val title: String, val page: WebPage) {
    /** Whether a view shows the page; that view's release then destroys it. */
    var viewAttached = false
  }

  var popup by mutableStateOf<Popup?>(null)
    private set

  private fun showPopup(fields: JSONObject) {
    destroyPopup()
    val extension = fields.string("extension_identifier")
    val url = fields.string("url")
    val page = browser.createPopupPage(url)
    val handle = page.engineHandle.let { ExtensionsNative.pageId(it) }
    val attached = call(JSONObject().put("op", "attachPopup").put("extension", extension).put("page", handle.toString()))
    if (attached.has("error")) {
      Log.w(TAG, "popup of $extension: ${attached.optString("error")}")
      page.destroy()
      return
    }
    page.setChromeClient(object : WebPageChromeClient() {
      // window.close() in the popup, as extensions do after an action.
      override fun onCloseWindow(page: WebPage) = closePopup()
    })
    page.loadUrl(url)
    popup = Popup(extension, fields.string("title"), page)
  }

  /** The person closed the popup's sheet, or the popup closed itself. */
  fun closePopup() {
    val shown = popup ?: return
    call(JSONObject().put("op", "popupClosed").put("extension", shown.extension))
    destroyPopup()
  }

  private fun destroyPopup() {
    val shown = popup ?: return
    popup = null
    if (!shown.viewAttached) shown.page.destroy()
  }

  /** The sheet's view let go of the popup's page. */
  fun popupViewReleased(released: Popup) {
    released.viewAttached = false
    if (popup !== released) released.page.destroy()
  }

  fun pageIdOf(tab: Tab): Long = (tab.page ?: tab.placeholder)?.engineHandle?.let { ExtensionsNative.pageId(it) } ?: 0

  fun refreshActions() {
    if (!ready) return
    call(JSONObject().put("op", "actions").put("window", WINDOW.toString())) { reply ->
      actions.clear()
      val list = reply.objects("action")
      for (a in list) {
        actions += Action(a.string("extension_identifier"), a.string("name").ifEmpty { a.string("extension_name") },
          a.string("load_identifier").toLongOrNull() ?: 0, a.bool("has_popup"), a.string("badge_text"))
      }
    }
  }

  fun activate(action: Action) {
    val page = browser.selected?.let { pageIdOf(it) } ?: 0
    call(JSONObject().put("op", "activateAction").put("extension", action.extension).put("window", WINDOW.toString())
      .put("load", action.loadIdentifier.toString()).put("page", page.toString())) { }
  }

  // ---------------------------------------------------------------- engine events

  fun onEvent(event: JSONObject) {
    if (Log.isLoggable(TAG, Log.DEBUG) || works.airos.summit.BuildConfig.DEBUG) Log.d(TAG, "engine: ${event.toString().take(600)}")
    val fields = event.optJSONObject("fields") ?: JSONObject()
    when (event.optString("target")) {
      REPLY_TARGET -> {
        val id = fields.string("identifier").toLongOrNull() ?: return
        replies.remove(id)?.invoke(fields)
      }
      ACTION_TARGET -> when (event.optLong("what")) {
        POPUP_SHOW -> handler.post { showPopup(fields) }
        POPUP_CLOSE -> handler.post { if (popup?.extension == fields.string("extension_identifier")) destroyPopup() }
        else -> handler.post { refreshActions() }
      }
      BROWSER_TARGET -> when (event.optLong("what")) {
        BrowserCodes.BROWSER_COMMAND.toLong() -> browserCommand(fields)
        // A page made for an extension went to a web page: show it in a normal page.
        BrowserCodes.EXTERNAL_NAVIGATION_REQUESTED.toLong() -> {
          val page = fields.string("page_identifier").toLongOrNull() ?: 0
          val url = fields.string("url")
          browser.tabs.firstOrNull { pageIdOf(it) == page }?.let { tab -> handler.post { browser.go(url, tab) } }
        }
      }
    }
  }

  /** An extension asks the browser to change its tabs (the BROWSER_* commands of WebKitContext.h). */
  private fun browserCommand(fields: JSONObject) {
    val identifier = fields.string("identifier")
    fun respond(status: String, pages: List<Long> = emptyList(), error: String = "", data: String = "") {
      val request = JSONObject().put("op", "respond").put("identifier", identifier).put("status", status)
        .put("error", error).put("pages", JSONArray(pages.map { it.toString() }))
      if (data.isNotEmpty()) request.put("data", data)
      call(request)
    }
    fun tabsNamed(field: String): List<Tab> {
      val ids = fields.optJSONArray("${field}_identifier")?.let { a -> (0 until a.length()).map { a.optString(it).toLongOrNull() ?: 0 } }
        ?: emptyList()
      return browser.tabs.filter { pageIdOf(it) in ids }
    }
    val pageTabs = tabsNamed("page")
    when (fields.int("command")) {
      BrowserCodes.BROWSER_OPEN_TAB -> {
        val url = fields.string("url").let { if (it.isEmpty()) "" else SummitCore.resolve(it).url }
        val tab = browser.openTab(url, select = fields.bool("active", true))
        val created = pageIdOf(tab)
        respond("B_OK", if (created != 0L) listOf(created) else emptyList())
        syncTabs()
      }
      BrowserCodes.BROWSER_CLOSE_TABS -> {
        pageTabs.forEach { browser.close(it) }
        respond("B_OK")
        syncTabs()
      }
      BrowserCodes.BROWSER_ACTIVATE_TAB -> {
        pageTabs.firstOrNull()?.let { browser.select(it) }
        respond(if (pageTabs.isEmpty()) "B_ENTRY_NOT_FOUND" else "B_OK")
        syncTabs()
      }
      BrowserCodes.BROWSER_NAVIGATE_TAB -> {
        pageTabs.firstOrNull()?.let { browser.go(fields.string("url"), it) }
        respond(if (pageTabs.isEmpty()) "B_ENTRY_NOT_FOUND" else "B_OK")
      }
      BrowserCodes.BROWSER_RELOAD_TAB -> {
        pageTabs.firstOrNull()?.page?.let { if (fields.bool("bypass_cache")) it.reloadBypassingCache() else it.reload() }
        respond("B_OK")
      }
      BrowserCodes.BROWSER_GO_BACK -> {
        pageTabs.firstOrNull()?.page?.goBack()
        respond("B_OK")
      }
      BrowserCodes.BROWSER_GO_FORWARD -> {
        pageTabs.firstOrNull()?.page?.goForward()
        respond("B_OK")
      }
      // chrome.history and chrome.bookmarks, answered by the shared core as on air/OS.
      BrowserCodes.BROWSER_HISTORY, BrowserCodes.BROWSER_BOOKMARKS -> {
        val kind = if (fields.int("command") == BrowserCodes.BROWSER_HISTORY) "history" else "bookmarks"
        val answer = JSONObject(SummitCore.browserData(kind, fields.string("method"), fields.string("arguments").ifEmpty { "{}" }))
        if (works.airos.summit.BuildConfig.DEBUG) Log.d(TAG, "$kind.${fields.string("method")}: ${answer.toString().take(300)}")
        val history = answer.optJSONArray("historyEvents") ?: JSONArray()
        for (i in 0 until history.length()) {
          val event = history.getJSONObject(i)
          call(JSONObject().put("op", "historyChanged").put("event", event.optString("event")).put("json", event.optString("json")))
        }
        // Extensions hear of bookmark changes before the answer, as in Chrome.
        forwardBookmarkEvents(answer.optJSONArray("bookmarkEvents"))
        respond(answer.optString("status", "B_ERROR"), error = answer.optString("error"), data = answer.optString("data"))
      }
      // windows.create: Android's Summit has one window, so its pages open as tabs there.
      BrowserCodes.BROWSER_OPEN_WINDOW -> {
        val urls = fields.optJSONArray("url")?.let { a -> (0 until a.length()).map { a.optString(it) } }.orEmpty()
          .filter { it.isNotEmpty() }.map { SummitCore.resolve(it).url }
        var select = fields.bool("focused", true)
        val created = (urls.ifEmpty { listOf("") }).map { url ->
          browser.openTab(url, select).also { select = false }
        }
        respond("B_OK", created.map { pageIdOf(it) }.filter { it != 0L })
        syncTabs()
      }
      BrowserCodes.BROWSER_CLOSE_WINDOW -> respond("B_NOT_ALLOWED", error = "Extensions cannot close the last browser window.")
      BrowserCodes.BROWSER_FOCUS_WINDOW, BrowserCodes.BROWSER_SET_WINDOW_FRAME -> respond("B_OK")
      BrowserCodes.BROWSER_MOVE_TAB -> {
        val tab = pageTabs.firstOrNull()
        if (tab == null) respond("B_ENTRY_NOT_FOUND", error = "The tab is not open in this window.")
        else {
          browser.moveTab(tab, fields.int("index", -1))
          respond("B_OK")
          syncTabs()
        }
      }
      BrowserCodes.BROWSER_SEARCH -> {
        val search = JSONObject(SummitCore.extensionSearch(fields.string("method"), fields.string("arguments").ifEmpty { "{}" }))
        val url = search.optString("url")
        if (url.isEmpty()) respond(search.optString("status", "B_ERROR"), error = search.optString("error"), data = search.optString("data"))
        else {
          when (search.optString("disposition")) {
            "NEW_TAB", "NEW_WINDOW" -> browser.openTab(url, true)
            else -> browser.go(url, pageTabs.firstOrNull() ?: browser.selected)
          }
          respond("B_OK", data = "null")
          syncTabs()
        }
      }
      BrowserCodes.BROWSER_DETECT_LANGUAGE -> respond("B_OK", data = SummitCore.detectLanguage(fields.string("text")))
      // Chrome's favicon permission (chrome-extension://ID/_favicon/?pageUrl=): the icon tabs show.
      BrowserCodes.BROWSER_FAVICON -> {
        val pageUrl = fields.string("page_url")
        val bitmap = browser.tabs.firstOrNull { it.url == pageUrl }?.page?.getFavicon(64)
        val dataURL = bitmap?.let {
          val out = java.io.ByteArrayOutputStream()
          it.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, out)
          "data:image/png;base64," + android.util.Base64.encodeToString(out.toByteArray(), android.util.Base64.NO_WRAP)
        }.orEmpty()
        respond("B_OK", data = JSONObject().put("dataURL", dataURL).toString())
      }
      else -> respond("B_NOT_SUPPORTED", error = "not supported on Android yet")
    }
  }
}

// BMessage fields arrive as lists of values (NativeBrowserCommand::toJSON).
private fun JSONObject.first(name: String): Any? = optJSONArray(name)?.opt(0)
private fun JSONObject.string(name: String): String = first(name)?.toString() ?: ""
private fun JSONObject.strings(name: String): List<String> =
  optJSONArray(name)?.let { a -> (0 until a.length()).map { a.optString(it) } } ?: emptyList()
private fun JSONObject.int(name: String, default: Int = 0): Int = (first(name) as? Number)?.toInt() ?: string(name).toIntOrNull() ?: default
private fun JSONObject.bool(name: String, default: Boolean = false): Boolean = first(name) as? Boolean ?: default
private fun JSONObject.error(): Int = int("error")
private fun JSONObject.objects(name: String): List<JSONObject> =
  optJSONArray(name)?.let { a -> (0 until a.length()).mapNotNull { a.optJSONObject(it)?.optJSONObject("fields") } } ?: emptyList()
