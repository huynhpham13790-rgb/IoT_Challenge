package com.example.smartivmonitor.net

import android.content.Context
import okhttp3.Cookie
import okhttp3.CookieJar
import okhttp3.HttpUrl

/**
 * HisServer authenticates with a cookie (his_session - see Program.cs on the
 * server, "cookie auth rather than tokens"), not a bearer token. A native
 * client has no browser cookie store to lean on, so this is that store:
 * kept in memory for the running process and mirrored to SharedPreferences
 * so a nurse who was signed in yesterday does not have to sign in again
 * today just because the app process was killed overnight.
 */
class PersistentCookieJar(context: Context) : CookieJar {

    private val prefs = context.applicationContext
        .getSharedPreferences("cookie_jar", Context.MODE_PRIVATE)
    private val cache = mutableMapOf<String, List<Cookie>>()

    override fun saveFromResponse(url: HttpUrl, cookies: List<Cookie>) {
        if (cookies.isEmpty()) return
        cache[url.host] = cookies
        val serialized = cookies.joinToString(";") { "${it.name}=${it.value}" }
        prefs.edit().putString(url.host, serialized).apply()
    }

    override fun loadForRequest(url: HttpUrl): List<Cookie> {
        cache[url.host]?.let { return it }

        val serialized = prefs.getString(url.host, null) ?: return emptyList()
        val cookies = serialized.split(";").mapNotNull { pair ->
            val parts = pair.split("=", limit = 2)
            if (parts.size != 2) return@mapNotNull null
            Cookie.Builder()
                .name(parts[0])
                .value(parts[1])
                .domain(url.host)
                .build()
        }
        cache[url.host] = cookies
        return cookies
    }

    fun clear(host: String) {
        cache.remove(host)
        prefs.edit().remove(host).apply()
    }
}
