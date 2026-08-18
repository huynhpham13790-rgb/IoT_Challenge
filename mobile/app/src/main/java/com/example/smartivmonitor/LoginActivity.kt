package com.example.smartivmonitor

import android.content.Intent
import android.os.Bundle
import android.view.View
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.example.smartivmonitor.databinding.ActivityLoginBinding
import com.example.smartivmonitor.net.ApiClient
import com.example.smartivmonitor.net.LoginRequest
import com.example.smartivmonitor.net.ServerPrefs
import java.io.IOException
import kotlinx.coroutines.launch

/**
 * Entry point of the app. A nurse's device has no built-in way to know
 * which HisServer to talk to, so this screen asks once for the server's
 * LAN address (see ServerPrefs) alongside the usual username/password, then
 * hands off to MainActivity for everything else.
 */
class LoginActivity : AppCompatActivity() {

    private lateinit var binding: ActivityLoginBinding

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityLoginBinding.inflate(layoutInflater)
        setContentView(binding.root)

        prefillSavedValues()
        binding.connectButton.setOnClickListener { attemptLogin() }

        // A nurse who signed in yesterday should not have to sign in again
        // today just because the app process died overnight - if a server is
        // already configured and its session cookie is still valid, skip
        // straight to the ward.
        trySilentLogin()
    }

    private fun prefillSavedValues() {
        ServerPrefs.getBaseUrl(this)?.let { url ->
            val bare = url.removePrefix("https://").removePrefix("http://").trimEnd('/')
            binding.serverAddressEditText.setText(bare)
        }
        ServerPrefs.getLastUsername(this)?.let { binding.usernameEditText.setText(it) }
    }

    private fun trySilentLogin() {
        if (ServerPrefs.getBaseUrl(this) == null) return
        lifecycleScope.launch {
            try {
                val response = ApiClient.service(this@LoginActivity).me()
                if (response.isSuccessful) goToWard()
            } catch (ex: Exception) {
                // No server reachable, or no valid session yet - fine, the
                // nurse just signs in normally below.
            }
        }
    }

    private fun attemptLogin() {
        val serverAddress = binding.serverAddressEditText.text?.toString()?.trim().orEmpty()
        val username = binding.usernameEditText.text?.toString()?.trim().orEmpty()
        val password = binding.passwordEditText.text?.toString().orEmpty()

        if (serverAddress.isEmpty() || username.isEmpty() || password.isEmpty()) {
            showError(getString(R.string.error_missing_fields))
            return
        }

        // The host may have just changed - ApiClient.reset() forces a fresh
        // OkHttpClient/cookie jar for it rather than reusing the previous
        // server's connection.
        ServerPrefs.setBaseUrl(this, serverAddress)
        ApiClient.reset()

        setLoading(true)
        lifecycleScope.launch {
            try {
                val response = ApiClient.service(this@LoginActivity)
                    .login(LoginRequest(username, password))
                when {
                    response.isSuccessful -> {
                        ServerPrefs.setLastUsername(this@LoginActivity, username)
                        goToWard()
                    }
                    response.code() == 401 -> showError(getString(R.string.error_wrong_credentials))
                    else -> showError(getString(R.string.error_generic))
                }
            } catch (ex: IOException) {
                showError(getString(R.string.error_cannot_reach_server))
            } catch (ex: Exception) {
                showError(getString(R.string.error_generic))
            } finally {
                setLoading(false)
            }
        }
    }

    private fun goToWard() {
        startActivity(Intent(this, MainActivity::class.java))
        finish()
    }

    private fun setLoading(loading: Boolean) {
        binding.loadingSpinner.visibility = if (loading) View.VISIBLE else View.GONE
        binding.connectButton.isEnabled = !loading
    }

    private fun showError(message: String) {
        binding.errorText.text = message
        binding.errorText.visibility = View.VISIBLE
    }
}
