package com.example.llama

import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder
import java.time.ZoneId
import java.time.ZonedDateTime
import java.time.format.DateTimeFormatter
import java.time.format.TextStyle
import java.util.Locale
import kotlin.math.abs
import kotlin.math.pow
import kotlin.math.roundToLong

/**
 * Kotlin ports of the desktop CLI's deterministic/network tool modules
 * (modules/math_fetch.cpp, unit_fetch.cpp, datetime_fetch.cpp, wiki_fetch.cpp,
 * news_fetch.cpp, weather_fetch.cpp, quick_response.cpp). Pure Kotlin, no
 * native/JNI changes -- network calls reuse the same HttpURLConnection
 * approach as MainActivity's GitHub model download, matching the desktop's
 * "bypass model generation for deterministic/verifiable facts" pattern.
 */
object Tools {

    // ---- math_fetch --------------------------------------------------

    private val wordOps = linkedMapOf(
        "added to" to '+', "plus" to '+', "add" to '+',
        "subtracted from" to '-', "minus" to '-', "subtract" to '-',
        "multiplied by" to '*', "times" to '*', "multiply" to '*',
        "divided by" to '/', "over" to '/', "divide" to '/',
        "modulo" to '%', "mod" to '%',
    )

    private val symbolicRe = Regex("""(-?\d+(?:\.\d+)?)\s*([+\-*/^%])\s*(-?\d+(?:\.\d+)?)""")
    private val unaryRe = Regex("""(-?\d+(?:\.\d+)?)\s+(squared|cubed)""")
    private val numberRe = Regex("""-?\d+(?:\.\d+)?""")

    private fun formatNumber(v: Double): String {
        if (v == Math.floor(v) && abs(v) < 1e15) {
            return v.toLong().toString()
        }
        return "%.6g".format(v).trimEnd('0').trimEnd('.')
    }

    private fun applyOp(a: Double, op: Char, b: Double): Double? = when (op) {
        '+' -> a + b
        '-' -> a - b
        '*' -> a * b
        '/' -> if (b != 0.0) a / b else null
        '^' -> a.pow(b)
        '%' -> if (b != 0.0) a % b else null
        else -> null
    }

    fun mathFetch(query: String): String? {
        unaryRe.find(query)?.let { m ->
            val a = m.groupValues[1].toDouble()
            val exp = if (m.groupValues[2] == "squared") 2.0 else 3.0
            return "${m.groupValues[1]} ${m.groupValues[2]} = ${formatNumber(a.pow(exp))}"
        }
        symbolicRe.find(query)?.let { m ->
            val a = m.groupValues[1].toDouble()
            val op = m.groupValues[2][0]
            val b = m.groupValues[3].toDouble()
            val result = applyOp(a, op, b) ?: return null
            return "${m.groupValues[1]} $op ${m.groupValues[3]} = ${formatNumber(result)}"
        }

        val q = query.lowercase()
        val numbers = numberRe.findAll(q).map { it.value.toDouble() }.toList()
        if (numbers.size < 2) return null

        for ((word, op) in wordOps) {
            if (q.contains(word)) {
                val result = applyOp(numbers[0], op, numbers[1]) ?: return null
                return "${formatNumber(numbers[0])} $op ${formatNumber(numbers[1])} = ${formatNumber(result)}"
            }
        }
        return null
    }

    // ---- unit_fetch ---------------------------------------------------

    private data class UnitInfo(val category: String, val factor: Double, val tempSymbol: Char, val label: String)

