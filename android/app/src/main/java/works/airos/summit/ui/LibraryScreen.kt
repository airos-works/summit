package works.airos.summit.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
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
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import works.airos.summit.browser.Browser
import works.airos.summit.core.SummitCore
import java.text.DateFormat
import java.util.Date

/** Bookmarks, Reading List and History: the profile air/OS's Summit keeps, in the same format. */
@Composable
fun LibraryScreen(browser: Browser, onOpen: (String) -> Unit, onDone: () -> Unit) {
  var section by remember { mutableIntStateOf(0) }
  var revision by remember { mutableIntStateOf(0) }
  var confirmClear by remember { mutableStateOf(false) }
  val pages = remember(section, revision) {
    when (section) {
      0 -> SummitCore.bookmarkPages()
      1 -> SummitCore.readingListPages()
      else -> SummitCore.historyPages()
    }
  }

  Column(Modifier.fillMaxSize()) {
    Row(Modifier.fillMaxWidth().padding(horizontal = 4.dp), verticalAlignment = Alignment.CenterVertically) {
      Text(listOf("Bookmarks", "Reading List", "History")[section], style = MaterialTheme.typography.titleLarge,
        modifier = Modifier.weight(1f).padding(12.dp))
      if (section == 2) TextButton(onClick = { confirmClear = true }) { Text("Clear") }
      TextButton(onClick = onDone) { Text("Done") }
    }
    LazyColumn(Modifier.weight(1f)) {
      if (pages.isEmpty()) {
        item {
          Text(
            listOf("No bookmarks yet. Tap ☆ or Add Bookmark to keep a page here.",
              "Pages saved to read later appear here.", "Pages you visit appear here.")[section],
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(24.dp),
          )
        }
      }
      items(pages) { page ->
        Row(Modifier.fillMaxWidth().clickable { onOpen(page.url) }.padding(start = 16.dp, top = 8.dp, bottom = 8.dp),
          verticalAlignment = Alignment.CenterVertically) {
          Column(Modifier.weight(1f)) {
            Text(page.title.ifBlank { page.url }, maxLines = 1, overflow = TextOverflow.Ellipsis,
              style = MaterialTheme.typography.bodyLarge)
            val detail = if (section == 2 && page.visited > 0)
              DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT).format(Date(page.visited * 1000)) + " · " +
                SummitCore.shortAddress(page.url)
            else SummitCore.shortAddress(page.url)
            Text(detail, maxLines = 1, overflow = TextOverflow.Ellipsis, style = MaterialTheme.typography.bodySmall,
              color = MaterialTheme.colorScheme.onSurfaceVariant)
          }
          if (section != 1) {
            IconButton(onClick = {
              if (section == 0) {
                SummitCore.toggleBookmark(page.url, page.title)
                browser.extensions.bookmarksChanged()
              } else SummitCore.removeHistory(page.url)
              revision++
            }) { Icon(SummitIcons.Close, "Remove") }
          }
        }
      }
    }
    SingleChoiceSegmentedButtonRow(Modifier.fillMaxWidth().padding(12.dp)) {
      listOf("Bookmarks", "Reading List", "History").forEachIndexed { index, label ->
        SegmentedButton(selected = section == index, onClick = { section = index },
          shape = SegmentedButtonDefaults.itemShape(index, 3)) { Text(label, maxLines = 1) }
      }
    }
  }

  if (confirmClear) {
    AlertDialog(
      onDismissRequest = { confirmClear = false },
      title = { Text("Clear history?") },
      text = { Text("Every page in History is removed. Bookmarks and the Reading List are kept.") },
      confirmButton = {
        TextButton(onClick = {
          SummitCore.clearHistory(0)
          browser.saveSession()
          revision++
          confirmClear = false
        }) { Text("Clear History") }
      },
      dismissButton = { TextButton(onClick = { confirmClear = false }) { Text("Cancel") } },
    )
  }
}
