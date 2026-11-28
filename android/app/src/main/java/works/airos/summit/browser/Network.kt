package works.airos.summit.browser

import android.content.Context
import android.net.ConnectivityManager
import android.net.NetworkCapabilities

object Network {
  fun online(context: Context): Boolean {
    val cm = context.getSystemService(ConnectivityManager::class.java) ?: return true
    val caps = cm.getNetworkCapabilities(cm.activeNetwork) ?: return false
    return caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
  }
}
