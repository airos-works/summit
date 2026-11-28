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
    try {
      val type = WPEProcessType.WebDriverProcess
      val service = Class.forName("org.wpewebkit.wpe.services." + type.name + "Service")
      val connection = WPEServiceConnection(inspectorPort.toLong(), type, ParcelFileDescriptor.fromFd(0), connectionListener)
      applicationContext.bindService(Intent(applicationContext, service), connection,
        Context.BIND_AUTO_CREATE or Context.BIND_IMPORTANT)
    } catch (e: Exception) {
      Log.e(TAG, "cannot start WebDriver", e)
    }
    WebContext.enableRemoteInspector(inspectorPort, false)
    context = WebContext(applicationContext, true)
    context.setClient(automationClient)
  }

  private fun show(window: WebView) {
    (window.parent as? ViewGroup)?.removeView(window)
    setContentView(window)
    window.requestFocus()
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
