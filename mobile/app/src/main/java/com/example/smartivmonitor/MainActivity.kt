package com.example.smartivmonitor

import android.os.Bundle
import androidx.activity.addCallback
import androidx.activity.enableEdgeToEdge
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import com.example.smartivmonitor.databinding.ActivityMainBinding
import com.example.smartivmonitor.model.Bed

class MainActivity : AppCompatActivity(), BedSelectionListener {

    private lateinit var binding: ActivityMainBinding

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()

        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        ViewCompat.setOnApplyWindowInsetsListener(binding.main) { v, insets ->
            val systemBars = insets.getInsets(WindowInsetsCompat.Type.systemBars())
            v.setPadding(systemBars.left, systemBars.top, systemBars.right, systemBars.bottom)
            insets
        }
        setSupportActionBar(binding.toolbar)
        supportActionBar?.setDisplayShowTitleEnabled(false) // the toolbar draws its own title view

        // On a phone, the system Back button should close the detail pane
        // and return to the bed list before falling through to leaving the
        // app - otherwise a nurse mid-shift loses the app entirely from one
        // accidental back-press while looking at a bed's detail.
        onBackPressedDispatcher.addCallback(this) {
            if (binding.slidingPaneLayout.isSlideable && binding.slidingPaneLayout.isOpen) {
                binding.slidingPaneLayout.closePane()
            } else {
                isEnabled = false
                onBackPressedDispatcher.onBackPressed()
                isEnabled = true
            }
        }
    }

    override fun onBedSelected(bed: Bed) {
        // Fired only from a tap inside BedListFragment, i.e. well after both
        // panes' fragments have a live view - unlike a call from onCreate(),
        // this one is always safe.
        val detailFragment = supportFragmentManager.findFragmentById(R.id.detailPane) as? BedDetailFragment
        detailFragment?.showBed(bed)
        binding.slidingPaneLayout.openPane()
    }

    /** Called by BedDetailFragment's own back button. */
    fun closeDetailPane() {
        binding.slidingPaneLayout.closePane()
    }
}