    private val unitAliases: Map<String, UnitInfo> = buildMap {
        fun add(names: List<String>, info: UnitInfo) = names.forEach { put(it, info) }
        add(listOf("km", "kilometer", "kilometers"), UnitInfo("length", 1000.0, ' ', "km"))
        add(listOf("m", "meter", "meters"), UnitInfo("length", 1.0, ' ', "m"))
        add(listOf("cm", "centimeter", "centimeters"), UnitInfo("length", 0.01, ' ', "cm"))
        add(listOf("mm", "millimeter", "millimeters"), UnitInfo("length", 0.001, ' ', "mm"))
        add(listOf("mile", "miles", "mi"), UnitInfo("length", 1609.344, ' ', "miles"))
        add(listOf("yard", "yards", "yd"), UnitInfo("length", 0.9144, ' ', "yards"))
        add(listOf("foot", "feet", "ft"), UnitInfo("length", 0.3048, ' ', "feet"))
        add(listOf("inch", "inches"), UnitInfo("length", 0.0254, ' ', "inches"))

        add(listOf("kg", "kilogram", "kilograms"), UnitInfo("weight", 1.0, ' ', "kg"))
        add(listOf("g", "gram", "grams"), UnitInfo("weight", 0.001, ' ', "g"))
        add(listOf("lb", "lbs", "pound", "pounds"), UnitInfo("weight", 0.453592, ' ', "lbs"))
        add(listOf("oz", "ounce", "ounces"), UnitInfo("weight", 0.0283495, ' ', "oz"))
        add(listOf("ton", "tons", "tonne", "tonnes"), UnitInfo("weight", 1000.0, ' ', "tons"))

        add(listOf("l", "liter", "liters", "litre", "litres"), UnitInfo("volume", 1.0, ' ', "liters"))
        add(listOf("ml", "milliliter", "milliliters"), UnitInfo("volume", 0.001, ' ', "ml"))
        add(listOf("gallon", "gallons", "gal"), UnitInfo("volume", 3.78541, ' ', "gallons"))
        add(listOf("quart", "quarts", "qt"), UnitInfo("volume", 0.946353, ' ', "quarts"))
        add(listOf("pint", "pints", "pt"), UnitInfo("volume", 0.473176, ' ', "pints"))
        add(listOf("cup", "cups"), UnitInfo("volume", 0.236588, ' ', "cups"))

        add(listOf("kmh", "kph", "km/h"), UnitInfo("speed", 1.0, ' ', "km/h"))
        add(listOf("mph"), UnitInfo("speed", 1.60934, ' ', "mph"))
        add(listOf("knot", "knots"), UnitInfo("speed", 1.852, ' ', "knots"))

        add(listOf("celsius", "c"), UnitInfo("temp", 0.0, 'C', "Celsius"))
        add(listOf("fahrenheit", "f"), UnitInfo("temp", 0.0, 'F', "Fahrenheit"))
        add(listOf("kelvin", "k"), UnitInfo("temp", 0.0, 'K', "Kelvin"))
    }

    private val conversionRe = Regex("""(-?\d+(?:\.\d+)?)\s*([A-Za-z/]+)\s+(?:in|to)\s+([A-Za-z/]+)""")

    private fun toCelsius(v: Double, symbol: Char) = when (symbol) {
        'F' -> (v - 32.0) * 5.0 / 9.0
        'K' -> v - 273.15
        else -> v
    }

    private fun fromCelsius(c: Double, symbol: Char) = when (symbol) {
        'F' -> c * 9.0 / 5.0 + 32.0
        'K' -> c + 273.15
        else -> c
    }

    private fun formatUnitNumber(v: Double): String {
        if (abs(v - v.roundToLong()) < 1e-9) return v.roundToLong().toString()
        return "%.4g".format(v).trimEnd('0').trimEnd('.')
    }

    fun unitFetch(query: String): String? {
        val m = conversionRe.find(query) ?: return null
        val from = unitAliases[m.groupValues[2].lowercase()] ?: return null
        val to = unitAliases[m.groupValues[3].lowercase()] ?: return null
        if (from.category != to.category) return null

        val value = m.groupValues[1].toDouble()
        val result = if (from.category == "temp") {
            fromCelsius(toCelsius(value, from.tempSymbol), to.tempSymbol)
        } else {
            value * from.factor / to.factor
        }
        return "${formatUnitNumber(value)} ${from.label} = ${formatUnitNumber(result)} ${to.label}"
    }

    // ---- datetime_fetch -------------------------------------------------
    // uses real IANA zones via java.time, which handles DST correctly on its
    // own -- no need to port the desktop's manual UTC-offset/DST bookkeeping

    private val knownZones = linkedMapOf(
        "eastern" to ("Eastern time" to "America/New_York"),
        "central" to ("Central time" to "America/Chicago"),
        "mountain" to ("Mountain time" to "America/Denver"),
        "pacific" to ("Pacific time" to "America/Los_Angeles"),
        "alaska" to ("Alaska time" to "America/Anchorage"),
        "hawaii" to ("Hawaii time" to "Pacific/Honolulu"),
        "utc" to ("UTC" to "UTC"),
        "gmt" to ("GMT" to "UTC"),
    )

    private fun levenshtein(a: String, b: String): Int {
        val d = Array(a.length + 1) { IntArray(b.length + 1) }
        for (i in 0..a.length) d[i][0] = i
        for (j in 0..b.length) d[0][j] = j
        for (i in 1..a.length) {
            for (j in 1..b.length) {
                val cost = if (a[i - 1] == b[j - 1]) 0 else 1
                d[i][j] = minOf(d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + cost)
            }
        }
        return d[a.length][b.length]
    }

    private fun findZone(q: String): Pair<String, String>? {
        for ((keyword, info) in knownZones) {
            if (q.contains(keyword)) return info
        }
        for (word in q.split(Regex("\\s+"))) {
            if (word.length < 4) continue
            for ((keyword, info) in knownZones) {
                if (keyword.length < 4) continue
                if (levenshtein(word, keyword) <= 2) return info
            }
        }
        return null
    }

