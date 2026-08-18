package com.example.smartivmonitor

import android.content.Context
import androidx.core.content.ContextCompat
import com.example.smartivmonitor.model.BedStatus

object StatusColors {
    fun of(context: Context, status: BedStatus): Int = ContextCompat.getColor(
        context,
        when (status) {
            BedStatus.CRITICAL -> R.color.status_critical
            BedStatus.WARNING -> R.color.status_warning
            BedStatus.STABLE -> R.color.status_stable
            BedStatus.OFFLINE -> R.color.status_offline
        }
    )
}

object StatusIcons {
    fun of(status: BedStatus): Int = when (status) {
        BedStatus.CRITICAL -> R.drawable.ic_status_critical
        BedStatus.WARNING -> R.drawable.ic_status_warning
        BedStatus.STABLE -> R.drawable.ic_status_stable
        BedStatus.OFFLINE -> R.drawable.ic_status_offline
    }
}
