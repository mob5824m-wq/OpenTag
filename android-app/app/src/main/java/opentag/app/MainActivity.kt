package opentag.app

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.Editable
import android.text.TextWatcher
import android.text.method.ScrollingMovementMethod
import android.view.View
import android.view.ViewGroup
import android.widget.BaseAdapter
import android.widget.Button
import android.widget.EditText
import android.widget.ImageView
import android.widget.ListView
import android.widget.TextView
import android.widget.Toast
import java.io.ByteArrayOutputStream
import java.util.zip.ZipInputStream

class MainActivity : Activity(), BleOpenTag.Listener {

    /** A dump picked from phone storage (loose file or from a .zip). Invalid
     *  dumps stay in the list — their row shows the reject reason instead of
     *  an upload chip. Normalization mirrors the web sync page: 532/540/572-
     *  byte containers (optionally behind a 32-byte N2-style header or with
     *  trailing junk) become clean 540-byte NTAG215 page images; 1024+ byte
     *  dumps carrying a v3 figure ID are cut to the first 256 pages and
     *  flagged V3 · I2C (Kirby Air Riders generation, emulation-only). */
    private class Dump(val name: String, raw: ByteArray) {
        val image: ByteArray?       // normalized page image; null = invalid
        val v3: Boolean
        val reason: String?         // null = valid
        val uidHex: String
        val meta: AmiiboDb.Meta? = db?.matchFilename(name)

        init {
            val norm = normalizeDump(raw)
            image = norm.first
            v3 = norm.second
            reason = norm.third
            uidHex = if (image != null) uidHexOf(image!!) else ""
        }

        val valid: Boolean get() = image != null
    }

    private lateinit var ble: BleOpenTag
    private val local = ArrayList<Dump>()
    private val deviceLib = ArrayList<String>()
    private val keyFiles = ArrayList<Pair<String, ByteArray>>()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        loadDb()
        ble = BleOpenTag(this, this)

        findViewById<Button>(R.id.connect).setOnClickListener {
            if (ensurePermissions()) ble.connect()
        }
        findViewById<Button>(R.id.disconnect).setOnClickListener { ble.disconnect() }
        findViewById<Button>(R.id.sendNote).setOnClickListener { sendNote() }
        findViewById<Button>(R.id.clearNote).setOnClickListener { ble.sendCommand("NOTE:CLEAR") }
        findViewById<Button>(R.id.pick).setOnClickListener { pickFiles() }
        findViewById<Button>(R.id.uploadAll).setOnClickListener { uploadAll() }
        findViewById<Button>(R.id.refresh).setOnClickListener { ble.refreshLibrary() }
        findViewById<Button>(R.id.stopEmu).setOnClickListener { ble.sendCommand("STOP") }

        findViewById<TextView>(R.id.log).movementMethod = ScrollingMovementMethod()
        findViewById<ListView>(R.id.localList).setEmptyView(findViewById(R.id.localEmpty))
        findViewById<ListView>(R.id.deviceList).setEmptyView(findViewById(R.id.deviceEmpty))

