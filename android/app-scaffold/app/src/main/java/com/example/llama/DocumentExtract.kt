package com.example.llama

import android.content.Context
import android.net.Uri
import android.util.Xml
import com.tom_roush.pdfbox.android.PDFBoxResourceLoader
import com.tom_roush.pdfbox.pdmodel.PDDocument
import com.tom_roush.pdfbox.text.PDFTextStripper
import org.xmlpull.v1.XmlPullParser
import java.util.zip.ZipInputStream

/**
 * Text extraction for uploaded documents (PDF, Word .docx) -- there's no desktop CLI
 * equivalent to port; this is an Android-only feature. Both extractors return only plain
 * text, truncated to a size sane for a small local model's context window, since the whole
 * point is feeding it in as chat context (see handleAttachment() in MainActivity.kt), not
 * archiving the file.
 */
object DocumentExtract {

    private const val MAX_CHARS = 12000

    private var pdfBoxInitialized = false

    private fun truncate(text: String): String =
        if (text.length > MAX_CHARS) {
            text.take(MAX_CHARS) + "\n\n[...document truncated at ${MAX_CHARS} characters...]"
        } else {
            text
        }

    /**
     * PDFBox-Android (com.tom-roush:pdfbox-android) -- a real, actively maintained Android
     * port of Apache PDFBox, rather than hand-rolling PDF text extraction.
     */
    fun extractPdfText(context: Context, uri: Uri): String? {
        if (!pdfBoxInitialized) {
            PDFBoxResourceLoader.init(context.applicationContext)
            pdfBoxInitialized = true
        }
        return try {
            context.contentResolver.openInputStream(uri)?.use { input ->
                PDDocument.load(input).use { doc ->
                    truncate(PDFTextStripper().getText(doc))
                }
            }
        } catch (e: Exception) {
            null
        }
    }

    /**
     * .docx is just a ZIP archive containing word/document.xml with the actual text runs in
     * <w:t> elements -- no library needed, just java.util.zip + Android's built-in
     * XmlPullParser, avoiding a heavier dependency like Apache POI for one file format.
     */
    fun extractDocxText(context: Context, uri: Uri): String? = try {
        context.contentResolver.openInputStream(uri)?.use { input ->
            ZipInputStream(input).use { zip ->
                var entry = zip.nextEntry
                var documentXml: String? = null
                while (entry != null) {
                    if (entry.name == "word/document.xml") {
                        documentXml = zip.bufferedReader().readText()
                        break
                    }
                    entry = zip.nextEntry
                }
                documentXml?.let { parseDocxXml(it) }
            }
        }
    } catch (e: Exception) {
        null
    }

    private fun parseDocxXml(xml: String): String {
        val parser: XmlPullParser = Xml.newPullParser()
        parser.setInput(xml.reader())

        val sb = StringBuilder()
        var inTextRun = false
        var event = parser.eventType

        while (event != XmlPullParser.END_DOCUMENT) {
            when (event) {
                XmlPullParser.START_TAG -> {
                    when (parser.name) {
                        "t" -> inTextRun = true
                        "p" -> if (sb.isNotEmpty()) sb.append('\n')
                        "tab" -> sb.append('\t')
                        "br", "cr" -> sb.append('\n')
                    }
                }
                XmlPullParser.TEXT -> if (inTextRun) sb.append(parser.text)
                XmlPullParser.END_TAG -> if (parser.name == "t") inTextRun = false
            }
            event = parser.next()
        }
        return truncate(sb.toString().trim())
    }
}
