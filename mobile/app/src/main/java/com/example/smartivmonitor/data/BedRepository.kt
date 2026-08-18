package com.example.smartivmonitor.data

import android.content.Context
import com.example.smartivmonitor.model.Bed
import com.example.smartivmonitor.model.BedStatus
import com.example.smartivmonitor.net.ApiClient
import com.example.smartivmonitor.net.BedApiDto
import java.time.Instant
import java.time.format.DateTimeParseException

sealed class BedsResult {
    data class Success(val beds: List<Bed>) : BedsResult()

    /** Reached the server but it said no - almost always the session cookie
     * expired, since GET /api/beds only needs Capabilities.ViewWard, which
     * every nurse account already has. */
    object Unauthorized : BedsResult()

    data class Failure(val message: String) : BedsResult()
}

/**
 * Talks to GET /api/beds (server/src/HisServer/Api/BedEndpoints.cs) and maps
 * BedDto's ~40 AI/telemetry fields down to the handful this screen shows.
 *
 * No SignalR client yet - BedListFragment instead polls this on a timer.
 * Simple, and correct is worth more than fashionable for a first cut; a
 * SignalR-backed push connection is a drop-in upgrade behind this same
 * interface later.
 */
class BedRepository(private val context: Context) {

    suspend fun getBeds(): BedsResult {
        return try {
            val response = ApiClient.service(context).getBeds()
            when {
                response.code() == 401 -> BedsResult.Unauthorized
                response.isSuccessful -> {
                    val body = response.body().orEmpty()
                    BedsResult.Success(body.map { it.toBed() })
                }
                else -> BedsResult.Failure("Server trả lỗi ${response.code()}")
            }
        } catch (ex: java.io.IOException) {
            BedsResult.Failure("Không kết nối được tới server")
        } catch (ex: Exception) {
            BedsResult.Failure(ex.message ?: "Lỗi không xác định")
        }
    }

    private fun BedApiDto.toBed(): Bed {
        val status = when (status.uppercase()) {
            "CRITICAL" -> BedStatus.CRITICAL
            "WARNING" -> BedStatus.WARNING
            "STABLE" -> BedStatus.STABLE
            else -> BedStatus.OFFLINE
        }
        return Bed(
            bedId = bedId,
            room = room,
            status = status,
            spo2 = spo2,
            heartRate = heartRate,
            temperature = temperature,
            dripRate = dripRate,
            flowRate = flowRate,
            remainingMl = remainingMl,
            remainingMin = remainingMin,
            alertMessage = alertMessage,
            lastUpdated = lastUpdated.toEpochMillisOrNow(),
            heartRateSignal = heartRateSignal,
            spo2Signal = spo2Signal,
            flowSignal = flowSignal,
            dripRateSignal = dripRateSignal
        )
    }

    private fun String?.toEpochMillisOrNow(): Long {
        if (this.isNullOrBlank()) return System.currentTimeMillis()
        return try {
            Instant.parse(this).toEpochMilli()
        } catch (ex: DateTimeParseException) {
            System.currentTimeMillis()
        }
    }
}
