package com.example.smartivmonitor

import android.content.res.ColorStateList
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.fragment.app.Fragment
import com.example.smartivmonitor.databinding.FragmentBedDetailBinding
import com.example.smartivmonitor.model.Bed

class BedDetailFragment : Fragment() {

    private var _binding: FragmentBedDetailBinding? = null
    private val binding get() = _binding!!

    override fun onCreateView(
        inflater: LayoutInflater, container: ViewGroup?, savedInstanceState: Bundle?
    ): View {
        _binding = FragmentBedDetailBinding.inflate(inflater, container, false)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        /* Configured here, not by MainActivity reaching in from onCreate():
         * a fragment declared statically via FragmentContainerView's
         * android:name is only guaranteed to have a view by the time its OWN
         * onViewCreated runs - the host Activity's onCreate() can execute
         * before that, and calling into the fragment that early crashes with
         * an NPE from its view binding (see the .kt history for this file).
         *
         * "Phone width" mirrors the same 600dp breakpoint values-w600dp/
         * dimens.xml already uses for the list pane, so the back button
         * shows exactly when SlidingPaneLayout can only show one pane. */
        val isPhoneWidth = resources.configuration.smallestScreenWidthDp < 600
        binding.backButton.visibility = if (isPhoneWidth) View.VISIBLE else View.GONE
        binding.backButton.setOnClickListener {
            (activity as? MainActivity)?.closeDetailPane()
        }
    }

    fun showBed(bed: Bed) {
        val context = requireContext()
        val color = StatusColors.of(context, bed.status)

        binding.emptyDetailState.visibility = View.GONE
        binding.detailScroll.visibility = View.VISIBLE

        binding.detailBedId.text = bed.bedId
        binding.detailRoom.text = bed.room
        binding.detailStatusChip.statusChipIcon.setImageResource(StatusIcons.of(bed.status))
        binding.detailStatusChip.statusChipText.text = bed.status.name
        binding.detailStatusChip.root.backgroundTintList = ColorStateList.valueOf(color)

        if (bed.alertMessage.isNullOrBlank()) {
            binding.alertBanner.visibility = View.GONE
        } else {
            binding.alertBanner.visibility = View.VISIBLE
            binding.alertBanner.backgroundTintList = ColorStateList.valueOf(color)
            binding.alertBannerText.text = bed.alertMessage
        }
        binding.acknowledgeButton.setOnClickListener {
            // TODO: POST /api/alerts/{id}/ack (Capabilities.AckAlerts) once
            // the current alert's id is threaded through to this screen.
            binding.alertBanner.visibility = View.GONE
        }

        binding.bigSpo2.bigVitalIcon.setImageResource(R.drawable.ic_spo2)
        binding.bigSpo2.bigVitalLabel.text = getString(R.string.detail_spo2)
        binding.bigSpo2.bigVitalValue.text = Formatting.metric(bed.spo2, "%")
        binding.bigSpo2.bigVitalSub.visibility = View.GONE

        binding.bigHr.bigVitalIcon.setImageResource(R.drawable.ic_heart)
        binding.bigHr.bigVitalLabel.text = getString(R.string.detail_hr)
        binding.bigHr.bigVitalValue.text = Formatting.metric(bed.heartRate, " bpm")
        binding.bigHr.bigVitalSub.visibility = View.GONE

        binding.bigTemp.bigVitalIcon.setImageResource(R.drawable.ic_thermometer)
        binding.bigTemp.bigVitalLabel.text = getString(R.string.detail_temp)
        binding.bigTemp.bigVitalValue.text = Formatting.metric(bed.temperature, "°C")
        binding.bigTemp.bigVitalSub.visibility = View.GONE

        binding.bigDrip.bigVitalIcon.setImageResource(R.drawable.ic_drop)
        binding.bigDrip.bigVitalLabel.text = getString(R.string.detail_drip)
        binding.bigDrip.bigVitalValue.text = Formatting.metric(bed.dripRate, "%")
        binding.bigDrip.bigVitalSub.visibility = View.GONE

        binding.bigFlow.bigVitalIcon.setImageResource(R.drawable.ic_flow)
        binding.bigFlow.bigVitalLabel.text = getString(R.string.detail_flow)
        binding.bigFlow.bigVitalValue.text = Formatting.metric(bed.flowRate, "%")
        binding.bigFlow.bigVitalSub.visibility = View.GONE

        binding.bigRemaining.bigVitalIcon.setImageResource(R.drawable.ic_remaining)
        binding.bigRemaining.bigVitalLabel.text = getString(R.string.detail_remaining)
        binding.bigRemaining.bigVitalValue.text =
            if (bed.remainingMl != null) "${bed.remainingMl} mL" else getString(R.string.label_unknown)
        if (bed.remainingMin != null) {
            binding.bigRemaining.bigVitalSub.visibility = View.VISIBLE
            binding.bigRemaining.bigVitalSub.text = getString(R.string.label_minutes_left, bed.remainingMin)
        } else {
            binding.bigRemaining.bigVitalSub.visibility = View.GONE
        }

        val lost = bed.lostSignals
        val updated = Formatting.relativeTime(context, bed.lastUpdated)
        binding.detailUpdatedText.text = if (lost.isNotEmpty())
            "${getString(R.string.label_no_signal, lost.joinToString(", "))}  ·  $updated"
        else
            updated
    }

    override fun onDestroyView() {
        super.onDestroyView()
        _binding = null
    }
}
