package works.airos.summit.automation

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.os.ParcelFileDescriptor
import android.util.Log
import android.view.ViewGroup
import org.wpewebkit.wpe.WPEProcessType
import org.wpewebkit.wpe.services.WPEServiceConnection
import org.wpewebkit.wpe.services.WPEServiceConnectionListener
import org.wpewebkit.wpeview.WebChromeClient
import org.wpewebkit.wpeview.WebContext
import org.wpewebkit.wpeview.WebView
import java.net.ServerSocket
import kotlin.system.exitProcess

/**
 * WebDriver sessions on Summit's engine (debug builds): starts the engine's
 * WebDriver process beside an automation context, and shows the windows
 * WebDriver opens. Their pages are Summit's (its engine and the web features
 * it turns on, Platform/summit/SummitWebPagePreferences.cpp), so test results
 * (Interop, WPT) compare with air/OS's. tools/android/summit-webdriver starts
 * it from the host and forwards WebDriver's port (8888 on the device).
 */
class AutomationActivity : Activity() {
  private lateinit var context: WebContext
  private val windows = mutableListOf<WebView>()
  private val inspectorPort = ServerSocket(0).use { it.localPort }

  private val connectionListener = object : WPEServiceConnectionListener {
    override fun onCleanExit(connection: WPEServiceConnection) {
      Log.i(TAG, "WebDriver ended")
      exitProcess(0)
    }

    override fun onServiceDisconnected(connection: WPEServiceConnection) {
      Log.w(TAG, "WebDriver process disconnected")
    }
  }

  private val chromeClient = object : WebChromeClient() {
    // window.open() in a test page (scoped registries, opener tests): a window
    // of its own, which WebDriver lists and can switch to, as on air/OS.
    override fun onCreateWindow(view: WebView, isUserGesture: Boolean): WebView {
      val client = this
      val window = WebView(this@AutomationActivity, view).apply {
        setWebChromeClient(client)
        settings.javaScriptCanOpenWindowsAutomatically = true
      }
      windows += window
      show(window)
      return window
    }

    override fun onCloseWindow(window: WebView) {
      val shown = window.parent != null
      close(window)
      if (shown) windows.lastOrNull()?.let { show(it) }
    }
  }

  private val automationClient = object : WebContext.Client {
    override fun createWebViewForAutomation(): WebView {
      val window = WebView(context, false).apply {
        setWebChromeClient(chromeClient)
        // As air/OS's test profile, which allows pop-ups on the test origins
        // (tools/interop2026/make-profile.py): tests call window.open() unprompted.
        settings.javaScriptCanOpenWindowsAutomatically = true
        show(this)
        loadUrl("about:blank")
      }
      windows += window
      return window
    }

    // Each session starts without the previous one's windows (and their web processes).
    override fun onAutomationSessionEnded() {
      windows.toList().forEach { close(it) }
    }
  }

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    // The page has the whole display, so that its viewport is the display's
    // size: tests expect 800x600 (wm size 800x600, wm density 160 for 1x).
    window.setDecorFitsSystemWindows(false)
    window.attributes = window.attributes.apply {
      layoutInDisplayCutoutMode = android.view.WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
    }
    try {
      val type = WPEProcessType.WebDriverProcess
      val service = Class.forName("org.wpewebkit.wpe.services." + type.name + "Service")
      val connection = WPEServiceConnection(inspectorPort.toLong(), type, ParcelFileDescriptor.fromFd(0), connectionListener)
      applicationContext.bindService(Intent(applicationContext, service), connection,
        Context.BIND_AUTO_CREATE or Context.BIND_IMPORTANT)
    } catch (e: Exception) {
      Log.e(TAG, "cannot start WebDriver", e)
    }
    // As air/OS's test profile (tools/interop2026/make-profile.py, httpsFirst
    // off): tests load http:// pages on purpose.
    android.system.Os.setenv("SUMMIT_HTTPS_FIRST", "0", true)
    WebContext.enableRemoteInspector(inspectorPort, false)
    context = WebContext(applicationContext, true)
    context.setClient(automationClient)
  }

  private fun show(window: WebView) {
    (window.parent as? ViewGroup)?.removeView(window)
    setContentView(window)
    window.requestFocus()
    // After the window has content (its insets controller comes with it).
    this.window.insetsController?.let {
      it.hide(android.view.WindowInsets.Type.systemBars())
      it.systemBarsBehavior = android.view.WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
    }
  }

  private fun close(window: WebView) {
    (window.parent as? ViewGroup)?.removeView(window)
    windows.remove(window)
    window.destroy()
  }

  override fun onDestroy() {
    context.setClient(null)
    windows.toList().forEach { close(it) }
    context.destroy()
    super.onDestroy()
  }

  private companion object {
    const val TAG = "SummitAutomation"
  }
}
