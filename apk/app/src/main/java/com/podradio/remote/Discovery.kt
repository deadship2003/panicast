package com.podradio.remote

import android.content.Context
import android.net.wifi.WifiManager
import android.os.Build
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress

/** One discovered PodRadio player on the LAN. */
data class Player(val host: String, val tcpPort: Int, val wsPort: Int)

/**
 * UDP discovery: broadcasts "PANICAST_DISCOVER" to the LAN broadcast address and collects
 * "PANICAST 1 tcp=<p> ws=<p>" responses. Requires a MulticastLock so the WiFi radio doesn't
 * drop the broadcasts while the screen is off.
 */
class Discovery {
    /** Run a scan for `timeoutMs`; returns the players found. Call off the main thread. */
    fun scan(context: Context, timeoutMs: Int = 2500, onFound: ((Player) -> Unit)? = null): List<Player> {
        val wm = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager
        val lock = wm?.createMulticastLock("podradio-discovery")?.apply {
            setReferenceCounted(false); acquire()
        }
        val found = mutableListOf<Player>()
        var sock: DatagramSocket? = null
        try {
            sock = DatagramSocket()
            sock.broadcast = true
            val probe = "PANICAST_DISCOVER".toByteArray()
            // 18430 is the default PodRadio discovery port.
            sock.send(DatagramPacket(probe, probe.size, InetAddress.getByName("255.255.255.255"), 18430))
            val buf = ByteArray(256)
            val end = System.currentTimeMillis() + timeoutMs
            while (System.currentTimeMillis() < end) {
                sock.soTimeout = (end - System.currentTimeMillis()).toInt().coerceAtLeast(1)
                try {
                    val pkt = DatagramPacket(buf, buf.size)
                    sock.receive(pkt)
                    val txt = String(pkt.data, 0, pkt.length)
                    if (txt.startsWith("PANICAST 1")) {
                        val p = parse(txt, pkt.address.hostAddress ?: continue)
                        if (p != null && found.none { it.host == p.host }) {
                            found.add(p); onFound?.invoke(p)
                        }
                    }
                } catch (_: java.net.SocketTimeoutException) {
                    // keep waiting until overall timeout
                }
            }
        } catch (e: Exception) {
            // ignored — scan returns whatever was found
        } finally {
            try { sock?.close() } catch (_: Exception) {}
            try { if (lock?.isHeld == true) lock.release() } catch (_: Exception) {}
        }
        return found
    }

    private fun parse(line: String, host: String): Player? {
        var tcp = 18421; var ws = 18422
        for (tok in line.trim().split(" ")) {
            if (tok.startsWith("tcp=")) tcp = tok.substring(4).toIntOrNull() ?: tcp
            else if (tok.startsWith("ws=")) ws = tok.substring(3).toIntOrNull() ?: ws
        }
        return Player(host, tcp, ws)
    }
}
