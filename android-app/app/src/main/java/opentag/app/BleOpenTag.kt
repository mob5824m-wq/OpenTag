package opentag.app

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import java.nio.charset.StandardCharsets
import java.util.ArrayDeque
import java.util.UUID

/**
 * BLE client for the OpenTag device. Speaks the same protocol as the web
 * sync page:
 *
 *   control  0002  "AMI:PUT:<name>:<size>" / "AMI:LIST" / "AMI:DEL:<name>" /
 *                   "AMI:EMU:<name>" / "AMI:ABORT" / "STOP" / "STAT" /
 *                   "TEXT:<note>" / "URL:<note>" / "KEY:STATUS"
 *   status   0003  notifications, one status line each
 *   data     0004  [seq_hi][seq_lo][len][payload] chunks of one transfer
 */
class BleOpenTag(private val ctx: Context, private val listener: Listener) {

    interface Listener {
        fun onLog(line: String)
        fun onState(connected: Boolean)
        fun onDeviceList(names: List<String>)
        fun onUploadDone(name: String, ok: Boolean)
    }

    private enum class Phase { IDLE, WAIT_PUT_OK, STREAM, WAIT_SAVED }

    private val main = Handler(Looper.getMainLooper())
    private val lock = Any()

    private var gatt: BluetoothGatt? = null
    private var ctrlChar: BluetoothGattCharacteristic? = null
    private var dataChar: BluetoothGattCharacteristic? = null
    private var scanning = false

    private val ctrlQueue = ArrayDeque<String>()
    private var ctrlBusy = false

    private val uploadQueue = ArrayDeque<Pair<String, ByteArray>>()
    private var phase = Phase.IDLE
    private var streamName = ""
    private var streamBytes = ByteArray(0)
    private var streamOff = 0
    private var streamSeq = 0
    private var watchdog: Runnable? = null

    private var listing = false
    private val listLines = ArrayList<String>()

    val connected: Boolean get() = gatt != null

    // ---- connection ---------------------------------------------------------

    @SuppressLint("MissingPermission")
    fun connect() {
        val manager = ctx.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
        val adapter = manager?.adapter
        if (adapter == null || !adapter.isEnabled) {
            log("Bluetooth is off"); return
        }
        if (gatt != null) { log("Already connected"); return }
        val scanner = adapter.bluetoothLeScanner
        if (scanner == null) { log("BLE scanner unavailable"); return }
        log("Scanning for OpenTag…")
        scanning = true
        val filters = listOf(ScanFilter.Builder().setServiceUuid(ParcelUuid(SERVICE)).build())
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        scanner.startScan(filters, settings, scanCallback)
        main.postDelayed({
            if (scanning) { stopScan(); log("No OpenTag found — is it powered on?") }
        }, 15000)
    }

    @SuppressLint("MissingPermission")
    private fun stopScan() {
        if (!scanning) return
        scanning = false
        val manager = ctx.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
        try {
            manager?.adapter?.bluetoothLeScanner?.stopScan(scanCallback)
        } catch (e: Exception) { /* adapter off */ }
    }

    private val scanCallback = object : ScanCallback() {
        @SuppressLint("MissingPermission")
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            stopScan()
            log("Found OpenTag, connecting…")
            result.device.connectGatt(ctx, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
        }

        override fun onScanFailed(errorCode: Int) {
            scanning = false
            log("Scan failed: $errorCode")
        }
    }

