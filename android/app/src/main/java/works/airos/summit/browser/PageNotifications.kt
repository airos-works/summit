package works.airos.summit.browser

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.app.Notification
import org.wpewebkit.wpeview.WPENotification
import works.airos.summit.core.SummitCore

/** Web notifications (the Notifications API) as Android notifications. Tapping one clicks it in its page. */
class PageNotifications(private val context: Context) {
  private companion object {
    const val CHANNEL = "sites"
    const val ACTION_CLICK = "works.airos.summit.NOTIFICATION_CLICK"
  }

  private val manager = context.getSystemService(NotificationManager::class.java)
  private val shown = mutableMapOf<Int, Pair<Long, WPENotification>>()
  private var nextId = 1

  init {
    manager.createNotificationChannel(NotificationChannel(CHANNEL, "Notifications from sites",
      NotificationManager.IMPORTANCE_DEFAULT))
    val receiver = object : BroadcastReceiver() {
      override fun onReceive(context: Context, intent: Intent) {
        val id = intent.getIntExtra("id", 0)
        shown.remove(id)?.second?.click()
        context.packageManager.getLaunchIntentForPackage(context.packageName)?.let {
          it.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
          context.startActivity(it)
        }
      }
    }
    context.registerReceiver(receiver, IntentFilter(ACTION_CLICK), Context.RECEIVER_NOT_EXPORTED)
  }

  fun show(tab: Tab, notification: WPENotification) {
    notification.replaced?.let { close(it) }
    val id = nextId++
    shown[id] = tab.id to notification
    val click = PendingIntent.getBroadcast(context, id,
      Intent(ACTION_CLICK).setPackage(context.packageName).putExtra("id", id),
      PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
    val built = Notification.Builder(context, CHANNEL)
      .setSmallIcon(android.R.drawable.ic_dialog_info)
      .setContentTitle(notification.title)
      .setContentText(notification.body)
      .setSubText(SummitCore.shortAddress(notification.origin.toString()))
      .setContentIntent(click)
      .setAutoCancel(true)
      .build()
    runCatching { manager.notify(id, built) }
  }

  fun close(notification: WPENotification) {
    val entry = shown.entries.firstOrNull { it.value.second === notification } ?: return
    shown.remove(entry.key)
    manager.cancel(entry.key)
  }

  fun closeFor(tabId: Long) {
    for (key in shown.filterValues { it.first == tabId }.keys) {
      shown.remove(key)
      manager.cancel(key)
    }
  }
}
