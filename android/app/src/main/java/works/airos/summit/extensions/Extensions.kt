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

  /** Engine events (JSON), on the main thread. */
  @JvmStatic fun onEvent(json: String) {
    Extensions.instance?.onEvent(JSONObject(json))
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
    val path = staged.optString("staged")
    val pkg = staged.optString("package")
    val order = staged.optLong("order")
    if (path.isEmpty()) {
      status = "Could not read the package: ${staged.optString("error")}"
      return
    }
    status = "Checking ${source.name}…"
    call(JSONObject().put("op", "prepare").put("path", path)) { reply ->
      if (reply.error() != 0) {
        ExtensionsNative.discardStaged()
        status = "Not a valid extension: ${reply.string("description").ifEmpty { "error ${reply.error()}" }}"
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
      val id = pageIdOf(tab)
      if (id == 0L) continue
      pages.put(id.toString())
      if (tab === browser.selected) active = id
    }
    call(JSONObject().put("op", "setWindowTabs").put("window", WINDOW.toString()).put("pages", pages)
      .put("active", active.toString()).put("focused", true))
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

  fun pageIdOf(tab: Tab): Long = tab.page?.engineHandle?.let { ExtensionsNative.pageId(it) } ?: 0

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
    fun respond(status: String, pages: List<Long> = emptyList(), error: String = "") {
      call(JSONObject().put("op", "respond").put("identifier", identifier).put("status", status)
        .put("error", error).put("pages", JSONArray(pages.map { it.toString() })))
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
      else -> respond("B_NOT_SUPPORTED", error = "not supported on Android yet")
    }
  }
}

// BMessage fields arrive as lists of values (NativeBrowserCommand::toJSON).
private fun JSONObject.first(name: String): Any? = optJSONArray(name)?.opt(0)
private fun JSONObject.string(name: String): String = first(name)?.toString() ?: ""
private fun JSONObject.strings(name: String): List<String> =
  optJSONArray(name)?.let { a -> (0 until a.length()).map { a.optString(it) } } ?: emptyList()
private fun JSONObject.int(name: String): Int = (first(name) as? Number)?.toInt() ?: string(name).toIntOrNull() ?: 0
private fun JSONObject.bool(name: String, default: Boolean = false): Boolean = first(name) as? Boolean ?: default
private fun JSONObject.error(): Int = int("error")
private fun JSONObject.objects(name: String): List<JSONObject> =
  optJSONArray(name)?.let { a -> (0 until a.length()).mapNotNull { a.optJSONObject(it)?.optJSONObject("fields") } } ?: emptyList()
