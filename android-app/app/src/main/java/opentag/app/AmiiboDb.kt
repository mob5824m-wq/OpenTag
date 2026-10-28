package opentag.app

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.os.Handler
import android.os.Looper
import android.widget.ImageView
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.ConcurrentHashMap

/**
 * Amiibo metadata database, loaded from assets/amiibo.json (932 entries
 * generated from the community AmiiboAPI database). Key = 16-hex-character
 * amiibo ID; value = [name, amiiboSeries, type, gameSeries, releaseYear].
 *
 * The amiibo ID inside a retail dump is AES-encrypted, so bins are matched by
 * their FILE NAME: first a 16-hex ID (the usual dump naming convention),
 * then a fuzzy match on the amiibo name. Box-art images are fetched from
 * AmiiboAPI's image CDN at runtime.
 */
class AmiiboDb(assetsJson: String) {

    data class Meta(
        val id: String, val name: String, val series: String,
        val type: String, val game: String, val year: String, val imageUrl: String,
    )

    private val entries = HashMap<String, Meta>()                 // id -> meta
    private val byName = HashMap<String, MutableList<Meta>>()      // norm(name) -> metas
    private val cache = ConcurrentHashMap<String, Bitmap>()
    private val main = Handler(Looper.getMainLooper())

    val size: Int get() = entries.size

    init {
        val root = JSONObject(assetsJson)
        val keys = root.keys()
        while (keys.hasNext()) {
            val id = keys.next()
            val v = root.optJSONArray(id) ?: continue
            val meta = Meta(
                id = id,
                name = v.optString(0, ""),
                series = v.optString(1, ""),
                type = v.optString(2, ""),
                game = v.optString(3, ""),
                year = v.optString(4, ""),
                imageUrl = IMAGE_BASE + "icon_" + id.substring(0, 8) + "-" + id.substring(8) + ".png",
            )
            entries[id.lowercase()] = meta
            byName.getOrPut(norm(meta.name)) { ArrayList() }.add(meta)
        }
    }

    fun lookup(id: String): Meta? = entries[id.lowercase()]

    fun matchFilename(name: String): Meta? {
        val hex = Regex("[0-9a-fA-F]{16}").find(name)?.value
        if (hex != null) lookup(hex)?.let { return it }
        val hay = norm(name)
        if (hay.isEmpty()) return null
        var best: Meta? = null
        for (m in entries.values) {
            val n = norm(m.name)
            if (n.length >= 3 && hay.contains(n)) {
                if (best == null || n.length > norm(best.name).length) best = m
            }
        }
        if (best != null) {
            val same = byName[norm(best.name)]
            if (same != null && same.size > 1) {
                for (m in same) {
                    val s = norm(m.series)
                    if (s.isNotEmpty() && hay.contains(s)) { best = m; break }
                }
            }
        }
        return best
    }

    /** Loads the box art into [view]; falls back to a colored initial. */
    fun loadInto(view: ImageView, meta: Meta?) {
        view.setImageDrawable(null)
        if (meta == null) {
            view.setImageBitmap(placeholder("?", "0000000000000000"))
            return
        }
        cache[meta.imageUrl]?.let { view.setImageBitmap(it); return }
        view.tag = meta.imageUrl
        view.setImageBitmap(placeholder(meta.name, meta.id))
        Thread {
            val bmp = download(meta.imageUrl)
            if (bmp != null) cache[meta.imageUrl] = bmp
            main.post {
                if (view.tag == meta.imageUrl && bmp != null) view.setImageBitmap(bmp)
            }
        }.start()
    }

    private fun download(url: String): Bitmap? = try {
        val conn = URL(url).openConnection() as HttpURLConnection
        conn.connectTimeout = 8000
        conn.readTimeout = 8000
        conn.instanceFollowRedirects = true
        try {
            conn.inputStream.use { BitmapFactory.decodeStream(it) }
        } finally {
            conn.disconnect()
        }
    } catch (e: Exception) {
        null
    }

    private fun placeholder(label: String, id: String): Bitmap {
        val b = Bitmap.createBitmap(96, 96, Bitmap.Config.ARGB_8888)
        val c = Canvas(b)
        var h = 0
        for (ch in id) h = h * 31 + Character.getNumericValue(ch)
        c.drawColor(Color.HSVToColor(floatArrayOf((Math.abs(h) % 360).toFloat(), 0.45f, 0.30f)))
        val p = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.WHITE
            textSize = 46f
            textAlign = Paint.Align.CENTER
        }
        c.drawText(label.take(1).uppercase(), 48f, 63f, p)
        return b
    }

    private fun norm(s: String): String =
        s.lowercase().replace(Regex("[^a-z0-9]+"), "")

    companion object {
        const val IMAGE_BASE =
            "https://raw.githubusercontent.com/N3evin/AmiiboAPI/master/images/"
    }
}
