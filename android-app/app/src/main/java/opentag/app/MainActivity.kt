package opentag.app

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
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

    /** A .bin dump picked from phone storage (or extracted from a .zip). */
    private class Dump(val name: String, val bytes: ByteArray) {
        val valid: Boolean = bytes.size in 532..572
        val meta: AmiiboDb.Meta? = db?.matchFilename(name)

        fun metaLine(): String {
            val size = "${bytes.size} B"
            val m = meta ?: return "$size · no metadata match"
            return listOf(m.name, m.series, m.type, m.year).filter { it.isNotBlank() }
                .joinToString(" · ") + " · $size"
        }
    }

    private lateinit var ble: BleOpenTag
    private val local = ArrayList<Dump>()
    private val deviceLib = ArrayList<String>()

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
        findViewById<Button>(R.id.pick).setOnClickListener { pickFiles() }
        findViewById<Button>(R.id.uploadAll).setOnClickListener { uploadAll() }
        findViewById<Button>(R.id.refresh).setOnClickListener { ble.refreshLibrary() }
        findViewById<Button>(R.id.stopEmu).setOnClickListener { ble.sendCommand("STOP") }

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

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
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
        addDump(name, bytes)
        return 1
    }

    private fun importZip(bytes: ByteArray): Int {
        var added = 0
        try {
            ZipInputStream(bytes.inputStream()).use { zin ->
                var entry = zin.nextEntry
                while (entry != null) {
                    if (!entry.isDirectory && entry.name.endsWith(".bin", ignoreCase = true)) {
                        val out = ByteArrayOutputStream()
                        val buf = ByteArray(4096)
                        while (true) {
                            val n = zin.read(buf)
                            if (n < 0) break
                            out.write(buf, 0, n)
                        }
                        val base = entry.name.substringAfterLast('/')
                        addDump(base, out.toByteArray())
                        added++
                    }
                    entry = zin.nextEntry
                }
            }
        } catch (e: Exception) {
            log("Zip read failed: ${e.message}")
        }
        return added
    }

    private fun addDump(rawName: String, bytes: ByteArray) {
        val name = sanitizeName(rawName) ?: return
        if (bytes.size !in 532..572) {
            log("$name: skipped (${bytes.size} B — not an NTAG215 dump container)")
            return
        }
        local.removeAll { it.name == name }
        local.add(Dump(name, bytes))
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
        if (!dump.valid) { toast("${dump.name} is not a valid dump size."); return }
        ble.uploadDump(dump.name, dump.bytes)
    }

    private fun uploadAll() {
        if (!ble.connected) { toast("Connect to OpenTag first."); return }
        var n = 0
        for (d in local) {
            if (d.valid) { ble.uploadDump(d.name, d.bytes); n++ }
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
        if (ok) ble.refreshLibrary()
    }

    // ---- UI helpers ------------------------------------------------------------

    private fun log(line: String) {
        val v = findViewById<TextView>(R.id.log)
        val text = v.text.toString()
        v.text = (text.split('\n').takeLast(200) + line).joinToString("\n")
    }

    private fun toast(msg: String) = runOnUiThread { Toast.makeText(this, msg, Toast.LENGTH_SHORT).show() }

    private fun updateButtons() {
        val on = ble.connected
        for (id in intArrayOf(R.id.disconnect, R.id.sendNote, R.id.uploadAll, R.id.refresh, R.id.stopEmu))
            findViewById<Button>(id).isEnabled = on
        findViewById<Button>(R.id.connect).isEnabled = !on
    }

    private inner class LocalAdapter : BaseAdapter() {
        override fun getCount() = local.size
        override fun getItem(position: Int) = local[position]
        override fun getItemId(position: Int) = position.toLong()
        override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
            val v = convertView ?: layoutInflater.inflate(R.layout.item_amiibo, parent, false)
            val d = local[position]
            v.findViewById<ImageView>(R.id.icon).let { db?.loadInto(it, d.meta) }
            v.findViewById<TextView>(R.id.title).text =
                (if (d.valid) "" else "\u26A0 ") + d.name
            v.findViewById<TextView>(R.id.meta).text = d.metaLine()
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
            v.findViewById<TextView>(R.id.meta).text = meta?.let {
                listOf(it.name, it.series, it.type, it.year).filter { x -> x.isNotBlank() }.joinToString(" · ")
            } ?: "no metadata match"
            return v
        }
    }

    private val localAdapter = LocalAdapter()
    private val deviceAdapter = DeviceAdapter()

    companion object {
        private const val REQ_PERMS = 1
        private const val REQ_FILES = 2
        private var db: AmiiboDb? = null
    }
}
