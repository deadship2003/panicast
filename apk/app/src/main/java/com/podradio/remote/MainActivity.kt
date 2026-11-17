package com.podradio.remote

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.TextFieldValue
import androidx.compose.ui.unit.dp
import androidx.compose.foundation.text.KeyboardOptions
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent { App() }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun App() {
    var screen by remember { mutableStateOf("scan") }   // scan | pin | control
    var players by remember { mutableStateOf(listOf<Player>()) }
    var scanning by remember { mutableStateOf(false) }
    var selected by remember { mutableStateOf<Player?>(null) }
    val scope = rememberCoroutineScope()
    val discovery = remember { Discovery() }
    val ctx = LocalContext.current

    // PRP state
    val client = remember { PrpClient() }
    var stateMap by remember { mutableStateOf(mapOf<String, String>()) }
    var logs by remember { mutableStateOf(listOf<String>()) }

    Scaffold(topBar = {
        TopAppBar(title = { Text("panicast Remote") },
            navigationIcon = {
                if (screen != "scan") IconButton(onClick = {
                    client.close(); screen = "scan"
                }) { Icon(Icons.AutoMirrored.Filled.ArrowBack, "back") }
            })
    }) { p ->
        Column(Modifier.padding(p).fillMaxSize()) {
            when (screen) {
                "scan" -> ScanScreen(players, scanning, {
                    scope.launch {
                        scanning = true; players = emptyList()
                        val found = withContext(Dispatchers.IO) { discovery.scan(ctx) }
                        players = found; scanning = false
                    }
                }, { selected = it; screen = "pin" })
                "pin" -> PinScreen(selected!!, { pin ->
                    val p = selected!!
                    client.onLog = { l -> logs = (logs + l).takeLast(50) }
                    client.onState = { m -> stateMap = stateMap + m }
                    client.onIdle = { subsys ->
                        // a subsystem changed — re-pull status
                        client.send("status")
                    }
                    if (client.connect(p.host, p.tcpPort)) {
                        client.authenticate(pin) { ok ->
                            scope.launch { if (ok) screen = "control" else logs = logs + "auth failed" }
                        }
                    }
                })
                "control" -> ControlScreen(stateMap, client)
            }
            if (logs.isNotEmpty()) {
                Text(logs.joinToString("\n"), style = MaterialTheme.typography.labelSmall,
                    modifier = Modifier.padding(8.dp))
            }
        }
    }
}

@Composable
fun ScanScreen(players: List<Player>, scanning: Boolean, onScan: () -> Unit, onPick: (Player) -> Unit) {
    Column(Modifier.padding(16.dp)) {
        Button(onClick = onScan, enabled = !scanning) {
            Text(if (scanning) "Scanning…" else "Scan network for players")
        }
        Spacer(Modifier.height(12.dp))
        LazyColumn { items(players) { p ->
            ListItem(headlineContent = { Text("panicast @ ${p.host}") },
                supportingContent = { Text("tcp ${p.tcpPort} · ws ${p.wsPort}") },
                modifier = Modifier.padding(4.dp).fillMaxWidth(),
                trailingContent = { Icon(Icons.Default.ChevronRight, null) },
                leadingContent = { Icon(Icons.Default.Radio, null) })
            HorizontalDivider()
        } }
        if (players.isEmpty() && !scanning) Text("No players found. Ensure panicast is running with [remote] enable=true and bind=0.0.0.0.",
            style = MaterialTheme.typography.bodySmall, modifier = Modifier.padding(top = 8.dp))
    }
}

@Composable
fun PinScreen(player: Player, onSubmit: (String) -> Unit) {
    var pin by remember { mutableStateOf("6696") }
    Column(Modifier.padding(16.dp)) {
        Text("Connect to ${player.host}:${player.tcpPort}", style = MaterialTheme.typography.titleMedium)
        Text("Enter the PIN shown in panicast (:pin), or 6696 for headless pairing.",
            style = MaterialTheme.typography.bodySmall, modifier = Modifier.padding(vertical = 8.dp))
        OutlinedTextField(value = pin, onValueChange = { pin = it.filter { c -> c.isDigit() }.take(8) },
            label = { Text("PIN") }, keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.NumberPassword))
        Spacer(Modifier.height(12.dp))
        Button(onClick = { onSubmit(pin) }) { Text("Connect") }
    }
}

@Composable
fun ControlScreen(state: Map<String, String>, client: PrpClient) {
    val title = state["title"] ?: "Not playing"
    val mode = state["mode"] ?: "--"
    val st = state["state"] ?: "stop"
    val vol = state["volume"] ?: "100"
    val speed = state["speed"] ?: "1.0"
    val pmode = state["play_mode"] ?: "cycle"
    Column(Modifier.padding(12.dp).fillMaxSize()) {
        AssistChip(onClick = {}, label = { Text(mode) })
        Text(title, style = MaterialTheme.typography.titleLarge)
        Spacer(Modifier.height(8.dp))
        Row(verticalAlignment = Alignment.CenterVertically) {
            Btn("⏮") { client.send("previous") }
            Btn(if (st == "play") "⏸" else "▶") { client.send("play_pause") }
            Btn("⏭") { client.send("next") }
            Btn("⏪15") { client.send("seek -15") }
            Btn("15⏩") { client.send("seek 15") }
        }
        Spacer(Modifier.height(8.dp))
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("Vol $vol"); Spacer(Modifier.width(8.dp))
            Slider(value = vol.toFloatOrNull() ?: 100f, onValueChange = {},
                valueRange = 0f..100f, onValueChangeFinished = {
                    client.send("volume ${(vol.toFloatOrNull() ?: 100f).toInt()}") })
        }
        Row {
            Btn("-") { client.send("speed_down") }; Text(speed, Modifier.padding(8.dp)); Btn("+") { client.send("speed_up") }
            Btn("1x") { client.send("speed_reset") }
            Btn("Repeat") { client.send("repeat") }; Btn("Shuffle") { client.send("shuffle") }; Btn("Cycle") { client.send("cycle") }
        }
        Spacer(Modifier.height(8.dp))
        Text("Mode", style = MaterialTheme.typography.labelMedium)
        Row(verticalAlignment = Alignment.CenterVertically) {
            for (m in listOf("RADIO", "PODCAST", "FAVOURITE", "HISTORY", "ONLINE", "ACCOUNT", "BILIBILI", "TIKTOK", "IPTV"))
                Btn(m.first().toString()) { client.send("mode $m") }
        }
        Row {
            Btn("▲") { client.send("nav_up") }; Btn("▼") { client.send("nav_down") }
            Btn("Enter") { client.send("nav_enter") }; Btn("Back") { client.send("nav_back") }
        }
        Spacer(Modifier.height(8.dp))
        Text("play_mode=$pmode  state=$st", style = MaterialTheme.typography.labelSmall)
        Button(onClick = { client.send("status") }, modifier = Modifier.padding(top = 8.dp)) { Text("Refresh status") }
    }
}

@Composable
fun Btn(label: String, onClick: () -> Unit) {
    OutlinedButton(onClick = onClick, modifier = Modifier.padding(2.dp)) { Text(label) }
}
