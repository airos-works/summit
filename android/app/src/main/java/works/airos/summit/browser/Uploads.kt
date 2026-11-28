package works.airos.summit.browser

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import android.util.Log
import java.io.File

/**
 * Files picked for a page's file field. The engine reads files by path, and
 * documents from the picker are content: URIs, so they are copied into the
 * app's cache (which the web processes, running as the app, can read). Old
 * copies are removed after a day.
 */
object Uploads {
  private const val KEEP_MS = 24 * 60 * 60 * 1000L

  fun copy(context: Context, uris: List<Uri>): List<String> {
    val dir = File(context.cacheDir, "uploads").apply { mkdirs() }
    dir.listFiles()?.forEach { if (System.currentTimeMillis() - it.lastModified() > KEEP_MS) it.deleteRecursively() }
    return uris.mapNotNull { uri ->
      runCatching {
        val name = displayName(context, uri).replace(Regex("[/\\\\:*?\"<>|]"), "_").ifBlank { "upload" }
        val target = File(File(dir, System.nanoTime().toString()).apply { mkdirs() }, name)
        context.contentResolver.openInputStream(uri)!!.use { input -> target.outputStream().use { input.copyTo(it) } }
        target.absolutePath
      }.onFailure { Log.w("Summit", "upload $uri: $it") }.getOrNull()
    }
  }

  private fun displayName(context: Context, uri: Uri): String =
    context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use {
      if (it.moveToFirst()) it.getString(0) else null
    } ?: uri.lastPathSegment ?: "upload"
}
