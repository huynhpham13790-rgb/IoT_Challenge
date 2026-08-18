package com.example.smartivmonitor.model

/** Mirrors the four values BedStatus.cs on the server can hold. */
enum class BedStatus { STABLE, WARNING, CRITICAL, OFFLINE }

/**
 * The subset of BedDto (server/src/HisServer/Models/Dtos.cs) a nurse's
 * dashboard actually needs. No patient name/code here on purpose - this
 * screen answers "which bed needs me", the bed detail patient card is a
 * separate concern once that screen exists.
 */
data class Bed(
    val bedId: String,
    val room: String,
    val status: BedStatus,
    val spo2: Int?,
    val heartRate: Int?,
    val temperature: Double?,
    val dripRate: Int?,
    val flowRate: Int?,
    val remainingMl: Int?,
    val remainingMin: Int?,
    val alertMessage: String?,
    val lastUpdated: Long,
    val heartRateSignal: Boolean = true,
    val spo2Signal: Boolean = true,
    val flowSignal: Boolean = true,
    val dripRateSignal: Boolean = true
) {
    /** Vietnamese channel names for whichever signals are currently lost -
     * matches the "No signal from: ..." wording the server itself uses
     * (see DeviceHealthEvaluator.cs), just localized for this screen. */
    val lostSignals: List<String>
        get() = buildList {
            if (!spo2Signal) add("SpO2")
            if (!heartRateSignal) add("Nhịp tim")
            if (!flowSignal) add("Tốc độ truyền")
            if (!dripRateSignal) add("Tốc độ giọt")
        }
}
