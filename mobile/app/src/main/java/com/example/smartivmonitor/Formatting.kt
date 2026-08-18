package com.example.smartivmonitor

import android.content.Context
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

object Formatting {
    /** "--" for a missing reading, same convention as the web console's
     * formatMetric - a dash reads as "unknown", never as zero. */
    fun metric(value: Int?, suffix: String): String =
        if (value == null) "--" else "$value$suffix"

    fun metric(value: Double?, suffix: String): String =
        if (value == null) "--" else "${String.format(Locale.getDefault(), "%.1f", value)}$suffix"

    fun relativeTime(context: Context, timestampMillis: Long): String {
        val diffSeconds = (System.currentTimeMillis() - timestampMillis) / 1000
        return when {
            diffSeconds < 5 -> context.getString(R.string.label_updated_now)
            diffSeconds < 60 -> context.getString(R.string.label_updated_seconds_ago, diffSeconds)
            diffSeconds < 3600 -> context.getString(R.string.label_updated_minutes_ago, diffSeconds / 60)
            else -> {
                val time = SimpleDateFormat("HH:mm:ss", Locale.getDefault()).format(Date(timestampMillis))
                context.getString(R.string.label_updated_at, time)
            }
        }
    }
}