    private val gattCallback = object : BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                g.requestMtu(247)
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                close()
                log("Disconnected")
                main.post { listener.onState(false) }
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            g.discoverServices()
        }

        @SuppressLint("MissingPermission")
        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) { close(); log("Service discovery failed"); return }
            val svc = g.getService(SERVICE)
            val ctrl = svc?.getCharacteristic(CONTROL)
            val data = svc?.getCharacteristic(DATA)
            val stat = svc?.getCharacteristic(STATUS)
            if (ctrl == null || data == null || stat == null) {
                close(); log("OpenTag characteristics missing"); return
            }
            synchronized(lock) {
                ctrlChar = ctrl
                dataChar = data
            }
            g.setCharacteristicNotification(stat, true)
            val d = stat.getDescriptor(CCCD)
            if (d == null) { close(); log("CCCD missing"); return }
            if (Build.VERSION.SDK_INT >= 33) {
                g.writeDescriptor(d, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
            } else {
                @Suppress("DEPRECATION")
                d.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                @Suppress("DEPRECATION")
                g.writeDescriptor(d)
            }
        }

        override fun onDescriptorWrite(g: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) {
            if (descriptor.uuid == CCCD) {
                if (status == BluetoothGatt.GATT_SUCCESS) {
                    log("Connected")
                    main.post { listener.onState(true) }
                    sendCommand("STAT")
                } else {
                    close(); log("Could not enable notifications")
                }
            }
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) {
            val uuid = characteristic.uuid
            if (uuid == CONTROL) {
                synchronized(lock) {
                    ctrlBusy = false
                    pumpCtrlLocked()
                }
            } else if (uuid == DATA) {
                pumpStream()
            }
        }

        @Deprecated("Deprecated in Java")
        @Suppress("DEPRECATION")
        override fun onCharacteristicChanged(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
            if (Build.VERSION.SDK_INT < 33) {
                val v = characteristic.value ?: return
                handleNotify(String(v, StandardCharsets.UTF_8))
            }
        }

        override fun onCharacteristicChanged(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic, value: ByteArray) {
            if (Build.VERSION.SDK_INT >= 33) {
                handleNotify(String(value, StandardCharsets.UTF_8))
            }
        }
    }

    @SuppressLint("MissingPermission")
    fun disconnect() {
        stopScan()
        val g = gatt ?: return
        try { g.disconnect() } catch (e: Exception) { /* already gone */ }
        close()
        log("Disconnected")
        main.post { listener.onState(false) }
    }

    @SuppressLint("MissingPermission")
    private fun close() {
        synchronized(lock) {
            gatt?.close()
            gatt = null
            ctrlChar = null
            dataChar = null
            ctrlQueue.clear()
            ctrlBusy = false
            uploadQueue.clear()
            phase = Phase.IDLE
            cancelWatchdog()
        }
    }

    // ---- command path -------------------------------------------------------

    fun sendCommand(cmd: String) {
        synchronized(lock) {
            ctrlQueue.add(cmd)
            pumpCtrlLocked()
        }
    }

    @SuppressLint("MissingPermission")
    private fun pumpCtrlLocked() {
        if (ctrlBusy || gatt == null) return
        val cmd = ctrlQueue.peekFirst() ?: return
        val ch = ctrlChar ?: return
        ctrlBusy = true
        val ok: Boolean
        if (Build.VERSION.SDK_INT >= 33) {
            val r = gatt?.writeCharacteristic(
                ch, cmd.toByteArray(StandardCharsets.UTF_8),
                BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT,
            )
            ok = r != null && r >= 0
        } else {
            @Suppress("DEPRECATION")
            ch.setValue(cmd)
            @Suppress("DEPRECATION")
            ch.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            @Suppress("DEPRECATION")
            ok = gatt?.writeCharacteristic(ch) == true
        }
        if (ok) {
            ctrlQueue.pollFirst()  // written; the callback frees the slot
        } else {
            ctrlBusy = false
        }
    }

    // ---- upload path --------------------------------------------------------

    /** Queues a dump upload; [Listener.onUploadDone] reports the result. */
    fun uploadDump(name: String, bytes: ByteArray) {
        synchronized(lock) {
            uploadQueue.add(name to bytes)
            if (phase == Phase.IDLE) startNextUploadLocked()
        }
    }

    private fun startNextUploadLocked() {
        val next = uploadQueue.peekFirst() ?: return
        streamName = next.first
        streamBytes = next.second
        streamOff = 0
        streamSeq = 0
        phase = Phase.WAIT_PUT_OK
        armWatchdog()
        val cmd = "AMI:PUT:$streamName:${streamBytes.size}"
        ctrlQueue.add(cmd)
        pumpCtrlLocked()
    }

    @SuppressLint("MissingPermission")
    private fun pumpStream() = synchronized(lock) {
        val ch = dataChar ?: return finishUpload(false, "data characteristic gone")
        if (streamOff >= streamBytes.size) return  // waiting for OK:ami-saved
        val n = minOf(CHUNK, streamBytes.size - streamOff)
        val frame = ByteArray(3 + n)
        frame[0] = ((streamSeq shr 8) and 0xFF).toByte()
        frame[1] = (streamSeq and 0xFF).toByte()
        frame[2] = n.toByte()
        System.arraycopy(streamBytes, streamOff, frame, 3, n)
        if (Build.VERSION.SDK_INT >= 33) {
            gatt?.writeCharacteristic(ch, frame, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT)
                ?: return finishUpload(false, "disconnected")
        } else {
            @Suppress("DEPRECATION")
            ch.setValue(frame)
            @Suppress("DEPRECATION")
            ch.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            @Suppress("DEPRECATION")
            if (gatt?.writeCharacteristic(ch) != true) return finishUpload(false, "write failed")
        }
        streamOff += n
        streamSeq++
    }

    private fun finishUpload(ok: Boolean, why: String) {
        synchronized(lock) {
            uploadQueue.pollFirst()
            phase = Phase.IDLE
            cancelWatchdog()
        }
        if (!ok) log("Upload of $streamName failed: $why")
        val name = streamName
        main.post { listener.onUploadDone(name, ok) }
        synchronized(lock) { if (phase == Phase.IDLE) startNextUploadLocked() }
    }

    private fun armWatchdog() {
        cancelWatchdog()
        val r = Runnable {
            synchronized(lock) {
                if (phase != Phase.IDLE) finishUpload(false, "timeout")
            }
        }
        watchdog = r
        main.postDelayed(r, 20000)
    }

    private fun cancelWatchdog() {
        watchdog?.let { main.removeCallbacks(it) }
        watchdog = null
    }

    // ---- notification handling ----------------------------------------------

    private fun handleNotify(text: String) {
        log(text)
        if (text.startsWith("ERR")) {
            synchronized(lock) {
                if (phase == Phase.WAIT_PUT_OK || phase == Phase.STREAM || phase == Phase.WAIT_SAVED) {
                    finishUpload(false, text)
                }
            }
            return
        }
        if (listing && text.startsWith("AMI:")) {
            val idx = text.indexOf(':', 4)
            if (idx > 0) listLines.add(text.substring(idx + 1))
            return
        }
        if (text.startsWith("OK:ami-list:")) {
            listing = false
            val names = ArrayList(listLines)
            main.post { listener.onDeviceList(names) }
            return
        }
        synchronized(lock) {
            when {
                phase == Phase.WAIT_PUT_OK && text.startsWith("OK:ami-put") -> {
                    phase = Phase.STREAM
                    pumpStream()
                }
                phase == Phase.WAIT_SAVED && text.startsWith("OK:ami-saved") -> {
                    finishUpload(true, "")
                }
            }
        }
    }

    fun refreshLibrary() {
        synchronized(lock) {
            listing = true
            listLines.clear()
        }
        sendCommand("AMI:LIST")
    }

    private fun log(line: String) {
        main.post { listener.onLog(line) }
    }

    companion object {
        val SERVICE: UUID = UUID.fromString("9b9e0001-9d2a-4b72-9a0c-6a8f6f4d0001")
        val CONTROL: UUID = UUID.fromString("9b9e0002-9d2a-4b72-9a0c-6a8f6f4d0001")
        val STATUS: UUID = UUID.fromString("9b9e0003-9d2a-4b72-9a0c-6a8f6f4d0001")
        val DATA: UUID = UUID.fromString("9b9e0004-9d2a-4b72-9a0c-6a8f6f4d0001")
        val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
        const val CHUNK = 240
    }
}