    fun datetimeIsRequested(query: String): Boolean {
        val q = query.lowercase()
        if (listOf("what time", "current time", "what day", "what date", "today's date", "todays date")
                .any { q.contains(it) }
        ) return true
        return q.contains("time") && findZone(q) != null
    }

    fun datetimeFetch(query: String): String {
        val q = query.lowercase()
        val zone = findZone(q)
        val zdt = if (zone != null) ZonedDateTime.now(ZoneId.of(zone.second)) else ZonedDateTime.now()
        val label = zone?.first ?: zdt.zone.getDisplayName(TextStyle.SHORT, Locale.US)
        val dateFmt = DateTimeFormatter.ofPattern("EEEE MMMM d yyyy", Locale.US)
        val timeFmt = DateTimeFormatter.ofPattern("h:mm a", Locale.US)
        return "Today is ${zdt.format(dateFmt)} at ${zdt.format(timeFmt)} $label"
    }

    // ---- quick_response -------------------------------------------------

    private val quickResponses = mapOf(
        "hello" to "Hey there! What can I help with?",
        "hi" to "Hi! What's up?",
        "hey" to "Hey! What's on your mind?",
        "greetings" to "Greetings! Ask me anything.",
        "good morning" to "Good morning! What can I help with?",
        "good afternoon" to "Good afternoon! What do you need?",
        "good evening" to "Good evening! How can I help?",
        "good night" to "Good night!",
        "goodbye" to "Goodbye! Come back anytime.",
        "bye" to "See you later!",
        "see you" to "See you later!",
        "farewell" to "Farewell!",
        "how are you" to "Doing well, thanks! How can I help?",
        "thanks" to "You're welcome!",
        "thank you" to "You're welcome!",
        "ok" to "Sounds good -- anything else?",
        "okay" to "Sounds good -- anything else?",
        "sure" to "Great, what would you like to know?",
    )

    fun quickResponse(query: String, userName: String): String? {
        val clean = query.lowercase().trim().trimEnd('.', '!', '?', ';', ',')
        if (Regex("""\bwho am i\b""").containsMatchIn(clean)) {
            return if (userName.isBlank()) "You're the one asking the questions! What would you like to know?"
            else "You're $userName!"
        }
        return quickResponses[clean]
    }

    // ---- shared HTTP helper ---------------------------------------------

    private fun httpGet(url: String, userAgent: String? = null): String? = try {
        (URL(url).openConnection() as HttpURLConnection).run {
            connectTimeout = 15000
            readTimeout = 15000
            if (userAgent != null) setRequestProperty("User-Agent", userAgent)
            if (responseCode in 200..299) {
                inputStream.bufferedReader().use { it.readText() }
            } else {
                null
            }.also { disconnect() }
        }
    } catch (e: Exception) {
        null
    }

    // ---- weather_fetch ----------------------------------------------------

    fun weatherIsRequested(query: String) = query.lowercase().contains("weather")

    private val weatherLocRe = Regex("""weather (?:in|for|at)\s+(.+?)[?.!]*$""", RegexOption.IGNORE_CASE)

    /**
     * @param fahrenheit report temperature in Fahrenheit instead of Celsius
     * @param metric report wind speed in km/h instead of mph -- wttr.in already returns
     *   both units per field (temp_C/temp_F, windspeedKmph/windspeedMiles), so this just
     *   picks which one to read rather than doing any conversion math
     */
    fun weatherFetch(query: String, fahrenheit: Boolean = false, metric: Boolean = true): String? {
        val location = weatherLocRe.find(query)?.groupValues?.get(1) ?: ""
        val encoded = URLEncoder.encode(location, "UTF-8")
        val body = httpGet("https://wttr.in/$encoded?format=j1") ?: return null
        return try {
            val data = JSONObject(body)
            val current = data.getJSONArray("current_condition").getJSONObject(0)
            val temp = current.optString(if (fahrenheit) "temp_F" else "temp_C", "?")
            val tempUnit = if (fahrenheit) "F" else "C"
            val humidity = current.optString("humidity", "?")
            val wind = current.optString(if (metric) "windspeedKmph" else "windspeedMiles", "?")
            val windUnit = if (metric) "km/h" else "mph"
            val condition = current.optJSONArray("weatherDesc")?.optJSONObject(0)?.optString("value") ?: "unknown"
            val areaName = data.optJSONArray("nearest_area")?.optJSONObject(0)
                ?.optJSONArray("areaName")?.optJSONObject(0)?.optString("value") ?: "your location"
            "Weather in $areaName: $temp $tempUnit, $condition, humidity $humidity%, wind $wind $windUnit"
        } catch (e: Exception) {
            null
        }
    }

    // ---- news_fetch ---------------------------------------------------

    fun newsIsRequested(query: String): Boolean {
        val q = query.lowercase()
        return listOf("hackernews", "hacker news", "dev.to", "devto", "news").any { q.contains(it) }
    }

