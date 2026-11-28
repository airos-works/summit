package works.airos.summit.extensions

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.graphics.drawable.Icon
import org.json.JSONObject

/**
 * Extensions' chrome.notifications as Android notifications, as air/OS shows
 * them with BNotification: the engine asks for them (summitNotificationWPE,
 * ExtensionActionPopupWPE.h), and a tap on one or on its buttons is the
 * extension's notifications.onClicked or onButtonClicked.
 */
class ExtensionNotifications(private val context: Context, private val clicked: (extension: String, notification: String, button: Int?) -> Unit) {
  private companion object {
    const val CHANNEL = "extensions"
    const val ACTION_CLICK = "works.airos.summit.EXTENSION_NOTIFICATION_CLICK"
  }

  private val manager = context.getSystemService(NotificationManager::class.java)

  init {
    manager.createNotificationChannel(NotificationChannel(CHANNEL, "Notifications from extensions",
      NotificationManager.IMPORTANCE_DEFAULT))
    val receiver = object : BroadcastReceiver() {
      override fun onReceive(context: Context, intent: Intent) {
        val extension = intent.getStringExtra("extension") ?: return
        val notification = intent.getStringExtra("notification") ?: return
        val button = intent.getIntExtra("button", -1).takeIf { it >= 0 }
        if (button == null) manager.cancel(tag(extension, notification), 0)
        clicked(extension, notification, button)
      }
    }
    context.registerReceiver(receiver, IntentFilter(ACTION_CLICK), Context.RECEIVER_NOT_EXPORTED)
  }

  private fun tag(extension: String, notification: String) = "$extension\n$notification"

  /** An engine event: {"operation": "present"|"clear", "extension_identifier", "extension_name", "notification", "options"}. */
  fun handle(fields: JSONObject, string: (String) -> String) {
    val extension = string("extension_identifier")
    val notification = string("notification")
    if (extension.isEmpty() || notification.isEmpty()) return
    if (string("operation") == "clear") {
      manager.cancel(tag(extension, notification), 0)
      return
    }
    val options = runCatching { JSONObject(string("options")) }.getOrNull() ?: return
    fun intent(code: Int, button: Int) = PendingIntent.getBroadcast(context, code,
      Intent(ACTION_CLICK).setPackage(context.packageName).putExtra("extension", extension)
        .putExtra("notification", notification).putExtra("button", button),
      PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
    val base = tag(extension, notification).hashCode() * 8
    val message = listOf(options.optString("message"), options.optString("contextMessage")).filter { it.isNotEmpty() }
      .joinToString("\n")
    val builder = Notification.Builder(context, CHANNEL)
      .setSmallIcon(android.R.drawable.ic_dialog_info)
      .setContentTitle(options.optString("title"))
      .setContentText(message)
      .setStyle(Notification.BigTextStyle().bigText(message))
      .setSubText(string("extension_name"))
      .setContentIntent(intent(base, -1))
      .setAutoCancel(true)
    val buttons = options.optJSONArray("buttons")
    for (i in 0 until minOf(buttons?.length() ?: 0, 2)) {
      val title = buttons!!.optJSONObject(i)?.optString("title").orEmpty()
      if (title.isNotEmpty())
        builder.addAction(Notification.Action.Builder(null as Icon?, title, intent(base + 1 + i, i)).build())
    }
    // The same identifier replaces a shown notification in place, as on air/OS.
    runCatching { manager.notify(tag(extension, notification), 0, builder.build()) }
  }
}
