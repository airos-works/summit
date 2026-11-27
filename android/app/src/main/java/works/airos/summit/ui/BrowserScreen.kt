package works.airos.summit.ui

import android.content.Intent
import android.view.ViewGroup
import android.widget.FrameLayout
import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.text.TextRange
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.TextFieldValue
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.ui.graphics.SolidColor
import works.airos.summit.browser.Browser
import works.airos.summit.browser.Tab
import works.airos.summit.core.SummitCore

private enum class Panel { Page, Tabs, Library, Settings }

/**
 * Summit's window. Phones get the address field at the top and the toolbar
 * at the bottom, in reach of a thumb (as on air/OS Mobile); tablets and
 * unfolded foldables (600 dp and wider) get one toolbar and a row of tabs at
 * the top, like the desktop.
 */
@Composable
fun BrowserScreen(browser: Browser, onExit: () -> Unit) {
  val tab = browser.selected
  var panel by remember { mutableStateOf(Panel.Page) }
  var menuOpen by remember { mutableStateOf(false) }
  var editing by remember { mutableStateOf(false) }

  BackHandler(enabled = panel != Panel.Page || menuOpen || editing || tab?.canGoBack == true || tab != null) {
    when {
      menuOpen -> menuOpen = false
      panel != Panel.Page -> panel = Panel.Page
      tab?.canGoBack == true -> tab.webView?.goBack()
      else -> onExit()
    }
  }

  SummitTheme(private = tab?.private == true) {
    Surface(color = MaterialTheme.colorScheme.surface, modifier = Modifier.fillMaxSize()) {
      BoxWithConstraints(Modifier.fillMaxSize()) {
        val wide = maxWidth >= 600.dp
        Column(Modifier.fillMaxSize().statusBarsPadding().navigationBarsPadding().imePadding()) {
          when (panel) {
            Panel.Tabs -> TabSwitcher(browser, columns = if (wide) 4 else 2, onDone = { panel = Panel.Page })
            Panel.Library -> LibraryScreen(browser, onOpen = { url ->
              browser.go(url)
              panel = Panel.Page
            }, onDone = { panel = Panel.Page })
            Panel.Settings -> SettingsScreen(browser, onDone = { panel = Panel.Page })
            Panel.Page -> {
              if (wide) {
                Toolbar(browser, tab, wide = true, onEditing = { editing = it },
                  onTabs = { panel = Panel.Tabs }, onLibrary = { panel = Panel.Library }, onMenu = { menuOpen = true })
                TabStrip(browser)
              } else {
                Toolbar(browser, tab, wide = false, onEditing = { editing = it },
                  onTabs = { panel = Panel.Tabs }, onLibrary = { panel = Panel.Library }, onMenu = { menuOpen = true })
              }
              Progress(tab)
              Box(Modifier.weight(1f).fillMaxWidth()) {
                PageHost(tab)
              }
              if (!wide && !editing) {
                BottomBar(browser, tab, onTabs = {
                  tab?.let { browser.capturePicture(it) }
                  panel = Panel.Tabs
                }, onLibrary = { panel = Panel.Library }, onMenu = { menuOpen = true })
              }
            }
          }
        }
        if (menuOpen && tab != null) {
          PageMenu(browser, tab, onDismiss = { menuOpen = false }, onSettings = {
            menuOpen = false
            panel = Panel.Settings
          }, onLibrary = {
            menuOpen = false
            panel = Panel.Library
          })
        }
      }
    }
  }
}

