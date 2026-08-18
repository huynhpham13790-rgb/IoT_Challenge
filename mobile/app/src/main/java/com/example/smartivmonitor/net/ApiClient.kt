package com.example.smartivmonitor.net

import android.content.Context
import com.example.smartivmonitor.BuildConfig
import java.util.concurrent.TimeUnit
import okhttp3.HttpUrl.Companion.toHttpUrlOrNull
import okhttp3.OkHttpClient
import okhttp3.logging.HttpLoggingInterceptor
import retrofit2.Retrofit
import retrofit2.converter.gson.GsonConverterFactory

/**
 * Builds (and rebuilds, if the configured server address changes - see
 * LoginActivity) a Retrofit client bound to whatever HisServer this
 * install has been pointed at. There is exactly one of these per process;
 * every screen shares the same cookie jar so the session survives moving
 * between fragments.
 */
object ApiClient {
    private var retrofit: Retrofit? = null
    private var cachedBaseUrl: String? = null
    private var cookieJar: PersistentCookieJar? = null

    fun service(context: Context): ApiService {
        val baseUrl = ServerPrefs.getBaseUrl(context)
            ?: error("Server address is not configured yet")

        if (retrofit == null || cachedBaseUrl != baseUrl) {
            val jar = PersistentCookieJar(context)
            cookieJar = jar

            val client = OkHttpClient.Builder()
                .cookieJar(jar)
                .connectTimeout(10, TimeUnit.SECONDS)
                .readTimeout(15, TimeUnit.SECONDS)
                .addInterceptor(HttpLoggingInterceptor().apply {
                    level = if (BuildConfig.DEBUG) HttpLoggingInterceptor.Level.BASIC
                            else HttpLoggingInterceptor.Level.NONE
                })
                .build()

            retrofit = Retrofit.Builder()
                .baseUrl(baseUrl)
                .client(client)
                .addConverterFactory(GsonConverterFactory.create())
                .build()
            cachedBaseUrl = baseUrl
        }

        return retrofit!!.create(ApiService::class.java)
    }

    /** Called on logout: drops the session cookie for the configured host,
     * so the next request to it is anonymous again. */
    fun forgetSession(context: Context) {
        val host = ServerPrefs.getBaseUrl(context)?.toHttpUrlOrNull()?.host ?: return
        cookieJar?.clear(host)
    }

    /** Forces the next service() call to build a fresh client - needed after
     * the server address itself changes, since the old OkHttpClient/cookie
     * jar were bound to the old host. */
    fun reset() {
        retrofit = null
        cachedBaseUrl = null
        cookieJar = null
    }
}
