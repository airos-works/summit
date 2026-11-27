package works.airos.summit.core

import org.json.JSONArray
import org.json.JSONObject

/**
 * Summit's shared core (src/core), the same C++ air/OS's Summit is built from,
 * through libsummit's JNI bridge (android/app/src/main/cpp/SummitCoreJni.cpp).
 * The profile lives in native memory; call [save] to write profile.json.
 */
object SummitCore {
  init {
    System.loadLibrary("summit")
  }

  /** Opens <directory>/profile.json, restoring a backup when it is damaged. */
  @JvmStatic external fun open(directory: String): String
  /** Writes the profile; returns an error message or "". */
  @JvmStatic external fun save(): String
  @JvmStatic external fun resolveAddress(input: String): String
  @JvmStatic external fun matchAddress(typed: String, limit: Int, complete: Boolean): String
  @JvmStatic external fun visit(url: String, title: String)
  @JvmStatic external fun history(limit: Int): String
  @JvmStatic external fun removeHistory(url: String)
  @JvmStatic external fun clearHistory(since: Long): Int
  @JvmStatic external fun isBookmarked(url: String): Boolean
  @JvmStatic external fun toggleBookmark(url: String, title: String): Boolean
  @JvmStatic external fun bookmarks(): String
  @JvmStatic external fun readingList(): String
  @JvmStatic external fun addToReadingList(url: String, title: String)
  @JvmStatic external fun session(): String
  @JvmStatic external fun setSession(session: String)
  @JvmStatic external fun setting(key: String): String
  @JvmStatic external fun setSetting(key: String, value: String): Boolean
  @JvmStatic external fun searchEngines(): String
  @JvmStatic external fun userAgentPresets(): String
  @JvmStatic external fun mobileUserAgent(): String
  @JvmStatic external fun shortAddress(url: String): String

  data class Resolved(val url: String, val search: Boolean, val error: String)
  data class Page(val url: String, val title: String, val visited: Long = 0, val bookmark: Boolean = false)
  data class Matches(val fillText: String, val fillUrl: String, val pages: List<Page>)
  data class Choice(val id: String, val name: String)

  fun resolve(input: String): Resolved {
    val o = JSONObject(resolveAddress(input))
    return Resolved(o.optString("url"), o.optBoolean("search"), o.optString("error"))
  }

  fun match(typed: String, limit: Int = 8, complete: Boolean = true): Matches {
    val o = JSONObject(matchAddress(typed, limit, complete))
    val fill = o.getJSONObject("fill")
    return Matches(fill.optString("text"), fill.optString("url"), pages(o.getJSONArray("pages")))
  }

  fun historyPages(limit: Int = 400): List<Page> = pages(JSONArray(history(limit)))

  fun readingListPages(): List<Page> = pages(JSONArray(readingList()))

  /** Every bookmark, folders flattened, bar first. */
  fun bookmarkPages(): List<Page> {
    val result = mutableListOf<Page>()
    fun walk(node: JSONObject) {
      if (node.has("url")) result += Page(node.getString("url"), node.optString("title"), bookmark = true)
      val children = node.optJSONArray("children") ?: return
      for (i in 0 until children.length()) walk(children.getJSONObject(i))
    }
    val tree = JSONObject(bookmarks())
    walk(tree.getJSONObject("bar"))
    walk(tree.getJSONObject("other"))
    return result
  }

  fun engines(): List<Choice> = choices(JSONArray(searchEngines()))

  fun flag(key: String): Boolean = setting(key) == "true"

  private fun choices(array: JSONArray) =
    (0 until array.length()).map { array.getJSONObject(it) }.map { Choice(it.getString("id"), it.getString("name")) }

  private fun pages(array: JSONArray): List<Page> = (0 until array.length()).map {
    val p = array.getJSONObject(it)
    Page(p.optString("url"), p.optString("title"), p.optLong("visited"), p.optBoolean("bookmark"))
  }
}