/** The shown tab's page. Only this page has a surface; the others are hidden to the engine. */
@Composable
private fun PageHost(tab: Tab?) {
  AndroidView(
    modifier = Modifier.fillMaxSize(),
    factory = { context -> FrameLayout(context) },
    update = { container ->
      val view = tab?.webView
      if (container.childCount == 1 && container.getChildAt(0) === view) return@AndroidView
      container.removeAllViews()
      if (view != null) {
        (view.parent as? ViewGroup)?.removeView(view)
        container.addView(view, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
      }
    },
    onRelease = { container -> container.removeAllViews() },
  )
}

@Composable
private fun Progress(tab: Tab?) {
  Box(Modifier.fillMaxWidth().height(2.dp)) {
    if (tab != null && tab.loading && tab.progress in 1..99) {
      LinearProgressIndicator(progress = { tab.progress / 100f }, modifier = Modifier.fillMaxWidth().height(2.dp))
    }
  }
}

@Composable
private fun Toolbar(
  browser: Browser,
  tab: Tab?,
  wide: Boolean,
  onEditing: (Boolean) -> Unit,
  onTabs: () -> Unit,
  onLibrary: () -> Unit,
  onMenu: () -> Unit,
) {
  var focused by remember { mutableStateOf(false) }
  Column {
    Row(
      Modifier.fillMaxWidth().padding(horizontal = 6.dp, vertical = 6.dp),
      verticalAlignment = Alignment.CenterVertically,
    ) {
      if (wide && !focused) {
        ToolIcon(SummitIcons.Back, "Back", enabled = tab?.canGoBack == true) { tab?.webView?.goBack() }
        ToolIcon(SummitIcons.Forward, "Forward", enabled = tab?.canGoForward == true) { tab?.webView?.goForward() }
        ReloadStop(tab)
      }
      AddressField(browser, tab, Modifier.weight(1f), onFocus = {
        focused = it
        onEditing(it)
      })
      if (!wide && !focused) ReloadStop(tab)
      if (wide && !focused) {
        BookmarkStar(tab)
        ToolIcon(SummitIcons.Bookmarks, "Bookmarks and history", onClick = onLibrary)
        TabCountButton(browser.tabs.count { it.private == (tab?.private == true) }, onTabs)
        ToolIcon(SummitIcons.Add, "New tab") { browser.newTab(private = tab?.private == true) }
        ToolIcon(SummitIcons.More, "Menu", onClick = onMenu)
      }
    }
    if (focused) Suggestions(browser, tab)
  }
}

@Composable
private fun ReloadStop(tab: Tab?) {
  if (tab?.loading == true) ToolIcon(SummitIcons.Close, "Stop") { tab.webView?.stopLoading() }
  else ToolIcon(SummitIcons.Reload, "Reload", enabled = tab != null) { tab?.webView?.reload() }
}

@Composable
private fun BookmarkStar(tab: Tab?) {
  val url = tab?.url.orEmpty()
  var bookmarked by remember(url) { mutableStateOf(url.isNotEmpty() && SummitCore.isBookmarked(url)) }
  ToolIcon(if (bookmarked) SummitIcons.Star else SummitIcons.StarOutline, "Bookmark", enabled = url.startsWith("http")) {
    bookmarked = SummitCore.toggleBookmark(url, tab?.title.orEmpty())
  }
}

/** The text being typed, shared with the suggestion list. */
private object Typing {
  var text by mutableStateOf("")
}

@Composable
private fun AddressField(browser: Browser, tab: Tab?, modifier: Modifier, onFocus: (Boolean) -> Unit) {
  var focused by remember { mutableStateOf(false) }
  var value by remember { mutableStateOf(TextFieldValue("")) }
  val focusManager = LocalFocusManager.current
  val requester = remember { FocusRequester() }
  val url = tab?.url.orEmpty()
  val secure = url.startsWith("https://")

  LaunchedEffect(tab?.id, url.isEmpty()) {
    // A blank new tab starts with the keyboard up.
    if (tab != null && (url.isEmpty() || url == "about:blank")) runCatching { requester.requestFocus() }
  }

  val shape = RoundedCornerShape(22.dp)
  Row(
    modifier.height(44.dp).clip(shape).background(MaterialTheme.colorScheme.surfaceContainerHighest)
      .padding(horizontal = 12.dp),
    verticalAlignment = Alignment.CenterVertically,
  ) {
    Icon(
      if (focused || url.isEmpty()) SummitIcons.Search else if (secure) SummitIcons.Lock else SummitIcons.Search,
      contentDescription = null,
      modifier = Modifier.size(16.dp),
      tint = MaterialTheme.colorScheme.onSurfaceVariant,
    )
    Spacer(Modifier.width(8.dp))
    Box(Modifier.weight(1f), contentAlignment = Alignment.CenterStart) {
      BasicTextField(
        value = value,
        onValueChange = {
          value = it
          Typing.text = it.text
        },
        singleLine = true,
        textStyle = MaterialTheme.typography.bodyLarge.copy(color = MaterialTheme.colorScheme.onSurface),
        cursorBrush = SolidColor(MaterialTheme.colorScheme.primary),
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri, imeAction = ImeAction.Go, autoCorrectEnabled = false),
        keyboardActions = KeyboardActions(onGo = {
          browser.go(value.text, tab)
          focusManager.clearFocus()
        }),
        modifier = Modifier.fillMaxWidth().focusRequester(requester).onFocusChanged { state ->
          if (state.isFocused == focused) return@onFocusChanged
          focused = state.isFocused
          onFocus(focused)
          value = if (focused) {
            val text = if (url == "about:blank") "" else url
            TextFieldValue(text, TextRange(0, text.length))
          } else TextFieldValue("")
          Typing.text = value.text
        },
      )
      if (!focused) {
        // The site, as Safari and air/OS Mobile show it; the whole address while editing.
        Text(
          if (url.isEmpty() || url == "about:blank") "Search or enter address" else SummitCore.shortAddress(url).substringBefore('/'),
          maxLines = 1,
          overflow = TextOverflow.Ellipsis,
          color = if (url.isEmpty() || url == "about:blank") MaterialTheme.colorScheme.onSurfaceVariant else MaterialTheme.colorScheme.onSurface,
          style = MaterialTheme.typography.bodyLarge,
          modifier = Modifier.fillMaxWidth().clickable { runCatching { requester.requestFocus() } },
        )
      }
    }
    if (focused && value.text.isNotEmpty()) {
      IconButton(onClick = {
        value = TextFieldValue("")
        Typing.text = ""
      }, modifier = Modifier.size(28.dp)) {
        Icon(SummitIcons.Close, "Clear", modifier = Modifier.size(16.dp))
      }
    }
  }
}