    private fun formatDatetime(unixSeconds: Long): String =
        java.text.SimpleDateFormat("M/d/yyyy, h:mm a", Locale.US).format(java.util.Date(unixSeconds * 1000))

    private fun fetchHackerNews(limit: Int): String? {
        val idsBody = httpGet("https://hacker-news.firebaseio.com/v0/topstories.json") ?: return null
        val ids = try { JSONArray(idsBody) } catch (e: Exception) { return null }

        val sb = StringBuilder("HackerNews Top Stories\n\n")
        var count = 0
        var i = 0
        while (i < ids.length() && count < limit) {
            val storyBody = httpGet("https://hacker-news.firebaseio.com/v0/item/${ids.getLong(i)}.json")
            i++
            val story = storyBody?.let { try { JSONObject(it) } catch (e: Exception) { null } } ?: continue
            val title = story.optString("title", "")
            if (title.isEmpty()) continue
            val points = story.optInt("score", 0)
            val time = story.optLong("time", 0)
            sb.append("- $title\n  $points pts | ${formatDatetime(time)}\n\n")
            count++
        }
        return sb.toString()
    }

    private fun fetchDevTo(limit: Int): String? {
        val body = httpGet("https://dev.to/api/articles?per_page=$limit&sort_by=latest") ?: return null
        val articles = try { JSONArray(body) } catch (e: Exception) { return null }

        val sb = StringBuilder("Dev.to Latest Articles\n\n")
        for (i in 0 until articles.length()) {
            val article = articles.getJSONObject(i)
            val reactions = article.optInt("public_reactions_count", 0)
            sb.append("- ${article.optString("title", "")}\n  $reactions reactions\n\n")
        }
        return sb.toString()
    }

    fun newsFetch(query: String): String? {
        val q = query.lowercase()
        val limit = if (q.contains("more")) 20 else 10
        return if (q.contains("dev.to") || q.contains("devto")) fetchDevTo(limit) else fetchHackerNews(limit)
    }

    // ---- wiki_fetch -----------------------------------------------------
    // no local disk cache on Android (desktop caches to ~/Byte/wiki-chat-cache.json);
    // every lookup is a fresh network call, acceptable given this is a mobile client

    private fun extractSentences(extract: String): List<String> {
        val paragraphs = extract.split("\n\n")
        val sentenceRe = Regex("""[.!?]\s+""")
        val sentences = mutableListOf<String>()

        for (para in paragraphs) {
            if (para.length < 10) continue
            for (s in para.split(sentenceRe)) {
                if (s.length in 6..499 && !s.startsWith("See also") && !s.startsWith("References")) {
                    sentences.add(s)
                    if (sentences.size >= 5) break
                }
            }
            if (sentences.size >= 5) break
        }
        if (sentences.isEmpty() && extract.length > 20) {
            sentences.add(extract.take(500))
        }
        return sentences
    }

    // ---- model-initiated tool calling ------------------------------------
    // port of parse_tool_request/the TOOL: protocol from wiki-chat.cpp: lets
    // the model request a tool itself on turns the per-message keyword
    // routing didn't already catch (e.g. a location-only follow-up like
    // "I meant Gresham" that doesn't contain the word "weather")

    /**
     * Parses a model-initiated tool request, only when the model's ENTIRE
     * trimmed response is exactly "TOOL: <name> <query>" -- requiring the
     * whole response to match avoids false positives from the phrase turning
     * up inside ordinary prose. Only the first line is taken as the query,
     * even if the model kept generating past the "TOOL: ..." line instead of
     * stopping there as instructed (same defensive fix as the desktop CLI).
     */
    fun parseToolRequest(response: String): Pair<String, String>? {
        val trimmed = response.trim()
        if (!trimmed.startsWith("TOOL:")) return null
        val rest = trimmed.removePrefix("TOOL:").trim().substringBefore('\n').trim()
        val sep = rest.indexOf(' ')
        if (sep < 0) return null
        return rest.substring(0, sep) to rest.substring(sep + 1)
    }

    fun wikiFetch(query: String): Pair<String, String>? {
        val encoded = URLEncoder.encode(query, "UTF-8")
        val body = httpGet(
            "https://en.wikipedia.org/api/rest_v1/page/summary/$encoded",
            userAgent = "Byte-AI-Tera-Android/1.0 (https://github.com/RealityBit/Byte-AI-Tera)"
        ) ?: return null

        return try {
            val data = JSONObject(body)
            val extract = data.optString("extract", "")
            if (extract.isEmpty()) return null
            val title = data.optString("title", query)
            val sentences = extractSentences(extract)
            if (sentences.isEmpty()) null else title to sentences.joinToString(" ")
        } catch (e: Exception) {
            null
        }
    }
}