        // database browser — same data the dump matching uses, no connection
        findViewById<EditText>(R.id.dbSearch).addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {}
            override fun afterTextChanged(s: Editable?) {
                dbSearchHandler.removeCallbacksAndMessages(null)
                dbSearchHandler.postDelayed({ renderDb() }, 150)
            }
        })
        renderDb()

        // signing keys (advanced) + Wi-Fi firmware updates
        findViewById<Button>(R.id.keyPick).setOnClickListener { pickKeyFiles() }
        findViewById<Button>(R.id.uploadKeys).setOnClickListener { uploadKeyFiles() }
        findViewById<Button>(R.id.keyStatusBtn).setOnClickListener { ble.sendCommand("KEY:STATUS") }
        findViewById<Button>(R.id.forgetKeys).setOnClickListener {
            ble.sendCommand("KEY:CLEAR")
            findViewById<TextView>(R.id.keyState).text = "Key state: no keys on device"
        }
        findViewById<Button>(R.id.wifiSave).setOnClickListener { saveWifi() }
        findViewById<Button>(R.id.otaCheck).setOnClickListener { ble.sendCommand("OTA:STATUS") }
        findViewById<Button>(R.id.otaStart).setOnClickListener { ble.sendCommand("OTA:START") }

        findViewById<ListView>(R.id.localList).apply {
            adapter = localAdapter
            setOnItemClickListener { _, _, position, _ -> uploadOne(local[position]) }
            setOnItemLongClickListener { _, _, position, _ ->
                local.removeAt(position)
                localAdapter.notifyDataSetChanged()
                updateButtons()
                true
            }
        }
        findViewById<ListView>(R.id.deviceList).apply {
            adapter = deviceAdapter
            setOnItemClickListener { _, _, position, _ -> deviceItemAction(position) }
        }
        findViewById<ListView>(R.id.dbList).adapter = dbAdapter
        updateButtons()
    }

    // ---- permissions ---------------------------------------------------------

    private fun ensurePermissions(): Boolean {
        val needed: Array<String> = if (Build.VERSION.SDK_INT >= 31)
            arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        else
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        val missing = needed.filter {
            checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isEmpty()) return true
        requestPermissions(missing.toTypedArray(), REQ_PERMS)
        return false
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode != REQ_PERMS) return
        if (grantResults.isNotEmpty() && grantResults.all { it == PackageManager.PERMISSION_GRANTED }) {
            ble.connect()
        } else {
            toast("Bluetooth permissions are required to connect.")
        }
    }

    // ---- database ------------------------------------------------------------

    private fun loadDb() {
        if (db != null) return
        val text = assets.open("amiibo.json").use { it.readBytes().decodeToString() }
        db = AmiiboDb(text)
    }

    // ---- note ----------------------------------------------------------------

    private fun sendNote() {
        val value = findViewById<EditText>(R.id.note).text.toString().trim()
        if (value.isEmpty()) { toast("Type a note first."); return }
        val isUrl = value.startsWith("http://") || value.startsWith("https://")
        ble.sendCommand((if (isUrl) "URL:" else "TEXT:") + value)
    }

    // ---- file picking --------------------------------------------------------

    private fun pickFiles() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
            putExtra(Intent.EXTRA_MIME_TYPES, arrayOf(
                "application/octet-stream", "application/zip", "application/x-zip-compressed"))
            putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
        }
        startActivityForResult(intent, REQ_FILES)
    }

    private fun pickKeyFiles() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
            putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
        }
        startActivityForResult(intent, REQ_KEYS)
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == REQ_KEYS) { onActivityResultKeys(resultCode, data); return }
        if (requestCode != REQ_FILES || resultCode != RESULT_OK) return
        val uris = ArrayList<Uri>()
        data?.data?.let { uris.add(it) }
        data?.clipData?.let { cd ->
            for (i in 0 until cd.itemCount) uris.add(cd.getItemAt(i).uri)
        }
        Thread {
            var added = 0
            for (uri in uris) added += importUri(uri)
            runOnUiThread {
                localAdapter.notifyDataSetChanged()
                updateButtons()
                toast("Added $added dump(s)")
            }
        }.start()
    }

    /** Reads one picked document; expands .zip archives. Returns count added. */
    private fun importUri(uri: Uri): Int {
        val name = displayName(uri) ?: "unnamed.bin"
        val bytes = try {
            contentResolver.openInputStream(uri)?.use { it.readBytes() } ?: return 0
        } catch (e: Exception) { return 0 }
        if (bytes.size >= 4 && bytes[0] == 'P'.code.toByte() && bytes[1] == 'K'.code.toByte()) {
            return importZip(bytes)
        }
        return if (addDump(name, bytes)) 1 else 0
    }

    private fun importZip(bytes: ByteArray): Int {
        var added = 0
        try {
            ZipInputStream(bytes.inputStream()).use { zin ->
                var entry = zin.nextEntry
                while (entry != null) {
                    val base = entry.name.substringAfterLast('/')
                    if (!entry.isDirectory &&
                        listOf(".bin", ".nfc", ".tag", ".dump")
                            .any { base.endsWith(it, ignoreCase = true) }) {
                        val out = ByteArrayOutputStream()
                        val buf = ByteArray(4096)
                        while (true) {
                            val n = zin.read(buf)
                            if (n < 0) break
                            out.write(buf, 0, n)
                        }
                        if (addDump(base, out.toByteArray())) added++
                    }
                    entry = zin.nextEntry
                }
            }
        } catch (e: Exception) {
            log("Zip read failed: ${e.message}")
        }
        return added
    }

    /** Adds (or replaces) one entry; false when the file is unusable. */
    private fun addDump(rawName: String, bytes: ByteArray): Boolean {
        val name = sanitizeName(rawName) ?: return false
        val raw = if (rawName.substringAfterLast('/').endsWith(".nfc", ignoreCase = true)) {
            parseFlipperNfc(bytes.decodeToString()) ?: run {
                log("$name: skipped (not a Flipper Zero .nfc file)")
                return false
            }
        } else bytes
        local.removeAll { it.name == name }
        local.add(Dump(name, raw))
        return true
    }

    // ---- signing keys (advanced) ---------------------------------------------

    /** Stage the picked key files (validated client-side, same check the
     *  web sync page runs); the Upload keys button sends them. */
    private fun onActivityResultKeys(resultCode: Int, data: Intent?) {
        if (resultCode != RESULT_OK) return
        val uris = ArrayList<Uri>()
        data?.data?.let { uris.add(it) }
        data?.clipData?.let { cd ->
            for (i in 0 until cd.itemCount) uris.add(cd.getItemAt(i).uri)
        }
        Thread {
            val picked = ArrayList<Pair<String, ByteArray>>()
            for (uri in uris) {
                val name = displayName(uri) ?: "key.bin"
                try {
                    val bytes = contentResolver.openInputStream(uri)?.use { it.readBytes() } ?: continue
                    if (keyFileKind(bytes)) picked.add(name to bytes)
                    else log("$name: not a valid 80/160-byte key file (type string check failed)")
                } catch (e: Exception) {
                    log("$name: read failed (${e.message})")
                }
            }
            runOnUiThread {
                keyFiles.clear()
                keyFiles.addAll(picked)
                toast(if (picked.isEmpty()) "No valid key files"
                      else "${picked.size} key file(s) staged — press Upload keys")
            }
        }.start()
    }

    /** Mirror of the device-side check: 80-byte key files carry their type
     *  in a 14-byte NUL-terminated string at offset 16; a 160-byte combined
     *  retail file is two of those back to back. */
    private fun keyFileKind(bytes: ByteArray): Boolean {
        if (bytes.size != 80 && bytes.size != 160) return false
        var off = 0
        while (off < bytes.size) {
            val ts = bytes.copyOfRange(off + 16, off + 30).decodeToString()
            val nul = ts.indexOf('\u0000')
            if (nul < 0) return false
            val kind = ts.substring(0, nul)
            if (kind != "unfixed infos" && kind != "locked secret") return false
            off += 80
        }
        return true
    }

    private fun uploadKeyFiles() {
        if (!ble.connected) { toast("Connect to OpenTag first."); return }
        if (keyFiles.isEmpty()) {
            toast("Choose your key files first — unfixed-info.bin and locked-secret.bin, or one combined 160-byte retail file.")
            return
        }
        for ((_, bytes) in keyFiles) ble.uploadKeys(bytes)
    }

    // ---- Wi-Fi provisioning + OTA --------------------------------------------

    private fun saveWifi() {
        if (!ble.connected) { toast("Connect to OpenTag first."); return }
        val ssid = findViewById<EditText>(R.id.wifiSsid).text.toString().trim()
        val pass = findViewById<EditText>(R.id.wifiPass).text.toString()
        if (ssid.isEmpty()) { toast("Enter a Wi-Fi name first."); return }
        ble.sendCommand("WIFI:$ssid|$pass")
    }

    private fun displayName(uri: Uri): String? = try {
        contentResolver.query(uri, null, null, null, null)?.use { c ->
            if (c.moveToFirst()) {
                val idx = c.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME)
                if (idx >= 0) c.getString(idx) else null
            } else null
        }
    } catch (e: Exception) { null }

    /** Mirrors the device's name rules: [A-Za-z0-9._ -], no leading dot, <= 40. */
    private fun sanitizeName(raw: String): String? {
        val base = raw.substringAfterLast('/')
            .replace(Regex("(?i)\\.(bin|nfc|tag|dump)$"), "")
            .trim()
            .replace(Regex("[^A-Za-z0-9._ -]"), "_")
            .take(40)
        if (base.isEmpty() || base.startsWith(".")) return null
        return base
    }

    // ---- uploads -------------------------------------------------------------

    private fun uploadOne(dump: Dump) {
        if (!ble.connected) { toast("Connect to OpenTag first."); return }
        val img = dump.image
        if (img == null) { toast("${dump.name}: ${dump.reason ?: "not a valid dump"}"); return }
        ble.uploadDump(dump.name, img)
    }

    private fun uploadAll() {
        if (!ble.connected) { toast("Connect to OpenTag first."); return }
        var n = 0
        for (d in local) {
            val img = d.image
            if (img != null) { ble.uploadDump(d.name, img); n++ }
        }
        if (n == 0) toast("Nothing valid to upload.")
    }

    // ---- device library ------------------------------------------------------

    private fun deviceItemAction(position: Int) {
        if (position !in deviceLib.indices) return
        val name = deviceLib[position]
        val meta = db?.matchFilename(name)
        val title = meta?.let { "${it.name} (${it.series})" } ?: name
        val options = arrayOf("Emulate on OpenTag", "Delete from device")
        AlertDialog.Builder(this)
            .setTitle(title)
            .setItems(options) { _, which ->
                when (which) {
                    0 -> ble.sendCommand("AMI:EMU:$name")
                    1 -> ble.sendCommand("AMI:DEL:$name")
                }
            }
            .show()
    }

    // ---- BleOpenTag.Listener ---------------------------------------------------

    override fun onLog(line: String) { log(line) }

    override fun onState(connected: Boolean) {
        updateButtons()
        findViewById<TextView>(R.id.stateText).text =
            if (connected) "Connected" else "Not connected"
        findViewById<View>(R.id.stateDot).setBackgroundResource(
            if (connected) R.drawable.dot_ok else R.drawable.dot)
        if (connected) {
            ble.refreshLibrary()
            ble.sendCommand("KEY:STATUS")
        }
    }

    override fun onDeviceList(names: List<String>) {
        deviceLib.clear()
        deviceLib.addAll(names)
        deviceAdapter.notifyDataSetChanged()
    }

    override fun onUploadDone(name: String, ok: Boolean) {
        if (name == "keys") {
            if (ok) ble.sendCommand("KEY:STATUS")
        } else if (ok) {
            ble.refreshLibrary()
        }
    }

    override fun onStatus(line: String) {
        when {
            line.startsWith("OK:keys:") ->
                findViewById<TextView>(R.id.keyState).text =
                    if (line.contains("ready"))
                        "Key state: ready — both key files loaded, writes enabled"
                    else
                        "Key state: missing or partial — upload both unfixed-info.bin and locked-secret.bin"
            line.startsWith("OK:keys-cleared") ->
                findViewById<TextView>(R.id.keyState).text = "Key state: no keys on device"
        }
    }

    // ---- UI helpers ------------------------------------------------------------

    private fun log(line: String) {
        // import runs on a background thread; BleOpenTag posts on main —
        // runOnUiThread covers both (immediate when already on main)
        runOnUiThread {
            val v = findViewById<TextView>(R.id.log)
            val text = v.text.toString()
            v.text = (text.split('\n').takeLast(200) + line).joinToString("\n")
        }
    }

    private fun toast(msg: String) = runOnUiThread { Toast.makeText(this, msg, Toast.LENGTH_SHORT).show() }

    private fun updateButtons() {
        val on = ble.connected
        for (id in intArrayOf(R.id.disconnect, R.id.sendNote, R.id.clearNote,
                               R.id.uploadAll, R.id.refresh, R.id.stopEmu,
                               R.id.uploadKeys, R.id.keyStatusBtn, R.id.forgetKeys,
                               R.id.wifiSave, R.id.otaCheck, R.id.otaStart))
            findViewById<Button>(id).isEnabled = on
        findViewById<Button>(R.id.connect).isEnabled = !on
    }

    /** Binds the outline chip on a row (NTAG215 / V3 · I2C / INVALID / BIN). */
    private fun bindTag(v: View, text: String, bg: Int, color: Int) {
        val tag = v.findViewById<TextView>(R.id.tag)
        tag.text = text
        tag.setBackgroundResource(bg)
        tag.setTextColor(getColor(color))
        tag.visibility = View.VISIBLE
    }

    private fun metaLine(m: AmiiboDb.Meta): String =
        listOf(m.name, m.series, m.type, m.year).filter { it.isNotBlank() }.joinToString(" · ")

    private inner class LocalAdapter : BaseAdapter() {
        override fun getCount() = local.size
        override fun getItem(position: Int) = local[position]
        override fun getItemId(position: Int) = position.toLong()
        override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
            val v = convertView ?: layoutInflater.inflate(R.layout.item_amiibo, parent, false)
            val d = local[position]
            v.findViewById<ImageView>(R.id.icon).let { db?.loadInto(it, d.meta) }
            v.findViewById<TextView>(R.id.title).text = d.name
            v.findViewById<TextView>(R.id.meta).text = d.meta?.let { metaLine(it) } ?: "no metadata match"
            val img = d.image
            v.findViewById<TextView>(R.id.sub).text =
                if (img != null) "${img.size} B · UID ${d.uidHex}"
                else (d.reason ?: "rejected")
            if (d.valid)
                bindTag(v, if (d.v3) "V3 · I2C" else "NTAG215",
                        R.drawable.tag_mint, R.color.mint)
            else
                bindTag(v, "INVALID", R.drawable.tag_bad, R.color.bad)
            return v
        }
    }

    private inner class DeviceAdapter : BaseAdapter() {
        override fun getCount() = deviceLib.size
        override fun getItem(position: Int) = deviceLib[position]
        override fun getItemId(position: Int) = position.toLong()
        override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
            val v = convertView ?: layoutInflater.inflate(R.layout.item_amiibo, parent, false)
            val name = deviceLib[position]
            val meta = db?.matchFilename(name)
            v.findViewById<ImageView>(R.id.icon).let { db?.loadInto(it, meta) }
            v.findViewById<TextView>(R.id.title).text = name
            v.findViewById<TextView>(R.id.meta).text = meta?.let { metaLine(it) } ?: "no metadata match"
            v.findViewById<TextView>(R.id.sub).visibility = View.GONE
            bindTag(v, "BIN", R.drawable.tag_violet, R.color.violet)
            return v
        }
    }

    private inner class DbAdapter : BaseAdapter() {
        private var rows: List<AmiiboDb.Meta> = emptyList()
        fun setRows(list: List<AmiiboDb.Meta>) { rows = list; notifyDataSetChanged() }
        override fun getCount() = rows.size
        override fun getItem(position: Int) = rows[position]
        override fun getItemId(position: Int) = position.toLong()
        override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
            val v = convertView ?: layoutInflater.inflate(R.layout.item_amiibo, parent, false)
            val m = rows[position]
            v.findViewById<ImageView>(R.id.icon).let { db?.loadInto(it, m) }
            v.findViewById<TextView>(R.id.title).text = m.name
            v.findViewById<TextView>(R.id.meta).text =
                listOf(m.series, m.type, m.year).filter { it.isNotBlank() }.joinToString(" · ")
            v.findViewById<TextView>(R.id.sub).visibility = View.GONE
            v.findViewById<TextView>(R.id.tag).visibility = View.GONE
            return v
        }
    }

    private val localAdapter = LocalAdapter()
    private val deviceAdapter = DeviceAdapter()
    private val dbAdapter = DbAdapter()

    // ---- database browser ------------------------------------------------------

    private val dbSearchHandler = Handler(Looper.getMainLooper())

    private fun renderDb() {
        val d = db ?: return
        val rows = d.search(findViewById<EditText>(R.id.dbSearch).text.toString(), DB_LIMIT)
        dbAdapter.setRows(rows)
        findViewById<TextView>(R.id.dbCount).text =
            if (rows.size >= DB_LIMIT)
                "showing the first $DB_LIMIT matches of ${d.size} amiibo — keep typing to narrow it down"
            else
                "${rows.size} of ${d.size} amiibo"
    }

    companion object {
        private const val REQ_PERMS = 1
        private const val REQ_FILES = 2
        private const val REQ_KEYS = 3
        private const val DB_LIMIT = 120   // rows kept in the list at once
        private var db: AmiiboDb? = null

        // ---- dump intake (ported from the web sync page's JS) --------------

        /** Structural check of a candidate NTAG215 page image; null = OK. */
        private fun structuralCheck(b: ByteArray): String? {
            if (b.size < 532) return "too short"
            var blankUid = true
            for (i in 0..7) if (b[i] != 0.toByte()) { blankUid = false; break }
            if (blankUid) return "blank UID (data-only export)"
            if (((0x88 xor b[0].toInt() xor b[1].toInt() xor b[2].toInt()) and 0xFF)
                != (b[3].toInt() and 0xFF)) return "UID checksum mismatch"
            if (b[0] != 0x04.toByte()) return "not an NXP tag UID"
            if (b[12] != 0xE1.toByte() || b[13] != 0x10.toByte() || b[14] != 0x3E.toByte())
                return "not an NTAG215 tag"
            return null
        }

        /** v3 (Kirby Air Riders generation) check: NXP UID byte plus the
         *  figure-ID format byte at 0x5B; the v2 UID checksum is not required. */
        private fun v3Check(b: ByteArray): String? {
            if (b.size < 1024) return "too short for v3"
            if (b[0] != 0x04.toByte()) return "not an NXP tag UID"
            if (b[0x5B] != 0x03.toByte()) return "not a v3 figure ID"
            return null
        }

        /** (image, v3, reason). 532/540/572-byte containers normalize to a
         *  clean 540-byte page image (trying offset 0 and a 32-byte N2-style
         *  header); 1024+ byte v3 dumps are cut to the first 256 pages. */
        private fun normalizeDump(bytes: ByteArray): Triple<ByteArray?, Boolean, String?> {
            if (bytes.size >= 1024 && v3Check(bytes) == null)
                return Triple(bytes.copyOf(1024), true, null)
            if (bytes.size < 532) return Triple(null, false, "too short")
            val candidates = if (bytes.size >= 564) intArrayOf(0, 32) else intArrayOf(0)
            for (off in candidates) {
                val end = minOf(bytes.size, off + 540)
                if (end - off < 532) continue
                val slice = bytes.copyOfRange(off, end)
                if (structuralCheck(slice) != null) continue
                val image = ByteArray(540)
                System.arraycopy(slice, 0, image, 0, slice.size)
                return Triple(image, false, null)
            }
            val head = bytes.copyOfRange(0, minOf(bytes.size, 540))
            return Triple(null, false, structuralCheck(head) ?: "unrecognized layout")
        }

        /** Flipper Zero .nfc text ("Page N: aa bb cc dd" lines); v3 exports
         *  carry pages past 134 and build a 1024-byte image. */
        private fun parseFlipperNfc(text: String): ByteArray? {
            if (!Regex("Page\\s+\\d+\\s*:", RegexOption.IGNORE_CASE).containsMatchIn(text)) return null
            val pages = arrayOfNulls<ByteArray>(256)
            var maxPage = 0
            val re = Regex("^Page\\s+(\\d+)\\s*:\\s*([0-9a-fA-F ]*)$",
                           setOf(RegexOption.MULTILINE, RegexOption.IGNORE_CASE))
            for (m in re.findAll(text)) {
                val n = m.groupValues[1].toIntOrNull() ?: return null
                if (n > 255) return null
                val parts = m.groupValues[2].trim().split(Regex("\\s+")).filter { it.isNotEmpty() }
                if (parts.isEmpty()) continue
                val byteVals = parts.map { it.toIntOrNull(16) }
                if (byteVals.size != 4 || byteVals.any { it == null }) return null
                val pb = ByteArray(4)
                for (i in 0..3) pb[i] = byteVals[i]!!.toByte()
                pages[n] = pb
                if (n > maxPage) maxPage = n
            }
            if (pages[0] == null) return null
            val v3 = maxPage > 134
            val image = ByteArray(if (v3) 1024 else 540)
            for (n in 0..maxPage) pages[n]?.let { System.arraycopy(it, 0, image, n * 4, 4) }
            return image
        }

        /** UID with the BCC byte elided: 04:D4:… (matches the web page). */
        private fun uidHexOf(b: ByteArray): String {
            val uid = b.sliceArray(0..2) + b.sliceArray(4..7)
            return uid.joinToString(":") { String.format("%02X", it.toInt() and 0xFF) }
        }
    }
}
