package works.airos.summit.ui

import android.app.SearchManager
import android.content.Intent
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.ProcessLifecycleOwner
import works.airos.summit.browser.Browser

class MainActivity : ComponentActivity() {
  private lateinit var browser: Browser

  private val appLifecycle = object : DefaultLifecycleObserver {
    override fun onStop(owner: LifecycleOwner) = browser.onBackground()
    override fun onStart(owner: LifecycleOwner) = browser.onForeground()
  }

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    enableEdgeToEdge()
    browser = Browser.get(this)
    browser.attach(this)
    browser.restoreSession()
    if (savedInstanceState == null) handleIntent(intent)
    ProcessLifecycleOwner.get().lifecycle.addObserver(appLifecycle)
    setContent { SummitTheme { BrowserScreen(browser, onExit = { moveTaskToBack(true) }) } }
  }

  override fun onNewIntent(intent: Intent) {
    super.onNewIntent(intent)
    handleIntent(intent)
  }

  override fun onDestroy() {
    ProcessLifecycleOwner.get().lifecycle.removeObserver(appLifecycle)
    browser.saveSession()
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
