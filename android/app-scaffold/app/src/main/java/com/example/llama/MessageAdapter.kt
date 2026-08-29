package com.example.llama

import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.ImageView
import android.widget.TextView
import androidx.recyclerview.widget.RecyclerView

data class Message(
    val id: String,
    val content: String,
    val isUser: Boolean,
    // used only by /version -- shows the real Byte AI logo bitmap instead of text, since the
    // desktop CLI's ANSI-colored ASCII art banner doesn't render in a plain TextView
    val isImage: Boolean = false
)

class MessageAdapter(
    private val messages: List<Message>,
    // read live rather than captured once, so a name set mid-session applies retroactively
    // to already-rendered bubbles too, without needing to rebuild the adapter
    private val userDisplayName: () -> String = { "You" }
) : RecyclerView.Adapter<RecyclerView.ViewHolder>() {

    companion object {
        private const val VIEW_TYPE_USER = 1
        private const val VIEW_TYPE_ASSISTANT = 2
        private const val VIEW_TYPE_ASSISTANT_IMAGE = 3
    }

    override fun getItemViewType(position: Int): Int {
        val message = messages[position]
        return when {
            message.isUser -> VIEW_TYPE_USER
            message.isImage -> VIEW_TYPE_ASSISTANT_IMAGE
            else -> VIEW_TYPE_ASSISTANT
        }
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): RecyclerView.ViewHolder {
        val layoutInflater = LayoutInflater.from(parent.context)
        return when (viewType) {
            VIEW_TYPE_USER -> UserMessageViewHolder(layoutInflater.inflate(R.layout.item_message_user, parent, false))
            VIEW_TYPE_ASSISTANT_IMAGE ->
                AssistantImageViewHolder(layoutInflater.inflate(R.layout.item_message_assistant_image, parent, false))
            else -> AssistantMessageViewHolder(layoutInflater.inflate(R.layout.item_message_assistant, parent, false))
        }
    }

    override fun onBindViewHolder(holder: RecyclerView.ViewHolder, position: Int) {
        val message = messages[position]
        if (holder is UserMessageViewHolder || holder is AssistantMessageViewHolder) {
            val textView = holder.itemView.findViewById<TextView>(R.id.msg_content)
            textView.text = message.content
        }
        if (holder is AssistantImageViewHolder) {
            holder.itemView.findViewById<ImageView>(R.id.msg_image).setImageResource(R.drawable.byte_ai_logo)
        }
        if (holder is UserMessageViewHolder) {
            val name = userDisplayName().ifBlank { "You" }
            holder.itemView.findViewById<TextView>(R.id.sender_label).text = name
        }
    }

    override fun getItemCount(): Int = messages.size

    class UserMessageViewHolder(view: View) : RecyclerView.ViewHolder(view)
    class AssistantMessageViewHolder(view: View) : RecyclerView.ViewHolder(view)
    class AssistantImageViewHolder(view: View) : RecyclerView.ViewHolder(view)
}
