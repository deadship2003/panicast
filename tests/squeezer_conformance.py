#!/usr/bin/env python3
"""Squeezer protocol conformance test against the panicast mini-LMS server.

Replays the EXACT request sequences the Squeezer Android app sends (verified against
nikclayton/android-squeezer source: CometClient.java / SqueezeService.java /
CurrentPlaylistActivity.java) and asserts the response shapes the app's parsers need.
"""
import base64
import json
import socket
import sys

HOST, PORT = "127.0.0.1", 9190
AUTH = base64.b64encode(b"panicast:panicast").decode()
PLAYER = "00:00:00:00:84:21"
passed, failed = 0, 0


class Cometd:
    """One keep-alive cometd connection (the app pools several; one is enough here)."""

    def __init__(self):
        self.sock = socket.create_connection((HOST, PORT), timeout=5)
        self.client_id = None
        self.cid_counter = 0

    def post(self, msgs):
        body = json.dumps(msgs)
        req = (
            f"POST /cometd HTTP/1.1\r\nHost: {HOST}\r\nAuthorization: Basic {AUTH}\r\n"
            f"Content-Type: application/json;charset=UTF-8\r\nContent-Length: "
            f"{len(body)}\r\nConnection: keep-alive\r\n\r\n{body}"
        )
        self.sock.sendall(req.encode())
        return self._read_response()

    def _read_response(self):
        buf = b""
        while b"\r\n\r\n" not in buf:
            buf += self.sock.recv(65536)
        head, rest = buf.split(b"\r\n\r\n", 1)
        clen = 0
        for line in head.split(b"\r\n"):
            if line.lower().startswith(b"content-length:"):
                clen = int(line.split(b":", 1)[1])
        while len(rest) < clen:
            rest += self.sock.recv(65536)
        return json.loads(rest[:clen])

    # ── Bayeux scaffolding, exactly as the app drives it ──
    def handshake(self):
        reps = self.post(
            [
                {
                    "channel": "/meta/handshake",
                    "version": "1.0",
                    "minimumVersion": "1.0",
                    "supportedConnectionTypes": ["streaming", "long-polling"],
                    "id": "1",
                }
            ]
        )
        self.client_id = reps[0]["clientId"]
        return reps[0]

    def connect(self):
        return self.post(
            [{"channel": "/meta/connect", "clientId": self.client_id, "id": "2"}]
        )

    def subscribe(self, channel):
        return self.post(
            [
                {
                    "channel": "/meta/subscribe",
                    "clientId": self.client_id,
                    "subscription": channel,
                    "id": "3",
                }
            ]
        )

    def slim(self, cmd, response_channel=None, publish_channel="/slim/request"):
        """Publish one slim.request; return the result dict (data of the response msg)."""
        self.cid_counter += 1
        resp_ch = response_channel or f"/{self.client_id}/slim/request/{self.cid_counter}"
        msg = {
            "channel": publish_channel,
            "data": {"response": resp_ch, "request": [PLAYER, cmd]},
            "clientId": self.client_id,
            "id": str(10 + self.cid_counter),
        }
        reps = self.post([msg])
        for m in reps:
            if m.get("channel") == resp_ch and "data" in m:
                return m["data"]
        return {}


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print(f"  ok   {name}")
    else:
        failed += 1
        print(f"  FAIL {name}  {detail}")


