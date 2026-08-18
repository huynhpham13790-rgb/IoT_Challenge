package com.example.smartivmonitor.net

/**
 * Wire shapes for HisServer's JSON responses (System.Text.Json's default
 * camelCase naming policy). Deliberately declares only the fields this app
 * actually reads - Gson ignores whatever else the server sends, so BedApiDto
 * does not have to track BedDto.cs field-for-field (see Models/Dtos.cs on
 * the server for the full shape).
 */

data class LoginRequest(val username: String, val password: String)

data class ChangePasswordRequest(val currentPassword: String, val newPassword: String)

data class MeResponse(
    val userId: Int,
    val username: String,
    val fullName: String?,
    val role: String,
    val mustChangePassword: Boolean,
    val capabilities: List<String>
)

data class ApiErrorResponse(val error: String?)

data class BedApiDto(
    val bedId: String,
    val room: String,
    val status: String,
    val spo2: Int?,
    val heartRate: Int?,
    val temperature: Double?,
    val dripRate: Int?,
    val flowRate: Int?,
    val heartRateSignal: Boolean = true,
    val spo2Signal: Boolean = true,
    val flowSignal: Boolean = true,
    val dripRateSignal: Boolean = true,
    val remainingMl: Int?,
    val remainingMin: Int?,
    val alertMessage: String?,
    val lastUpdated: String?
)
