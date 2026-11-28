package works.airos.summit.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import works.airos.summit.browser.Browser
import works.airos.summit.core.SummitCore

/** The settings air/OS Mobile's Settings page has; the values are the shared profile's. */
@Composable
fun SettingsScreen(browser: Browser, onDone: () -> Unit) {
  Column(Modifier.fillMaxSize()) {
    Row(Modifier.fillMaxWidth().padding(horizontal = 4.dp), verticalAlignment = Alignment.CenterVertically) {
      Text("Settings", style = MaterialTheme.typography.titleLarge, modifier = Modifier.weight(1f).padding(12.dp))
      TextButton(onClick = {
        browser.saveSession()
        browser.selected?.let { browser.applySettings(it) }
        onDone()
      }) { Text("Done") }
    }
    Column(Modifier.weight(1f).verticalScroll(rememberScrollState())) {
      Heading("Search Engine")
      var engine by remember { mutableStateOf(SummitCore.setting("searchEngine")) }
      for (choice in SummitCore.engines()) {
        Row(Modifier.fillMaxWidth().clickable {
          SummitCore.setSetting("searchEngine", choice.id)
          engine = choice.id
        }.padding(horizontal = 8.dp), verticalAlignment = Alignment.CenterVertically) {
          RadioButton(selected = engine == choice.id, onClick = null, modifier = Modifier.padding(8.dp))
          Text(choice.name)
        }
      }
      HorizontalDivider(Modifier.padding(vertical = 8.dp))
      Heading("Home Page")
      var home by remember { mutableStateOf(SummitCore.setting("homeURL")) }
      OutlinedTextField(home, onValueChange = {
        home = it
        SummitCore.setSetting("homeURL", it)
      }, singleLine = true, placeholder = { Text("about:blank") }, modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp))
      TextButton(onClick = {
        browser.selected?.url?.let {
          home = it
          SummitCore.setSetting("homeURL", it)
        }
      }, modifier = Modifier.padding(horizontal = 8.dp)) { Text("Use This Page as Home Page") }
      HorizontalDivider(Modifier.padding(vertical = 8.dp))
      Toggle("Reopen tabs at start", "restoreSession")
      Toggle("Prevent cross-site tracking", "preventCrossSiteTracking")
      Toggle("Secure connections first (HTTPS)", "httpsFirst")
      Toggle("Save passwords", "savePasswords")
      Toggle("Reduce motion", "reduceMotion")
      Toggle("Unload tabs when memory is low", "unloadWhenMemoryIsLow")
      HorizontalDivider(Modifier.padding(vertical = 8.dp))
      Heading("Autoplay")
      var autoplay by remember { mutableStateOf(SummitCore.setting("autoplay")) }
      for ((id, label) in listOf("allow" to "Allow", "block-audio" to "Block audio", "block" to "Block all")) {
        Row(Modifier.fillMaxWidth().clickable {
          SummitCore.setSetting("autoplay", id)
          autoplay = id
        }.padding(horizontal = 8.dp), verticalAlignment = Alignment.CenterVertically) {
          RadioButton(selected = autoplay == id, onClick = null, modifier = Modifier.padding(8.dp))
          Text(label)
        }
      }
      HorizontalDivider(Modifier.padding(vertical = 8.dp))
      Text(
        "Summit keeps up to ${browser.maxLivePages} pages loaded on this device; other tabs keep their place " +
          "and load again when you go back to them.",
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.padding(16.dp),
      )
    }
  }
}

@Composable
private fun Heading(text: String) {
  Text(text, style = MaterialTheme.typography.titleSmall, color = MaterialTheme.colorScheme.primary,
    modifier = Modifier.padding(start = 16.dp, top = 12.dp, bottom = 4.dp))
}

@Composable
private fun Toggle(label: String, key: String) {
  var on by remember { mutableStateOf(SummitCore.flag(key)) }
  Row(Modifier.fillMaxWidth().clickable {
    on = !on
    SummitCore.setSetting(key, on.toString())
  }.padding(horizontal = 16.dp, vertical = 6.dp), verticalAlignment = Alignment.CenterVertically) {
    Text(label, modifier = Modifier.weight(1f))
    Switch(checked = on, onCheckedChange = {
      on = it
      SummitCore.setSetting(key, it.toString())
    })
  }
}
