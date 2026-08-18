package com.example.smartivmonitor

import android.content.res.ColorStateList
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.core.content.ContextCompat
import androidx.recyclerview.widget.DiffUtil
import androidx.recyclerview.widget.ListAdapter
import androidx.recyclerview.widget.RecyclerView
import com.example.smartivmonitor.databinding.ItemBedCardBinding
import com.example.smartivmonitor.model.Bed

class BedAdapter(
    private val onClick: (Bed) -> Unit
) : ListAdapter<Bed, BedAdapter.BedViewHolder>(DIFF) {

    private var selectedBedId: String? = null

    /** Called by the fragment after a card is tapped, so the previously
     * selected card loses its highlight without a full list rebind. */
    fun setSelected(bedId: String?) {
        val previous = selectedBedId
        selectedBedId = bedId
        currentList.forEachIndexed { index, bed ->
            if (bed.bedId == previous || bed.bedId == bedId) notifyItemChanged(index)
        }
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): BedViewHolder {
        val binding = ItemBedCardBinding.inflate(LayoutInflater.from(parent.context), parent, false)
        return BedViewHolder(binding)
    }

    override fun onBindViewHolder(holder: BedViewHolder, position: Int) {
        val bed = getItem(position)
        holder.bind(bed, bed.bedId == selectedBedId)
    }

    inner class BedViewHolder(private val binding: ItemBedCardBinding) :
        RecyclerView.ViewHolder(binding.root) {

        fun bind(bed: Bed, selected: Boolean) {
            val context = binding.root.context
            val color = StatusColors.of(context, bed.status)

            binding.bedIdText.text = bed.bedId
            binding.roomText.text = bed.room
            binding.statusStripe.setBackgroundColor(color)
            binding.statusChip.statusChipIcon.setImageResource(StatusIcons.of(bed.status))
            binding.statusChip.statusChipText.text = bed.status.name
            binding.statusChip.root.backgroundTintList = ColorStateList.valueOf(color)

            binding.vitalSpo2.vitalIcon.setImageResource(R.drawable.ic_spo2)
            binding.vitalSpo2.vitalLabel.text = context.getString(R.string.detail_spo2)
            binding.vitalSpo2.vitalValue.text = Formatting.metric(bed.spo2, "%")

            binding.vitalHr.vitalIcon.setImageResource(R.drawable.ic_heart)
            binding.vitalHr.vitalLabel.text = context.getString(R.string.detail_hr)
            binding.vitalHr.vitalValue.text = Formatting.metric(bed.heartRate, "")

            binding.vitalDrip.vitalIcon.setImageResource(R.drawable.ic_drop)
            binding.vitalDrip.vitalLabel.text = context.getString(R.string.detail_drip)
            binding.vitalDrip.vitalValue.text = Formatting.metric(bed.dripRate, "%")

            binding.alertMessageText.visibility = if (bed.alertMessage.isNullOrBlank()) View.GONE else View.VISIBLE
            binding.alertMessageText.text = bed.alertMessage.orEmpty()

            binding.updatedText.text = Formatting.relativeTime(context, bed.lastUpdated)

            val strokeDp = if (selected) 2f else 1f
            binding.bedCard.strokeWidth = (strokeDp * context.resources.displayMetrics.density).toInt()
            binding.bedCard.strokeColor =
                if (selected) color else ContextCompat.getColor(context, R.color.border)

            binding.root.setOnClickListener { onClick(bed) }
        }
    }

    companion object {
        private val DIFF = object : DiffUtil.ItemCallback<Bed>() {
            override fun areItemsTheSame(oldItem: Bed, newItem: Bed) = oldItem.bedId == newItem.bedId
            override fun areContentsTheSame(oldItem: Bed, newItem: Bed) = oldItem == newItem
        }
    }
}
