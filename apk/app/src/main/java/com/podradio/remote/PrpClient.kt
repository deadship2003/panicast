package com.podradio.remote

import java.io.BufferedReader
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.net.InetSocketAddress
import java.net.Socket
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

/**
 * PRP (panicast Protocol) TCP client — MPD-style line protocol.
 * Connects to panicast :18421, sends commands, parses `key: value` responses, and subscribes
 * to `idle` for live state push. All callbacks fire on a background thread; marshal to the UI
 * thread in the callback.
 */
class PrpClient {
    var onLog: ((String) -> Unit)? = null
    var onState: ((Map<String, String>) -> Unit)? = null
    var onIdle: ((String) -> Unit)? = null
    var onClosed: (() -> Unit)? = null

    private var sock: Socket? = null
    private var writer: OutputStreamWriter? = null
    private val exec = Executors.newSingleThreadExecutor()
    private val running = AtomicBoolean(false)
    @Volatile var authed = false

    /** Connect (off main thread). Returns true on success. */
    fun connect(host: String, port: Int, timeoutMs: Int = 3000): Boolean {
        val s = Socket()
        try {
            s.connect(InetSocketAddress(host, port), timeoutMs)
        } catch (e: Exception) {
            onLog?.invoke("connect failed: ${e.message}")
            return false
        }
        sock = s
        writer = OutputStreamWriter(s.getOutputStream(), Charsets.UTF_8)
        running.set(true)
        exec.execute { readLoop() }
        return true
    }

    fun send(line: String) {
        try {
            val w = writer ?: return
            w.write(line + "\n"); w.flush()
        } catch (e: Exception) {
            onLog?.invoke("send failed: ${e.message}")
        }
    }

    /** Authenticate with a PIN (dynamic or 6696). */
    fun authenticate(pin: String, cb: ((Boolean) -> Unit)? = null) {
        // One-shot: send password, read the OK/ACK response synchronously via a temp handler.
        val prev = onState
        val done = java.util.concurrent.CountDownLatch(1)
        var ok = false
        onState = { m ->
            // not used for password; password response handled in readLoop via onAuthResult
        }
        // We use a dedicated callback for the next single response.
        authResultCb = { success -> ok = success; done.countDown() }
        send("password $pin")
        exec.execute {
            try { done.await(3, java.util.concurrent.TimeUnit.SECONDS) } catch (_: Exception) {}
            authed = ok
            cb?.invoke(ok)
            if (ok) {
                send("idle player mixer options mode")
                send("status")
            }
        }
    }
    private var authResultCb: ((Boolean) -> Unit)? = null

    fun close() {
        running.set(false)
        try { send("close") } catch (_: Exception) {}
        try { sock?.close() } catch (_: Exception) {}
        onClosed?.invoke()
    }

    private fun readLoop() {
        val reader: BufferedReader
        try {
            reader = BufferedReader(InputStreamReader(sock?.getInputStream(), Charsets.UTF_8))
            val greeting = reader.readLine() ?: return  // "OK Podradio_V0.1-Nxx"
            onLog?.invoke("greeting: $greeting")
        } catch (e: Exception) {
            onLog?.invoke("read failed: ${e.message}"); running.set(false); onClosed?.invoke(); return
        }
        val buf = StringBuilder()
        try {
            while (running.get()) {
                val line = reader.readLine() ?: break
                if (line == "OK" || line.startsWith("ACK")) {
                    // end of a response block
                    if (authResultCb != null) {
                        val ok = line == "OK"
                        val cb = authResultCb; authResultCb = null
                        cb?.invoke(ok)
                        continue
                    }
                    continue
                }
                if (line.startsWith("changed: ")) {
                    onIdle?.invoke(line.substring(9).trim())
                    continue
                }
                val i = line.indexOf(':')
                if (i > 0) {
                    val k = line.substring(0, i).trim()
                    val v = line.substring(i + 1).trim()
                    onState?.invoke(mapOf(k to v))
                }
            }
        } catch (e: Exception) {
            onLog?.invoke("loop ended: ${e.message}")
        }
        running.set(false)
        onClosed?.invoke()
    }
}
