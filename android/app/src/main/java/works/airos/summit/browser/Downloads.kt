package works.airos.summit.browser

import android.content.ContentValues
import android.content.Context
import android.os.Environment
import android.provider.MediaStore
import android.util.Log
import android.widget.Toast
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import org.wpewebkit.wpeview.WPEDownload
import java.io.File

/**
 * Downloads go to the app's cache while they run, then into the shared
 * Downloads collection (MediaStore), where the Files app and other apps find
 * them. No storage permission is needed for that on Android 10 and later.
 */
class Downloads(private val context: Context) {
  class Item(val name: String, val url: String) {
    var received by mutableStateOf(0L)
    var total by mutableStateOf(0L)
    var state by mutableStateOf("active") // active, done, failed
  }

  val items = mutableStateListOf<Item>()

  fun start(download: WPEDownload) {
    val name = safeName(download.suggestedFileName.ifBlank { download.url.lastPathSegment ?: "download" })
    val dir = File(context.cacheDir, "downloads").apply { mkdirs() }
    val partial = File(dir, "${System.nanoTime()}-$name")
    val item = Item(name, download.url.toString())
    items.add(0, item)
    Toast.makeText(context, "Downloading $name", Toast.LENGTH_SHORT).show()
    download.start(partial, object : WPEDownload.Listener {
      override fun onProgress(download: WPEDownload, receivedBytes: Long, totalBytes: Long) {
        item.received = receivedBytes
        item.total = totalBytes
      }

      override fun onFinished(download: WPEDownload) {
        item.state = if (publish(partial, name, download.mimeType)) "done" else "failed"
        partial.delete()
        Toast.makeText(context, if (item.state == "done") "Downloaded $name" else "Could not save $name",
          Toast.LENGTH_SHORT).show()
      }

      override fun onFailed(download: WPEDownload, errorCode: Int, description: String) {
        item.state = "failed"
        partial.delete()
        Log.w("Summit", "download of ${item.url} failed: $description")
      }
    })
  }

  private fun publish(file: File, name: String, mimeType: String): Boolean = runCatching {
    val values = ContentValues().apply {
      put(MediaStore.Downloads.DISPLAY_NAME, name)
      put(MediaStore.Downloads.MIME_TYPE, mimeType.ifBlank { "application/octet-stream" })
      put(MediaStore.Downloads.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS)
      put(MediaStore.Downloads.IS_PENDING, 1)
    }
    val resolver = context.contentResolver
    val uri = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values) ?: return false
    resolver.openOutputStream(uri)?.use { out -> file.inputStream().use { it.copyTo(out) } } ?: return false
    values.clear()
    values.put(MediaStore.Downloads.IS_PENDING, 0)
    resolver.update(uri, values, null, null)
    true
  }.getOrElse {
    Log.w("Summit", "saving download $name: $it")
    false
  }

  private fun safeName(name: String) = name.replace(Regex("[/\\\\:*?\"<>|\\u0000-\\u001f]"), "_").take(200)
}
