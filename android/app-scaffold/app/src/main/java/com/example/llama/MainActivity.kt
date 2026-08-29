package com.example.llama

import android.app.ActivityManager
import android.app.AlertDialog
import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.widget.EditText
import android.widget.ImageView
import android.widget.TextView
import android.widget.Toast
import androidx.activity.addCallback
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.arm.aichat.AiChat
import com.arm.aichat.InferenceEngine
import com.arm.aichat.gguf.GgufMetadata
import com.arm.aichat.gguf.GgufMetadataReader
import com.google.android.material.floatingactionbutton.FloatingActionButton
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.io.InputStream
import java.io.RandomAccessFile
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest
import java.util.UUID

class MainActivity : AppCompatActivity() {

    // Android views
    private lateinit var ggufTv: TextView
    private lateinit var messagesRv: RecyclerView
    private lateinit var userInputEt: EditText
    private lateinit var userActionFab: FloatingActionButton
    private lateinit var downloadFab: FloatingActionButton
    private lateinit var settingsIv: ImageView

    // Arm AI Chat inference engine
    private lateinit var engine: InferenceEngine
    private var generationJob: Job? = null

    // Conversation states
    private var isModelReady = false
    private var loadedModelInfo = "No model loaded."
    private val messages = mutableListOf<Message>()
    private val lastAssistantMsg = StringBuilder()
    private val messageAdapter = MessageAdapter(messages) { userName }

    // Persisted across launches in SharedPreferences, mirroring /user <name> in the desktop CLI
    // (see save_config_field/load_config_user_name in wiki-chat.cpp)
    private var userName: String
        get() = getSharedPreferences(PREFS_NAME, MODE_PRIVATE).getString(PREF_KEY_USER_NAME, "") ?: ""
        set(value) = getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit()
            .putString(PREF_KEY_USER_NAME, value).apply()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContentView(R.layout.activity_main)
        // View model boilerplate and state management is out of this basic sample's scope
        onBackPressedDispatcher.addCallback { Log.w(TAG, "Ignore back press for simplicity") }

        // Find views
        ggufTv = findViewById(R.id.gguf)
        messagesRv = findViewById(R.id.messages)
        messagesRv.layoutManager = LinearLayoutManager(this).apply { stackFromEnd = true }
        messagesRv.adapter = messageAdapter
        userInputEt = findViewById(R.id.user_input)
        userActionFab = findViewById(R.id.fab)
        downloadFab = findViewById(R.id.download_fab)
        settingsIv = findViewById(R.id.settings_iv)
        settingsIv.setOnClickListener { showSettingsMenu() }

        // Arm AI Chat initialization
        lifecycleScope.launch(Dispatchers.Default) {
            engine = AiChat.getInferenceEngine(applicationContext)
        }

        // Upon CTA button tapped
        userActionFab.setOnClickListener {
            if (isModelReady) {
                // If model is ready, validate input and send to engine
                handleUserInput()
            } else {
                // Otherwise, prompt user to select a GGUF metadata on the device
                getContent.launch(arrayOf("*/*"))
            }
        }

