package works.airos.summit.ui

import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import works.airos.summit.browser.Browser
import works.airos.summit.browser.Tab
import kotlin.math.abs
import kotlin.math.roundToInt

/**
 * The tab switcher: a grid of the open pages (Chrome's grid, as on air/OS
 * Mobile) with Tabs | Private segments. Tap a card to go to it; ✕ or a
 * sideways swipe closes it.
 */
@Composable
fun TabSwitcher(browser: Browser, columns: Int, onDone: () -> Unit) {
  val private = browser.showingPrivate
  val tabs = browser.tabs.filter { it.private == private }
  var confirmCloseAll by remember { mutableStateOf(false) }

  Column(Modifier.fillMaxSize()) {
    Row(Modifier.fillMaxWidth().padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
      SingleChoiceSegmentedButtonRow(Modifier.weight(1f)) {
        SegmentedButton(selected = !private, onClick = { browser.showingPrivate = false },
          shape = SegmentedButtonDefaults.itemShape(0, 2)) { Text("Tabs (${browser.tabs.count { !it.private }})") }
        SegmentedButton(selected = private, onClick = { browser.showingPrivate = true },
          shape = SegmentedButtonDefaults.itemShape(1, 2)) { Text("Private") }
      }
    }
    if (tabs.isEmpty() && private) {
      Box(Modifier.weight(1f).fillMaxWidth().padding(32.dp), contentAlignment = Alignment.Center) {
        Text(
          "Private tabs keep no history, and their cookies, storage and cache are gone when the last private tab closes.",
          style = MaterialTheme.typography.bodyLarge,
          color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
      }
    } else {
      LazyVerticalGrid(
        columns = GridCells.Fixed(columns),
        modifier = Modifier.weight(1f),
        contentPadding = PaddingValues(12.dp),
        horizontalArrangement = Arrangement.spacedBy(12.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
      ) {
        items(tabs, key = { it.id }) { tab ->
          TabCard(tab, current = tab === browser.selected, onOpen = {
            browser.select(tab)
            onDone()
          }, onClose = { browser.close(tab) })
        }
      }
    }
    Row(Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 4.dp), verticalAlignment = Alignment.CenterVertically) {
      TextButton(onClick = { confirmCloseAll = true }, enabled = tabs.isNotEmpty()) { Text("Close All") }
      Spacer(Modifier.weight(1f))
      IconButton(onClick = {
        browser.newTab(private = private)
        onDone()
      }) { Icon(SummitIcons.Add, "New tab") }
      Spacer(Modifier.weight(1f))
      TextButton(onClick = {
        if (browser.selected?.private != private) tabs.lastOrNull()?.let { browser.select(it) }
        onDone()
      }) { Text("Done") }
    }
  }

  if (confirmCloseAll) {
    AlertDialog(
      onDismissRequest = { confirmCloseAll = false },
      title = { Text(if (private) "Close all private tabs?" else "Close all tabs?") },
      confirmButton = {
        TextButton(onClick = {
          confirmCloseAll = false
          browser.closeAll(private)
          if (private) browser.showingPrivate = false
        }) { Text("Close All") }
      },
      dismissButton = { TextButton(onClick = { confirmCloseAll = false }) { Text("Cancel") } },
    )
  }
}

@Composable
private fun TabCard(tab: Tab, current: Boolean, onOpen: () -> Unit, onClose: () -> Unit) {
  var dragX by remember { mutableFloatStateOf(0f) }
  val shape = RoundedCornerShape(14.dp)
  Column(
    Modifier
      .offset { IntOffset(dragX.roundToInt(), 0) }
      .alpha((1f - abs(dragX) / 600f).coerceIn(0.2f, 1f))
      .pointerInput(tab.id) {
        detectHorizontalDragGestures(
          onDragEnd = { if (abs(dragX) > size.width * 0.4f) onClose() else dragX = 0f },
          onDragCancel = { dragX = 0f },
        ) { _, amount -> dragX += amount }
      }
      .clip(shape)
      .border(if (current) 3.dp else 1.dp,
        if (current) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.outlineVariant, shape)
      .background(MaterialTheme.colorScheme.surfaceContainer)
      .clickable(onClick = onOpen),
  ) {
    Row(Modifier.fillMaxWidth().height(36.dp).padding(start = 10.dp), verticalAlignment = Alignment.CenterVertically) {
      Text(tab.displayTitle, maxLines = 1, overflow = TextOverflow.Ellipsis, style = MaterialTheme.typography.labelLarge,
        modifier = Modifier.weight(1f))
      IconButton(onClick = onClose, modifier = Modifier.size(32.dp)) { Icon(SummitIcons.Close, "Close", Modifier.size(16.dp)) }
    }
    Box(Modifier.fillMaxWidth().aspectRatio(0.8f).background(MaterialTheme.colorScheme.surfaceContainerHighest)) {
      val picture = tab.picture
      if (picture != null) {
        Image(picture, contentDescription = null, contentScale = ContentScale.Crop, alignment = Alignment.TopCenter,
          modifier = Modifier.fillMaxSize())
      } else {
        Text(tab.url.ifBlank { "New Tab" }, maxLines = 3, overflow = TextOverflow.Ellipsis,
          style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant,
          modifier = Modifier.padding(10.dp))
      }
    }
  }
}
