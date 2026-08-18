package com.example.smartivmonitor.net

import retrofit2.Response
import retrofit2.http.Body
import retrofit2.http.GET
import retrofit2.http.POST

/**
 * Endpoints this app actually calls, mirroring server/src/HisServer/Api.
 * Each function returns a Retrofit Response so the caller can branch on the
 * HTTP status (401 = not signed in, 403 = signed in but lacks the
 * capability) instead of only ever seeing a generic exception.
 */
interface ApiService {

    @POST("api/auth/login")
    suspend fun login(@Body request: LoginRequest): Response<MeResponse>

    @GET("api/auth/me")
    suspend fun me(): Response<MeResponse>

    @POST("api/auth/logout")
    suspend fun logout(): Response<Unit>

    @GET("api/beds")
    suspend fun getBeds(): Response<List<BedApiDto>>
}