/** Pages from history and bookmarks that match what is typed (the same matching as air/OS's address field). */
@Composable
private fun Suggestions(browser: Browser, tab: Tab?) {
  val typed = Typing.text
  val focusManager = LocalFocusManager.current
  val matches = remember(typed) { if (typed.isBlank()) null else SummitCore.match(typed, 8, false) }
  Surface(tonalElevation = 2.dp, modifier = Modifier.fillMaxWidth()) {
    LazyColumn(Modifier.fillMaxWidth().heightIn(max = 360.dp)) {
      if (typed.isNotBlank()) {
        item {
          SuggestionRow(SummitIcons.Search, typed, "Search or go") {
            browser.go(typed, tab)
            focusManager.clearFocus()
          }
        }
      }
      items(matches?.pages.orEmpty()) { page ->
        SuggestionRow(if (page.bookmark) SummitIcons.Star else SummitIcons.Reload, page.title.ifBlank { page.url },
          SummitCore.shortAddress(page.url)) {
          browser.go(page.url, tab)
          focusManager.clearFocus()
        }
      }
    }
  }
}

@Composable
private fun SuggestionRow(icon: ImageVector, title: String, subtitle: String, onClick: () -> Unit) {
  Row(Modifier.fillMaxWidth().clickable(onClick = onClick).padding(horizontal = 16.dp, vertical = 10.dp),
    verticalAlignment = Alignment.CenterVertically) {
    Icon(icon, null, Modifier.size(18.dp), tint = MaterialTheme.colorScheme.onSurfaceVariant)
    Spacer(Modifier.width(14.dp))
    Column(Modifier.weight(1f)) {
      Text(title, maxLines = 1, overflow = TextOverflow.Ellipsis, style = MaterialTheme.typography.bodyLarge)
      Text(subtitle, maxLines = 1, overflow = TextOverflow.Ellipsis, style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
  }
}

@Composable
private fun BottomBar(browser: Browser, tab: Tab?, onTabs: () -> Unit, onLibrary: () -> Unit, onMenu: () -> Unit) {
  Column {
    HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
    Row(Modifier.fillMaxWidth().height(52.dp), horizontalArrangement = Arrangement.SpaceEvenly,
      verticalAlignment = Alignment.CenterVertically) {
      ToolIcon(SummitIcons.Back, "Back", enabled = tab?.canGoBack == true) { tab?.webView?.goBack() }
      ToolIcon(SummitIcons.Forward, "Forward", enabled = tab?.canGoForward == true) { tab?.webView?.goForward() }
      ToolIcon(SummitIcons.Share, "Share and more", onClick = onMenu)
      ToolIcon(SummitIcons.Bookmarks, "Bookmarks and history", onClick = onLibrary)
      TabCountButton(browser.tabs.count { it.private == (tab?.private == true) }, onTabs)
    }
  }
}

@Composable
fun ToolIcon(icon: ImageVector, description: String, enabled: Boolean = true, onClick: () -> Unit) {
  IconButton(onClick = onClick, enabled = enabled) {
    Icon(icon, contentDescription = description, modifier = Modifier.size(22.dp))
  }
}

@Composable
private fun TabCountButton(count: Int, onClick: () -> Unit) {
  IconButton(onClick = onClick) {
    Box(
      Modifier.size(22.dp).border(1.8.dp, MaterialTheme.colorScheme.onSurface, RoundedCornerShape(5.dp)),
      contentAlignment = Alignment.Center,
    ) {
      Text(if (count > 99) ":D" else count.toString(), fontSize = 11.sp, fontWeight = FontWeight.Bold)
    }
  }
}

/** Tablets: the row of tabs under the toolbar. */
@Composable
private fun TabStrip(browser: Browser) {
  val selected = browser.selected
  val tabs = browser.tabs.filter { it.private == (selected?.private == true) }
  Row(
    Modifier.fillMaxWidth().height(36.dp).horizontalScroll(rememberScrollState()).padding(horizontal = 6.dp),
    verticalAlignment = Alignment.CenterVertically,
  ) {
    for (tab in tabs) {
      val current = tab === selected
      Row(
        Modifier.padding(horizontal = 2.dp).widthIn(min = 120.dp, max = 220.dp).height(30.dp)
          .clip(RoundedCornerShape(8.dp))
          .background(if (current) MaterialTheme.colorScheme.secondaryContainer else MaterialTheme.colorScheme.surfaceContainer)
          .clickable { browser.select(tab) }
          .padding(start = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
      ) {
        Text(tab.displayTitle, maxLines = 1, overflow = TextOverflow.Ellipsis, style = MaterialTheme.typography.labelLarge,
          modifier = Modifier.weight(1f))
        IconButton(onClick = { browser.close(tab) }, modifier = Modifier.size(28.dp)) {
          Icon(SummitIcons.Close, "Close tab", Modifier.size(14.dp))
        }
      }
    }
  }
}

@Composable
private fun PageMenu(browser: Browser, tab: Tab, onDismiss: () -> Unit, onSettings: () -> Unit, onLibrary: () -> Unit) {
  val context = LocalContext.current
  val bookmarked = remember(tab.url) { SummitCore.isBookmarked(tab.url) }
  ActionSheet(onDismiss) {
    SheetAction(if (bookmarked) "Remove Bookmark" else "Add Bookmark") {
      SummitCore.toggleBookmark(tab.url, tab.title)
      onDismiss()
    }
    SheetAction("Add to Reading List") {
      SummitCore.addToReadingList(tab.url, tab.title)
      onDismiss()
    }
    SheetAction("Share…") {
      val send = Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_TEXT, tab.url)
        .putExtra(Intent.EXTRA_SUBJECT, tab.title)
      context.startActivity(Intent.createChooser(send, null))
      onDismiss()
    }
    SheetAction(if (tab.desktopSite) "Request Mobile Site" else "Request Desktop Site") {
      tab.desktopSite = !tab.desktopSite
      browser.applySettings(tab)
      tab.webView?.reload()
      onDismiss()
    }
    HorizontalDivider()
    SheetAction("New Tab") {
      browser.newTab()
      onDismiss()
    }
    SheetAction("New Private Tab") {
      browser.newTab(private = true)
      onDismiss()
    }
    SheetAction("Bookmarks, Reading List and History", onClick = onLibrary)
    SheetAction("Settings", onClick = onSettings)
    HorizontalDivider()
    SheetAction("Close Tab", destructive = true) {
      browser.close(tab)
      onDismiss()
    }
  }
}

@Composable
fun ActionSheet(onDismiss: () -> Unit, content: @Composable () -> Unit) {
  Box(Modifier.fillMaxSize().background(MaterialTheme.colorScheme.scrim.copy(alpha = 0.32f)).clickable(onClick = onDismiss),
    contentAlignment = Alignment.BottomCenter) {
    AnimatedVisibility(visible = true) {
      Surface(
        shape = RoundedCornerShape(topStart = 20.dp, topEnd = 20.dp),
        tonalElevation = 3.dp,
        modifier = Modifier.widthIn(max = 560.dp).fillMaxWidth().clickable(enabled = false) {},
      ) {
        Column(Modifier.navigationBarsPadding().padding(vertical = 8.dp)) { content() }
      }
    }
  }
}

@Composable
fun SheetAction(label: String, destructive: Boolean = false, onClick: () -> Unit) {
  TextButton(onClick = onClick, modifier = Modifier.fillMaxWidth().padding(horizontal = 8.dp)) {
    Text(label, modifier = Modifier.fillMaxWidth(), style = MaterialTheme.typography.bodyLarge,
      color = if (destructive) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurface)
  }
}