        // Downloads Byte's own model straight from Byte-AI-Models on GitHub, as an
        // alternative to picking a local file
        downloadFab.setOnClickListener {
            downloadFab.isEnabled = false
            userActionFab.isEnabled = false
            lifecycleScope.launch(Dispatchers.IO) { downloadByteModel() }
        }
    }

    /**
     * The gear icon's menu: set your display name, clear the current chat, or see what's
     * actually available on this Android build (most of the desktop CLI's tool modules --
     * wiki/news/weather/math/etc -- aren't ported yet, see android/README.md).
     */
    private fun showSettingsMenu() {
        val options = arrayOf("Set your name", "Clear chat", "Help / what's available")
        AlertDialog.Builder(this)
            .setTitle("Byte AI settings")
            .setItems(options) { _, which ->
                when (which) {
                    0 -> showSetNameDialog()
                    1 -> clearChat()
                    2 -> showHelpDialog()
                }
            }
            .show()
    }

    private fun showSetNameDialog() {
        val input = EditText(this).apply {
            hint = "Your name"
            setText(userName)
        }
        AlertDialog.Builder(this)
            .setTitle("What should Byte call you?")
            .setView(input)
            .setPositiveButton("Save") { _, _ ->
                userName = input.text.toString().trim()
                messageAdapter.notifyDataSetChanged()
                Toast.makeText(this, "Saved -- Byte will call you \"$userName\"", Toast.LENGTH_SHORT).show()
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun clearChat() {
        messages.clear()
        lastAssistantMsg.clear()
        messageAdapter.notifyDataSetChanged()
    }

    private fun showHelpDialog() {
        AlertDialog.Builder(this)
            .setTitle("Byte AI 4.0 \"Tera\" -- Android")
            .setMessage(
                "Available here:\n" +
                    "- Chat with Byte, powered by your loaded GGUF model\n" +
                    "- Pick a local GGUF file, or download Byte's own model from GitHub\n" +
                    "- Set your name and clear the chat (gear menu)\n" +
                    "- Type /model in chat to see the loaded model's info\n" +
                    "- Math (\"2+3\", \"10 times 5\") and unit conversion (\"10km in miles\")\n" +
                    "- Current date/time, including US timezones (\"what time is it in pacific?\")\n" +
                    "- Live weather (\"weather in Tokyo\"), HackerNews/Dev.to (\"hackernews\"), " +
                    "and Wikipedia lookups for real-world questions\n\n" +
                    "Not yet ported from the desktop CLI: saved/named conversations, /forget, " +
                    "scheduling, and cross-session memory search. See the project's " +
                    "android/README.md for what's planned."
            )
            .setPositiveButton("Got it", null)
            .show()
    }

    private val getContent = registerForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri ->
        Log.i(TAG, "Selected file uri:\n $uri")
        uri?.let { handleSelectedModel(it) }
    }

    /**
     * Handles the file Uri from [getContent] result
     */
    private fun handleSelectedModel(uri: Uri) {
        // Update UI states
        userActionFab.isEnabled = false
        userInputEt.hint = "Parsing GGUF..."
        ggufTv.text = "Parsing metadata from selected file \n$uri"

        lifecycleScope.launch(Dispatchers.IO) {
            // Parse GGUF metadata
            Log.i(TAG, "Parsing GGUF metadata...")
            contentResolver.openInputStream(uri)?.use {
                GgufMetadataReader.create().readStructuredMetadata(it)
            }?.let { metadata ->
                // Update UI to show GGUF metadata to user
                Log.i(TAG, "GGUF parsed: \n$metadata")
                withContext(Dispatchers.Main) {
                    ggufTv.text = metadata.toString()
                }

                // Ensure the model file is available
                val modelName = metadata.filename() + FILE_EXTENSION_GGUF
                contentResolver.openInputStream(uri)?.use { input ->
                    ensureModelFile(modelName, input)
                }?.let { modelFile ->
                    loadModel(modelName, modelFile)

                    withContext(Dispatchers.Main) {
                        isModelReady = true
                        loadedModelInfo = "Byte AI -- $modelName (local file)\n\n$metadata"
                        ggufTv.visibility = android.view.View.GONE
                        userInputEt.hint = "Type and send a message!"
                        userInputEt.isEnabled = true
                        userActionFab.setImageResource(R.drawable.outline_send_24)
                        userActionFab.isEnabled = true
                    }
                }
            }
        }
    }

    /**
     * Downloads Byte's own model (phi3.5-mini, MIT licensed) from the Byte-AI-Models manifest on
     * GitHub -- chunked (GitHub blocks single files over 100MB), resumable, and sha256-verified,
     * mirroring model_fetch.cpp's /downloadmodel logic in the desktop CLI.
     */
    private suspend fun downloadByteModel() = withContext(Dispatchers.IO) {
        try {
            withContext(Dispatchers.Main) { userInputEt.hint = "Fetching manifest..." }

            val manifestUrl = URL(MANIFEST_URL)
            val manifest = JSONObject((manifestUrl.openConnection() as HttpURLConnection).run {
                connectTimeout = 15000
                readTimeout = 15000
                inputStream.bufferedReader().use { it.readText() }.also { disconnect() }
            })

            val chunks = manifest.getJSONArray("chunks")
            val expectedSha = manifest.getString("sha256")
            val baseUrl = MANIFEST_URL.substringBeforeLast('/') + "/"
            val outputFile = File(ensureModelsDirectory(), "phi3.5-mini.gguf")

            // resume support: skip chunks whose bytes are already fully present at the right
            // offset, same as model_fetch.cpp's resume logic
            var offset = if (outputFile.exists()) outputFile.length() else 0L
            var resumeIndex = 0
            var probeOffset = 0L
            for (i in 0 until chunks.length()) {
                val size = chunks.getJSONObject(i).getLong("size")
                if (offset >= probeOffset + size) {
                    probeOffset += size
                    resumeIndex++
                } else {
                    break
                }
            }
            offset = probeOffset

            RandomAccessFile(outputFile, "rw").use { raf ->
                raf.setLength(offset)
                raf.seek(offset)
                for (i in resumeIndex until chunks.length()) {
                    val name = chunks.getJSONObject(i).getString("name")
                    withContext(Dispatchers.Main) {
                        userInputEt.hint = "Downloading model [${i + 1}/${chunks.length()}]..."
                    }
                    val conn = URL(baseUrl + name).openConnection() as HttpURLConnection
                    conn.connectTimeout = 15000
                    conn.readTimeout = 60000
                    conn.inputStream.use { input ->
                        val buf = ByteArray(65536)
                        var n: Int
                        while (input.read(buf).also { n = it } >= 0) {
                            raf.write(buf, 0, n)
                        }
                    }
                    conn.disconnect()
                }
            }

            withContext(Dispatchers.Main) { userInputEt.hint = "Verifying checksum..." }
            val actualSha = MessageDigest.getInstance("SHA-256").let { digest ->
                outputFile.inputStream().use { input ->
                    val buf = ByteArray(1 shl 20)
                    var n: Int
                    while (input.read(buf).also { n = it } >= 0) {
                        digest.update(buf, 0, n)
                    }
                }
                digest.digest().joinToString("") { "%02x".format(it) }
            }

            if (actualSha != expectedSha) {
                withContext(Dispatchers.Main) {
                    userInputEt.hint = "Checksum mismatch -- tap download again to retry."
                    downloadFab.isEnabled = true
                }
                return@withContext
            }

            loadModel(outputFile.name, outputFile)
            withContext(Dispatchers.Main) {
                isModelReady = true
                loadedModelInfo = "Byte AI -- ${outputFile.name} (downloaded from GitHub)"
                ggufTv.visibility = android.view.View.GONE
                userInputEt.hint = "Type and send a message!"
                userInputEt.isEnabled = true
                userActionFab.setImageResource(R.drawable.outline_send_24)
                userActionFab.isEnabled = true
                downloadFab.visibility = android.view.View.GONE
            }
        } catch (e: Exception) {
            Log.e(TAG, "Model download failed", e)
            withContext(Dispatchers.Main) {
                userInputEt.hint = "Download failed -- tap download again to retry."
                downloadFab.isEnabled = true
                userActionFab.isEnabled = true
            }
        }
    }

    /**
     * Prepare the model file within app's private storage
     */
    private suspend fun ensureModelFile(modelName: String, input: InputStream) =
        withContext(Dispatchers.IO) {
            File(ensureModelsDirectory(), modelName).also { file ->
                // Copy the file into local storage if not yet done
                if (!file.exists()) {
                    Log.i(TAG, "Start copying file to $modelName")
                    withContext(Dispatchers.Main) {
                        userInputEt.hint = "Copying file..."
                    }

                    FileOutputStream(file).use { input.copyTo(it) }
                    Log.i(TAG, "Finished copying file to $modelName")
                } else {
                    Log.i(TAG, "File already exists $modelName")
                }
            }
        }

    /**
     * Load the model file from the app private storage
     */
    private suspend fun loadModel(modelName: String, modelFile: File) =
        withContext(Dispatchers.IO) {
            Log.i(TAG, "Loading model $modelName")
            withContext(Dispatchers.Main) {
                userInputEt.hint = "Loading model..."
            }
            engine.loadModel(modelFile.path)
            Log.i(TAG, "Sending Byte system prompt...")
            var prompt = BYTE_SYSTEM_PROMPT + gatherAndroidSpecs()
            if (userName.isNotBlank()) {
                prompt += "\nThe user you're talking to is named $userName; address them by name naturally."
            }
            engine.setSystemPrompt(prompt)
        }

    /**
     * Real Android hardware/platform info (device model, Android version, CPU ABI, RAM), gathered
     * directly from the OS -- mirrors gather_system_specs() in the desktop CLI's wiki-chat.cpp, which
     * bypasses the model for facts it's been shown to unreliably relay/invent rather than trusting it
     * to state them faithfully. There's no fastfetch equivalent to shell out to on Android, so this
     * reads android.os.Build + ActivityManager.MemoryInfo directly instead.
     */
    private fun gatherAndroidSpecs(): String {
        val am = getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
        val memInfo = ActivityManager.MemoryInfo().also { am.getMemoryInfo(it) }
        val totalGb = memInfo.totalMem / 1073741824.0
        val availGb = memInfo.availMem / 1073741824.0

        return "\nplatform: Android ${Build.VERSION.RELEASE} (SDK ${Build.VERSION.SDK_INT})" +
            "\ndevice: ${Build.MANUFACTURER} ${Build.MODEL}" +
            "\ncpu abi: ${Build.SUPPORTED_ABIS.joinToString(", ")}" +
            "\nmemory: %.1f/%.1f GB available/total".format(availGb, totalGb)
    }

    /**
     * Streams one generation pass into a fresh assistant bubble and returns the final,
     * full response text -- factored out so both the initial turn and a model-initiated
     * TOOL: follow-up (see executeToolByName) can reuse the same streaming/UI-update logic.
     */
    private suspend fun streamGeneration(prompt: String): String {
        lastAssistantMsg.clear()
        withContext(Dispatchers.Main) {
            messages.add(Message(UUID.randomUUID().toString(), "", false))
            messageAdapter.notifyDataSetChanged()
        }
        engine.sendUserPrompt(prompt).collect { token ->
            withContext(Dispatchers.Main) {
                val messageCount = messages.size
                check(messageCount > 0 && !messages[messageCount - 1].isUser)
                messages.removeAt(messageCount - 1).copy(
                    content = lastAssistantMsg.append(token).toString()
                ).let { messages.add(it) }
                messageAdapter.notifyItemChanged(messages.size - 1)
            }
        }
        return lastAssistantMsg.toString()
    }

    /**
     * Dispatches a model-requested TOOL: <name> <query> to the matching Tools.kt function
     * (or gatherAndroidSpecs for "specs"), mirroring execute_tool_by_name in wiki-chat.cpp.
     */
    private fun executeToolByName(name: String, query: String): String? = when (name.lowercase()) {
        "math" -> Tools.mathFetch(query)
        "unit" -> Tools.unitFetch(query)
        "datetime" -> Tools.datetimeFetch(query)
        // weatherFetch extracts the location via a "weather in/for/at <place>" regex, so a
        // bare model-provided location needs wrapping first, same fix as the desktop CLI
        // (observed there: asked for Tokyo, got weather for the requester's IP location instead)
        "weather" -> Tools.weatherFetch("weather in $query")
        "news" -> Tools.newsFetch(query)
        "wiki" -> Tools.wikiFetch(query)?.second
        "specs" -> gatherAndroidSpecs()
        else -> null
    }

    /**
     * After a generation pass, checks whether the model's entire response was a TOOL:
     * request instead of an answer; if so, executes it and streams one follow-up
     * generation with the result. Single follow-up only, same as the desktop CLI --
     * the follow-up prompt's phrasing steers the model toward answering, not chaining
     * further tool requests.
     */
    private suspend fun handlePotentialToolRequest(response: String) {
        val (name, query) = Tools.parseToolRequest(response) ?: return
        val result = executeToolByName(name, query)
        val followup = if (result != null) {
            "Tool result for \"$query\": $result\nNow answer the user's original question using this."
        } else {
            null
        }
        if (followup != null) {
            streamGeneration(followup)
        } else {
            withContext(Dispatchers.Main) {
                val messageCount = messages.size
                messages.removeAt(messageCount - 1).copy(
                    content = "I tried to look that up but couldn't get a result."
                ).let { messages.add(it) }
                messageAdapter.notifyItemChanged(messages.size - 1)
            }
        }
    }

    /**
     * Validate and send the user message into [InferenceEngine]
     */
    private fun handleUserInput() {
        val userMsg = userInputEt.text.toString()
        if (userMsg.isEmpty()) {
            Toast.makeText(this, "Input message is empty!", Toast.LENGTH_SHORT).show()
            return
        }
        if (userMsg.trim().equals("/model", ignoreCase = true)) {
            // Mirrors the desktop CLI's /model -- answered directly, never sent to the model
            userInputEt.text = null
            messages.add(Message(UUID.randomUUID().toString(), userMsg, true))
            messages.add(Message(UUID.randomUUID().toString(), loadedModelInfo, false))
            messageAdapter.notifyDataSetChanged()
            return
        }

        userInputEt.text = null
        userInputEt.isEnabled = false
        userActionFab.isEnabled = false
        messages.add(Message(UUID.randomUUID().toString(), userMsg, true))
        messageAdapter.notifyDataSetChanged()

        generationJob = lifecycleScope.launch(Dispatchers.IO) {
            try {
                // Deterministic tools: computed directly and shown as-is, never sent to the
                // model -- same bypass pattern as math/unit/datetime on the desktop CLI, since
                // small local models have repeatedly been shown to garble even correct facts
                // handed to them (see the desktop CLI's commit history for concrete examples)
                val direct = Tools.quickResponse(userMsg, userName)
                    ?: Tools.mathFetch(userMsg)
                    ?: Tools.unitFetch(userMsg)
                    ?: if (Tools.datetimeIsRequested(userMsg)) Tools.datetimeFetch(userMsg) else null

                if (direct != null) {
                    withContext(Dispatchers.Main) {
                        messages.add(Message(UUID.randomUUID().toString(), direct, false))
                        messageAdapter.notifyDataSetChanged()
                    }
                    return@launch
                }

                // Network tools: fetched context is folded into the prompt actually sent to
                // the model, which phrases the final answer -- mirrors the desktop CLI's
                // turn_input augmentation for weather/news/wiki
                var promptForModel = userMsg
                when {
                    Tools.weatherIsRequested(userMsg) -> Tools.weatherFetch(userMsg)?.let { weather ->
                        promptForModel = "A weather API was just called for this request and returned " +
                            "real, current data (not something you need to disclaim): $weather. Report " +
                            "it directly and naturally, with no hedging about data access.\n" +
                            "User request: $userMsg"
                    }
                    Tools.newsIsRequested(userMsg) -> Tools.newsFetch(userMsg)?.let { news ->
                        promptForModel = "Live news feed just fetched for the user's request (you have " +
                            "no other way to know these current headlines, so present this list to " +
                            "them, verbatim titles, as your answer):\n$news\nUser request: $userMsg"
                    }
                    else -> Tools.wikiFetch(userMsg)?.let { (title, extract) ->
                        promptForModel = "Wikipedia summary for \"$title\" (use this to answer, but you " +
                            "may add your own knowledge too):\n$extract\nUser question: $userMsg"
                    }
                }

                // if none of the keyword tools matched this turn, the model may still
                // request one itself via TOOL: <name> <query> -- e.g. a location-only
                // follow-up like "I meant Gresham" that lacks the word "weather"
                val response = streamGeneration(promptForModel)
                handlePotentialToolRequest(response)
            } finally {
                withContext(Dispatchers.Main) {
                    userInputEt.isEnabled = true
                    userActionFab.isEnabled = true
                }
            }
        }
    }

    /**
     * Run a benchmark with the model file
     */
    @Deprecated("This benchmark doesn't accurately indicate GUI performance expected by app developers")
    private suspend fun runBenchmark(modelName: String, modelFile: File) =
        withContext(Dispatchers.Default) {
            Log.i(TAG, "Starts benchmarking $modelName")
            withContext(Dispatchers.Main) {
                userInputEt.hint = "Running benchmark..."
            }
            engine.bench(
                pp=BENCH_PROMPT_PROCESSING_TOKENS,
                tg=BENCH_TOKEN_GENERATION_TOKENS,
                pl=BENCH_SEQUENCE,
                nr=BENCH_REPETITION
            ).let { result ->
                messages.add(Message(UUID.randomUUID().toString(), result, false))
                withContext(Dispatchers.Main) {
                    messageAdapter.notifyItemChanged(messages.size - 1)
                }
            }
        }

    /**
     * Create the `models` directory if not exist.
     */
    private fun ensureModelsDirectory() =
        File(filesDir, DIRECTORY_MODELS).also {
            if (it.exists() && !it.isDirectory) { it.delete() }
            if (!it.exists()) { it.mkdir() }
        }

    override fun onStop() {
        generationJob?.cancel()
        super.onStop()
    }

    override fun onDestroy() {
        engine.destroy()
        super.onDestroy()
    }

    companion object {
        private val TAG = MainActivity::class.java.simpleName

        private const val DIRECTORY_MODELS = "models"
        private const val FILE_EXTENSION_GGUF = ".gguf"

        private const val PREFS_NAME = "byte_prefs"
        private const val PREF_KEY_USER_NAME = "user_name"

        private const val MANIFEST_URL =
            "https://raw.githubusercontent.com/RetroGigabyte/Byte-AI-Models/main/manifest.json"

        private const val BENCH_PROMPT_PROCESSING_TOKENS = 512
        private const val BENCH_TOKEN_GENERATION_TOKENS = 128
        private const val BENCH_SEQUENCE = 1
        private const val BENCH_REPETITION = 3

        // A more direct port of BYTE_SYSTEM_PROMPT from the desktop CLI's wiki-chat.cpp --
        // full module description + the TOOL: protocol (adapted to Android's actual tool
        // set: math/unit/datetime/weather/news/wiki/specs, no session/knowledge-base/
        // scheduling features since those aren't ported yet) + the strengthened "if
        // you're unsure, don't guess" paragraph, all ported essentially verbatim since a
        // trimmed prompt was found to let the model fall back to its default "I don't have
        // real-time access" refusal on follow-up turns the keyword routing didn't catch.
        private const val BYTE_SYSTEM_PROMPT =
            "You are Byte, an AI assistant (Byte AI 4.0 \"Tera\", Android). You are made up of " +
                "several tools working together, and you should describe yourself accurately " +
                "using them when asked what you can do:\n" +
                "- Live knowledge tools: a Wikipedia lookup, a HackerNews/Dev.to news feed, and " +
                "live weather data. Their results are supplementary context, filling gaps in or " +
                "checking facts against what you already know, never overriding your own judgment.\n" +
                "- Exact-computation tools: a calculator, a unit converter (length, weight, " +
                "volume, speed, temperature), and the current date/time (including US timezone " +
                "conversions). Their results are direct facts computed for you, so state them as " +
                "given rather than recomputing them yourself.\n" +
                "- A specs tool: real hardware/platform info for the device you're running on, " +
                "already given to you below.\n" +
                "\n" +
                "Most of the time a relevant tool's result is already given to you above, if one " +
                "applies. But if you genuinely need one of: wiki, news, weather, math, unit, " +
                "datetime, specs -- and none was already provided -- you may request it yourself. " +
                "To do that, reply with ONLY this exact line and nothing else: " +
                "TOOL: <name> <query>  (e.g. \"TOOL: weather Tokyo\", \"TOOL: wiki Eiffel Tower\"). " +
                "The wiki tool in particular is a real, working Wikipedia lookup you have direct " +
                "access to -- use it whenever a question is about a specific real-world person, " +
                "place, thing, or event and no Wikipedia context was already given to you, rather " +
                "than answering from memory alone or declining to answer. Do not say you lack " +
                "real-time or lookup access when a tool is available to you; request it instead. " +
                "Only do this when you truly cannot answer without it; never combine it with other " +
                "text, and never output more than one TOOL: line in a single response -- only the " +
                "first is ever used. If you are instead just describing or listing what tools you " +
                "have (e.g. someone asks \"what can you do\"), describe them in plain prose -- the " +
                "TOOL: syntax is only for actually invoking one, never for listing them.\n" +
                "\n" +
                "If you are not confident in a factual answer -- a specific name, date, number, " +
                "fact about a real person/place/thing/event, or anything you'd be guessing at -- " +
                "look it up with the appropriate tool before answering, rather than stating an " +
                "uncertain guess as if it were fact. Being wrong with confidence is worse than " +
                "taking one extra step to check. This applies especially to wiki for real-world " +
                "entities and news/weather for anything current. Only skip the lookup when you are " +
                "genuinely certain, or when a tool's result was already given to you above.\n" +
                "Answer naturally and concisely. Here is your current device's platform/hardware " +
                "info -- state it directly if asked, don't guess or omit parts of it:"
    }
}

fun GgufMetadata.filename() = when {
    basic.name != null -> {
        basic.name?.let { name ->
            basic.sizeLabel?.let { size ->
                "$name-$size"
            } ?: name
        }
    }
    architecture?.architecture != null -> {
        architecture?.architecture?.let { arch ->
            basic.uuid?.let { uuid ->
                "$arch-$uuid"
            } ?: "$arch-${System.currentTimeMillis()}"
        }
    }
    else -> {
        "model-${System.currentTimeMillis().toHexString()}"
    }
}
