package works.airos.summit.ui

import android.app.SearchManager
import android.content.Intent
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.ProcessLifecycleOwner
import works.airos.summit.browser.Browser

class MainActivity : ComponentActivity() {
  private lateinit var browser: Browser
  private var permissionsDone: ((Boolean) -> Unit)? = null
  private val permissionLauncher = registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { result ->
    val done = permissionsDone
    permissionsDone = null
    // Location is enough when the person gives an approximate one.
    done?.invoke(result.isNotEmpty() && result.all { (permission, granted) ->
      granted || (permission == android.Manifest.permission.ACCESS_FINE_LOCATION &&
        result[android.Manifest.permission.ACCESS_COARSE_LOCATION] == true)
    })
  }

  private val appLifecycle = object : DefaultLifecycleObserver {
    override fun onStop(owner: LifecycleOwner) = browser.onBackground()
    override fun onStart(owner: LifecycleOwner) = browser.onForeground()
  }

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    enableEdgeToEdge()
    browser = Browser.get(this)
    browser.attach(this)
    browser.systemPermissions = Browser.SystemPermissions { permissions, done ->
      val missing = permissions.filter { checkSelfPermission(it) != android.content.pm.PackageManager.PERMISSION_GRANTED }
      if (missing.isEmpty()) {
        done(true)
      } else {
        permissionsDone?.invoke(false)
        permissionsDone = done
        val ask = if (android.Manifest.permission.ACCESS_FINE_LOCATION in missing)
          missing + android.Manifest.permission.ACCESS_COARSE_LOCATION else missing
        permissionLauncher.launch(ask.toTypedArray())
      }
    }
    val opensPage = savedInstanceState == null && intent?.action in
      setOf(Intent.ACTION_VIEW, Intent.ACTION_WEB_SEARCH, Intent.ACTION_SEND)
    browser.restoreSession(openHome = !opensPage)
    if (savedInstanceState == null) handleIntent(intent)
    if (browser.tabs.isEmpty()) browser.newTab(browser.homeUrl())
    ProcessLifecycleOwner.get().lifecycle.addObserver(appLifecycle)
    setContent {
      SummitTheme {
        BrowserScreen(browser, onExit = { moveTaskToBack(true) }, onFullscreen = ::setFullscreen)
      }
    }
  }

  /** Pages in fullscreen (video, games) get the whole screen, system bars included. */
  private fun setFullscreen(fullscreen: Boolean) {
    val controller = WindowCompat.getInsetsController(window, window.decorView)
    if (fullscreen) {
      controller.systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
      controller.hide(WindowInsetsCompat.Type.systemBars())
    } else {
      controller.show(WindowInsetsCompat.Type.systemBars())
    }
  }

  override fun onNewIntent(intent: Intent) {
    super.onNewIntent(intent)
    handleIntent(intent)
  }

  override fun onDestroy() {
    ProcessLifecycleOwner.get().lifecycle.removeObserver(appLifecycle)
    browser.saveSession()
    browser.systemPermissions = null
    browser.detach(this)
    super.onDestroy()
  }

  /** Links from other apps open in a new tab; shared text is searched for. */
  private fun handleIntent(intent: Intent?) {
    intent ?: return
    when (intent.action) {
      Intent.ACTION_VIEW -> intent.dataString?.let { browser.go(it, browser.newTab()) }
      Intent.ACTION_WEB_SEARCH -> intent.getStringExtra(SearchManager.QUERY)?.let { browser.go(it, browser.newTab()) }
      Intent.ACTION_SEND -> intent.getStringExtra(Intent.EXTRA_TEXT)?.let { browser.go(it, browser.newTab()) }
    }
  }
}
