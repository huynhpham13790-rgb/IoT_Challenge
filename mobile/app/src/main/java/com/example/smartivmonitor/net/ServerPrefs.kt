package com.example.smartivmonitor.net

import android.content.Context

/**
 * The one thing this app must be told before it can do anything: which
 * HisServer to talk to. Entered once on the login screen (see LoginActivity)
 * and kept on the device - a nurse on the ward Wi-Fi should not have to
 * retype an IP address every shift.
 */
object ServerPrefs {
    private const val PREFS_NAME = "server_prefs"
    private const val KEY_BASE_URL = "base_url"
    private const val KEY_LAST_USERNAME = "last_username"

    private fun prefs(context: Context) =
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    /** Always ends with "/" - Retrofit's Retrofit.Builder.baseUrl requires it. */
    fun getBaseUrl(context: Context): String? = prefs(context).getString(KEY_BASE_URL, null)

    fun setBaseUrl(context: Context, hostAndPort: String) {
        val trimmed = hostAndPort.trim().trimEnd('/')
        val withScheme = if (trimmed.startsWith("http://") || trimmed.startsWith("https://"))
            trimmed
        else
            "http://$trimmed"
        prefs(context).edit().putString(KEY_BASE_URL, "$withScheme/").apply()
    }

    fun getLastUsername(context: Context): String? = prefs(context).getString(KEY_LAST_USERNAME, null)

    fun setLastUsername(context: Context, username: String) {
        prefs(context).edit().putString(KEY_LAST_USERNAME, username).apply()
    }

    fun clear(context: Context) {
        prefs(context).edit().clear().apply()
    }
}