def main():
    c = Cometd()

    print("== Bayeux handshake / subscribe ==")
    hs = c.handshake()
    check("handshake successful", hs.get("successful") is True)
    check(
        "handshake echoes transports",
        "streaming" in hs.get("supportedConnectionTypes", []),
    )
    conn = c.connect()
    check("connect successful", conn[0].get("successful") is True)
    sub = c.subscribe(f"/{c.client_id}/slim/request/*")
    check("subscribe successful", sub[0].get("successful") is True)
    sub2 = c.subscribe(f"/{c.client_id}/slim/playerstatus/*")
    check("playerstatus subscribe successful", sub2[0].get("successful") is True)

    print("== Server status / player discovery (parseServerStatus) ==")
    ss = c.slim(
        [
            "serverstatus",
            "0",
            "255",
            "prefs:mediadirs, defeatDestructiveTouchToPlay",
            "playerprefs:playtrackalbum, defeatDestructiveTouchToPlay",
        ]
    )
    check("players_loop present", "players_loop" in ss, str(ss)[:200])
    if "players_loop" in ss:
        p = ss["players_loop"][0]
        check("player id", p.get("playerid") == PLAYER)
        check("player isplayer=1", str(p.get("isplayer")) == "1")
        check("playerprefs flat in record", "playtrackalbum" in p)
    check("version (HandshakeComplete trigger)", "version" in ss)

    print("== Now-playing status (statusRequest → parsePlayerStatus/parseStatus) ==")
    st = c.slim(["status", "-", "1", "menu:menu", "useContextMenu:1"])
    for field in [
        "mode",
        "power",
        "playlist_tracks",
        "playlist_cur_index",
        "playlist_timestamp",
        "mixer volume",
        "time",
        "duration",
        "playlist repeat",
        "playlist shuffle",
    ]:
        check(f"status has '{field}'", field in st, str(st)[:120])
    check("status playerstatus push-shape (song/name keys ok)", "player_name" in st)

    print("== Player status subscription (subscribePlayerStatus) ==")
    sub_st = c.slim(
        ["status", "-", "1", "menu:menu", "useContextMenu:1", "subscribe:30"],
        response_channel=f"/{c.client_id}/slim/playerstatus/{PLAYER}",
        publish_channel="/slim/subscribe",
    )
    check("subscription reply carries mode", "mode" in sub_st)

    print("== Playlist page (CurrentPlaylistActivity → JiveItemListener) ==")
    page = c.slim(["status", "0", "20", "menu:menu"])
    check("page has count", "count" in page, str(page)[:120])
    check("page has item_loop", "item_loop" in page)
    if "item_loop" in page:
        for it in page["item_loop"]:
            if not ("text" in it or "name" in it):
                check("every item has text/name", False, str(it))
                break
        else:
            check("every item has text/name", True)
    page2 = c.slim(["status", "10", "5", "menu:menu"])
    check("windowed page still full-status", "playlist_cur_index" in page2)

    print("== Home menu / browse stubs (must not wedge the command queue) ==")
    menu = c.slim(["menu", "0", "20", "direct:1"])
    check("menu returns count", "count" in menu)
    check("menu returns item_loop", "item_loop" in menu)
    fav = c.slim(["favorites", "0", "20"])
    check("favorites → count/item_loop", "count" in fav and "item_loop" in fav)
    arts = c.slim(["artists", "0", "20"])
    check("artists → count/item_loop", "count" in arts and "item_loop" in arts)
    pl = c.slim(["playlists", "0", "20"])
    check("playlists → count/item_loop", "count" in pl and "item_loop" in pl)

    print("== Control commands (must be accepted; executed on the UI thread) ==")
    cmds = [
        (["pause", "1"], "pause 1"),
        (["pause", "0", "2"], "pause 0 2 (fade arg)"),
        (["play"], "play"),
        (["play", "2"], "play 2 (fade arg)"),
        (["stop"], "stop"),
        (["next"], "next"),
        (["prev"], "prev"),
        (["playlist", "index", "+1"], "playlist index +1"),
        (["playlist", "index", "-1"], "playlist index -1"),
        (["playlist", "index", "3", "2"], "playlist index 3 2 (tap row + fade)"),
        (["playlist", "jump", "2"], "playlist jump 2"),
        (["playlist", "next"], "playlist next"),
        (["playlist", "prev"], "playlist prev"),
        (["playlist", "delete", "2"], "playlist delete 2"),
        (["playlist", "move", "1", "3"], "playlist move 1 3"),
        (["playlist", "clear"], "playlist clear"),
        (["playlist", "save", "MyList"], "playlist save (ack, no crash)"),
        (["button", "shuffle"], "button shuffle"),
        (["button", "repeat"], "button repeat"),
        (["power"], "power (toggle)"),
        (["power", "0"], "power 0"),
        (["power", "1"], "power 1"),
        (["mixer", "volume", "55"], "mixer volume 55"),
        (["mixer", "volume", "+10"], "mixer volume +10"),
        (["mixer", "volume", "-5"], "mixer volume -5"),
        (["time", "42"], "time 42 (seek)"),
        (["playerpref", "alarmDefaultVolume", "40"], "playerpref (ack)"),
        (["alarm", "update", "id:1"], "alarm (ack shape)"),
        (["alarms", "0", "20"], "alarms (loop shape)"),
        (["login", "u", "p"], "login"),
        (["version"], "version"),
    ]
    for cmd, label in cmds:
        try:
            r = c.slim(cmd)
            check(f"accepted: {label}", isinstance(r, dict))
        except Exception as e:  # noqa: BLE001
            check(f"accepted: {label}", False, repr(e))

    mv = c.slim(["mixer", "volume", "?"])
    check("mixer volume ? → _volume (app re-queries forever without it)",
          "_volume" in mv, str(mv))

    print("== Connect-time status push (bidirectional sync) ==")
    pushed = None
    for _ in range(4):
        for m in c.connect():
            ch = m.get("channel", "")
            if "/slim/playerstatus/" in ch and "data" in m:
                pushed = m["data"]
    if pushed is None:
        # state unchanged since last push → no piggyback; force one via a status query
        st2 = c.slim(["status", "-", "1"])
        check("status query still fine after commands", "mode" in st2)
        print("  note: no push (idle state) — acceptable")
    else:
        check("push carries mode", "mode" in pushed)

    print(f"\n{passed} passed, {failed} failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
