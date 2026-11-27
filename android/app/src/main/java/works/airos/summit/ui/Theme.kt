package works.airos.summit.ui

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext

/** Summit's blue (the mountain in the icon) where the system has no dynamic colours. */
private val SummitBlue = Color(0xFF2F6FB5)

@Composable
fun SummitTheme(private: Boolean = false, content: @Composable () -> Unit) {
  val dark = isSystemInDarkTheme() || private
  val context = LocalContext.current
  val scheme = when {
    private -> darkColorScheme(primary = Color(0xFFB9A7FF), surface = Color(0xFF26232E), background = Color(0xFF1C1A22))
    dark -> runCatching { dynamicDarkColorScheme(context) }.getOrElse { darkColorScheme(primary = SummitBlue) }
    else -> runCatching { dynamicLightColorScheme(context) }.getOrElse { lightColorScheme(primary = SummitBlue) }
  }
  MaterialTheme(colorScheme = scheme, content = content)
}
