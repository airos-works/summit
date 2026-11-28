package works.airos.summit.ui

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.key
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import org.wpewebkit.wpeview.WebView
import works.airos.summit.extensions.Extensions

/**
 * Installed extensions: Firefox (.xpi), Chrome (.crx) and unpacked (.zip)
 * packages, run by the same engine code as on air/OS. Packages come from a
 * file for now; the stores' "Add to Summit" comes next.
 */
@Composable
fun ExtensionsScreen(extensions: Extensions, onAddFromFile: () -> Unit, onDone: () -> Unit) {
  Column(Modifier.fillMaxSize()) {
    Row(Modifier.fillMaxWidth().padding(horizontal = 4.dp), verticalAlignment = Alignment.CenterVertically) {
      Text("Extensions", style = MaterialTheme.typography.titleLarge, modifier = Modifier.weight(1f).padding(12.dp))
      TextButton(onClick = onAddFromFile) { Text("Add from File…") }
      TextButton(onClick = onDone) { Text("Done") }
    }
    extensions.status?.let {
      Text(it, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp))
    }
    LazyColumn(Modifier.weight(1f)) {
      if (extensions.installed.isEmpty()) {
        item {
          Text("No extensions yet. Add a Firefox (.xpi) or Chrome (.crx) package.",
            color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.padding(24.dp))
        }
      }
      items(extensions.installed, key = { it.identifier }) { extension ->
        Row(Modifier.fillMaxWidth().padding(start = 16.dp, top = 8.dp, bottom = 8.dp), verticalAlignment = Alignment.CenterVertically) {
          Column(Modifier.weight(1f)) {
            Text(extension.name, style = MaterialTheme.typography.bodyLarge)
            Text(extension.error ?: if (extension.loaded) "Version ${extension.version}" else "Loading…",
              style = MaterialTheme.typography.bodySmall,
              color = if (extension.error != null) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant)
          }
          TextButton(onClick = { extensions.remove(extension) }) { Text("Remove") }
        }
      }
    }
  }

}

/** Asks the person to approve an extension being added, wherever they are. */
@Composable
fun ExtensionApprovalDialog(extensions: Extensions) {
  extensions.pending?.let { pending ->
    AlertDialog(
      onDismissRequest = { extensions.approve(false) },
      title = { Text("Add ${pending.name}?") },
      text = {
        Column(Modifier.heightIn(max = 360.dp).verticalScroll(rememberScrollState())) {
          Text("Version ${pending.version}", style = MaterialTheme.typography.bodySmall)
          if (pending.permissions.isNotEmpty() || pending.origins.isNotEmpty()) {
            Text("It can:", modifier = Modifier.padding(top = 8.dp))
            for (p in pending.permissions) Text("• $p", style = MaterialTheme.typography.bodySmall)
            for (o in pending.origins.take(20)) Text("• Use $o", style = MaterialTheme.typography.bodySmall)
            if (pending.origins.size > 20) Text("• …and ${pending.origins.size - 20} more sites", style = MaterialTheme.typography.bodySmall)
          }
        }
      },
      confirmButton = { TextButton(onClick = { extensions.approve(true) }) { Text("Add") } },
      dismissButton = { TextButton(onClick = { extensions.approve(false) }) { Text("Cancel") } },
    )
  }
}

/**
 * An extension's action popup, in a sheet over the page as Firefox for Android
 * shows them: its own view on the popup's page, which is no tab, so the
 * extension's "active tab" stays the page it was opened for.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ExtensionPopupSheet(extensions: Extensions) {
  val popup = extensions.popup ?: return
  val activityContext = LocalContext.current
  ModalBottomSheet(
    onDismissRequest = { extensions.closePopup() },
    sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true),
  ) {
    if (popup.title.isNotEmpty()) {
      Text(popup.title, style = MaterialTheme.typography.titleMedium, modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp))
    }
    // A new popup gets a new view.
    key(popup) {
      AndroidView(
        factory = {
          popup.viewAttached = true
          WebView.createEmpty(activityContext).apply { setPage(popup.page) }
        },
        onRelease = { view ->
          view.setPage(null)
          view.destroy()
          extensions.popupViewReleased(popup)
        },
        modifier = Modifier.fillMaxWidth().fillMaxHeight(0.75f),
      )
    }
  }
}
