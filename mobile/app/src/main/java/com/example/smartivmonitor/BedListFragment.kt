package com.example.smartivmonitor

import android.content.Intent
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.core.widget.addTextChangedListener
import androidx.fragment.app.Fragment
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import androidx.recyclerview.widget.LinearLayoutManager
import com.example.smartivmonitor.data.BedRepository
import com.example.smartivmonitor.data.BedsResult
import com.example.smartivmonitor.databinding.FragmentBedListBinding
import com.example.smartivmonitor.databinding.ItemStatTileBinding
import com.example.smartivmonitor.model.Bed
import com.example.smartivmonitor.model.BedStatus
import com.example.smartivmonitor.net.ApiClient
import com.google.android.material.snackbar.Snackbar
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

/** Implemented by MainActivity - the two panes are siblings, not parent/child,
 * so they talk through the activity rather than reaching into each other. */
interface BedSelectionListener {
    fun onBedSelected(bed: Bed)
}

class BedListFragment : Fragment() {

    private var _binding: FragmentBedListBinding? = null
    private val binding get() = _binding!!
    private lateinit var adapter: BedAdapter
    private lateinit var repository: BedRepository

    // The stat tiles at the top always describe the WHOLE ward, even while a
    // filter/search narrows what the list below shows - a nurse filtering
    // down to "Critical" should not lose sight of the total bed count.
    private var allBeds: List<Bed> = emptyList()
    private var statusFilter: BedStatus? = null
    private var searchQuery: String = ""

    override fun onCreateView(
        inflater: LayoutInflater, container: ViewGroup?, savedInstanceState: Bundle?
    ): View {
        _binding = FragmentBedListBinding.inflate(inflater, container, false)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        repository = BedRepository(requireContext())

        adapter = BedAdapter { bed ->
            adapter.setSelected(bed.bedId)
            (activity as? BedSelectionListener)?.onBedSelected(bed)
        }
        binding.bedRecyclerView.layoutManager = LinearLayoutManager(requireContext())
        binding.bedRecyclerView.adapter = adapter

        binding.searchEditText.addTextChangedListener { text ->
            searchQuery = text?.toString()?.trim().orEmpty()
            applyFilters()
        }

        binding.statusFilterGroup.setOnCheckedStateChangeListener { _, checkedIds ->
            statusFilter = when (checkedIds.firstOrNull()) {
                R.id.chipCritical -> BedStatus.CRITICAL
                R.id.chipWarning -> BedStatus.WARNING
                R.id.chipStable -> BedStatus.STABLE
                R.id.chipOffline -> BedStatus.OFFLINE
                else -> null // chipAll, or nothing checked
            }
            applyFilters()
        }

        binding.swipeRefresh.setOnRefreshListener { refresh() }

        startPolling()
    }

    /**
     * No SignalR client yet (see BedRepository) - this polls GET /api/beds
     * every few seconds instead. repeatOnLifecycle pauses the loop the
     * moment this screen is not STARTED (e.g. the detail pane is the only
     * one visible on a phone... actually both panes share one fragment
     * lifecycle here, so in practice this pauses when the app itself is
     * backgrounded) and resumes it automatically, so it never polls into a
     * dead screen.
     */
    private fun startPolling() {
        viewLifecycleOwner.lifecycleScope.launch {
            viewLifecycleOwner.repeatOnLifecycle(Lifecycle.State.STARTED) {
                while (true) {
                    refresh()
                    delay(POLL_INTERVAL_MS)
                }
            }
        }
    }

    private fun refresh() {
        viewLifecycleOwner.lifecycleScope.launch {
            when (val result = repository.getBeds()) {
                is BedsResult.Success -> setBeds(result.beds)
                is BedsResult.Unauthorized -> signOutAndReturnToLogin()
                is BedsResult.Failure -> {
                    // Keep showing the last data we had rather than clearing
                    // the screen over a single dropped request - a nurse
                    // should not see the ward "go blank" from a Wi-Fi blip.
                    if (isAdded) Snackbar.make(binding.root, result.message, Snackbar.LENGTH_SHORT).show()
                }
            }
            binding.swipeRefresh.isRefreshing = false
        }
    }

    private fun signOutAndReturnToLogin() {
        val context = requireContext()
        ApiClient.forgetSession(context)
        startActivity(Intent(context, LoginActivity::class.java))
        requireActivity().finish()
    }

    private fun setBeds(beds: List<Bed>) {
        allBeds = beds
        bindStat(binding.statTotal, beds.size, R.string.stat_total, null)
        bindStat(binding.statCritical, beds.count { it.status == BedStatus.CRITICAL }, R.string.stat_critical, BedStatus.CRITICAL)
        bindStat(binding.statWarning, beds.count { it.status == BedStatus.WARNING }, R.string.stat_warning, BedStatus.WARNING)
        bindStat(binding.statStable, beds.count { it.status == BedStatus.STABLE }, R.string.stat_stable, BedStatus.STABLE)
        bindStat(binding.statOffline, beds.count { it.status == BedStatus.OFFLINE }, R.string.stat_offline, BedStatus.OFFLINE)
        applyFilters()
    }

    private fun bindStat(tile: ItemStatTileBinding, value: Int, labelRes: Int, status: BedStatus?) {
        tile.statValue.text = value.toString()
        tile.statLabel.text = getString(labelRes)
        if (status != null) tile.statValue.setTextColor(StatusColors.of(requireContext(), status))
    }

    private fun applyFilters() {
        val filtered = allBeds.filter { bed ->
            (statusFilter == null || bed.status == statusFilter) &&
                (searchQuery.isEmpty() ||
                    bed.bedId.contains(searchQuery, ignoreCase = true) ||
                    bed.room.contains(searchQuery, ignoreCase = true))
        }
        adapter.submitList(filtered)

        binding.emptyState.visibility = if (filtered.isEmpty()) View.VISIBLE else View.GONE
        binding.emptyState.text = if (allBeds.isEmpty())
            getString(R.string.label_no_beds)
        else
            getString(R.string.label_no_match)
    }

    override fun onDestroyView() {
        super.onDestroyView()
        _binding = null
    }

    companion object {
        private const val POLL_INTERVAL_MS = 4_000L
    }
}
