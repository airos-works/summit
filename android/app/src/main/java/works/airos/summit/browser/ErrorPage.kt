package works.airos.summit.browser

import android.text.Html

/** The page a tab shows when its page cannot load; the words come from the shared core. */
object ErrorPage {
  fun html(title: String, explanation: String): String {
    val t = Html.escapeHtml(title)
    val e = Html.escapeHtml(explanation)
    return """<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>$t</title>
<style>
:root{color-scheme:light dark}
body{font:16px/1.5 system-ui,sans-serif;margin:0;padding:15vh 24px;max-width:36em;margin:auto}
h1{font-size:22px;font-weight:600;margin:0 0 12px}
p{color:#666}
@media (prefers-color-scheme:dark){p{color:#aaa}}
button{font:inherit;padding:10px 20px;border-radius:20px;border:0;background:#2f6fb5;color:#fff;margin-top:12px}
</style></head><body><h1>$t</h1><p>$e</p>
<button onclick="location.reload()">Try Again</button></body></html>"""
  }
}
