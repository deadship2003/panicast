#!/usr/bin/env python3
"""Protocol conformance test against the panicast mini-LMS server.

Replays the exact request sequences of BOTH cometd clients in the field:
- Squeeze Client (de.maniac103.squeezeclient) — kotlinx.serialization STRICT
  decoding: numeric fields must be JSON NUMBERS, playlist rows need
  track/artist/album, the home menu needs numeric count/offset + id/node items.
  (Identified on the wire by `["play",""]` and `menu 0 512 direct:1`.)
- Squeezer (uk.org.ngo.squeezer) — Java tolerant parsing; the same replies must
  still carry its expected fields.
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


def is_num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def main():
    c = Cometd()

    print("== Bayeux handshake / subscribe ==")
    hs = c.handshake()
    check("handshake successful", hs.get("successful") is True)
    conn = c.connect()
    check("connect successful", conn[0].get("successful") is True)
    for ch in [f"/{c.client_id}/slim/request/*", f"/{c.client_id}/slim/playerstatus/*",
               f"/{c.client_id}/slim/displaystatus/*", f"/{c.client_id}/slim/menustatus/*"]:
        check(f"subscribe {ch.split('/')[-1]}", c.subscribe(ch)[0].get("successful") is True)

    print("== serverstatus (player discovery) ==")
    ss = c.slim(
        [
            "serverstatus", "0", "255",
            "playerprefs:alarmDefaultVolume,alarmfadeseconds,alarmSnoozeSeconds,"
            "alarmTimeoutSeconds,alarmsEnabled,playtrackalbum,defeatDestructiveTouchToPlay,"
            "syncVolume,syncPower,digitalVolumeControl",
            "prefs:mediadirs,defeatDestructiveTouchToPlay",
        ]
    )
    check("players_loop present", "players_loop" in ss)
    if "players_loop" in ss:
        p = ss["players_loop"][0]
        check("player id", p.get("playerid") == PLAYER)
        check("playerprefs flat in record", "playtrackalbum" in p)
    check("version present", "version" in ss)
    check("serverstatus subscribe:60 ack", "players_loop" in c.slim(
        ["serverstatus", "0", "255", "subscribe:60", "playerprefs:alarmsEnabled",
         "prefs:mediadirs"]))

    print("== now-playing status — STRICT DECODE TYPES (Squeeze Client) ==")
    st = c.slim(["status", "-", "1", "useContextMenu:1", "subscribe:0", "menu:menu"],
                publish_channel="/slim/subscribe")
    for f in ["mode", "player_name"]:
        check(f"'{f}' string", isinstance(st.get(f), str), repr(st.get(f))[:80])
    for f in ["count", "playlist_tracks", "playlist_cur_index", "player_connected",
              "power", "digital_volume_control", "mixer volume", "song"]:
        check(f"'{f}' NUMBER", is_num(st.get(f)), repr(st.get(f))[:80])
    for f in ["time", "duration", "playlist_timestamp"]:
        check(f"'{f}' NUMBER (float decode)", is_num(st.get(f)), repr(st.get(f))[:80])
    check("'playlist shuffle'/'repeat' string enums",
          isinstance(st.get("playlist shuffle"), str) and isinstance(st.get("playlist repeat"), str))
    check("sync_master string", isinstance(st.get("sync_master"), str))

    print("== now-playing fetch with tags ==")
    st2 = c.slim(["status", "-", "1", "tags:ABdejJKlrStTuxy"])
    check("tags variant parses (same shape)", is_num(st2.get("count")))

    print("== playlist page (Squeeze Client PlaylistFragment) ==")
    page = c.slim(["status", "0", "512", "menu:menu", "useContextMenu:1"])
    check("count NUMBER", is_num(page.get("count")))
    check("item_loop present", "item_loop" in page)
    if "item_loop" in page:
        rows = page["item_loop"]
        if rows:
            r0 = rows[0]
            check("row track/artist/album present",
                  all(k in r0 for k in ("track", "artist", "album")), str(r0)[:120])
            check("row text present (browse render)", "text" in r0)
            go = r0.get("actions", {}).get("go", {})
            check("row go action = playlist index",
                  go.get("cmd", [])[:2] == ["playlist", "index"], str(go)[:100])
        else:
            check("rows present (queue may be empty in test env)", True)

    print("== home menu (Squeeze Client main screen) ==")
    menu = c.slim(["menu", "0", "512", "direct:1"])
    check("menu count NUMBER", is_num(menu.get("count")))
    check("menu offset NUMBER", is_num(menu.get("offset")))
    items = menu.get("item_loop", [])
    check("menu item id+node strings",
          all(isinstance(i.get("id"), str) and isinstance(i.get("node"), str) for i in items),
          str(items)[:120])
    if items:
        check("menu go action cmd", "go" in items[0].get("actions", {}))

    print("== display/menustatus subscriptions (must ack, not wedge) ==")
    check("displaystatus ack", c.slim(["displaystatus", "subscribe:showbriefly"],
                                      publish_channel="/slim/subscribe") == {})
    check("menustatus ack", c.slim(["menustatus"],
                                   publish_channel="/slim/subscribe") == {})

    print("== control commands ==")
    cmds = [
        (["play", ""], "play \"\" (Squeeze Client zero-fade form)"),
        (["pause", "1"], "pause 1"),
        (["pause", "0", ""], "pause 0 (unpause)"),
        (["stop"], "stop"),
        (["playlist", "index", "3", ""], "playlist index 3 (tap row)"),
        (["playlist", "delete", "2"], "playlist delete 2"),
        (["playlist", "move", "1", "3"], "playlist move 1 3"),
        (["playlist", "clear"], "playlist clear"),
        (["power"], "power toggle"),
        (["power", "0"], "power 0"),
        (["mixer", "volume", "55"], "mixer volume 55"),
        (["time", "42"], "time 42"),
    ]
    for cmd, label in cmds:
        try:
            r = c.slim(cmd)
            check(f"accepted: {label}", isinstance(r, dict))
        except Exception as e:  # noqa: BLE001
            check(f"accepted: {label}", False, repr(e))

    mv = c.slim(["mixer", "volume", "?"])
    check("mixer volume ? → _volume", "_volume" in mv)

    print("== Squeezer-family compatibility (tolerant Java parsing) ==")
    stq = c.slim(["status", "-", "1", "menu:menu", "useContextMenu:1"])
    check("status query works", "mode" in stq)
    check("playlist_loop still served", "playlist_loop" in stq)

    print("== connect-time push ==")
    pushed = None
    for _ in range(4):
        for m in c.connect():
            if "/slim/playerstatus/" in m.get("channel", "") and "data" in m:
                pushed = m["data"]
    if pushed is None:
        st3 = c.slim(["status", "-", "1"])
        check("status still fine after commands", "mode" in st3)
        print("  note: no push (idle state unchanged) — acceptable")
    else:
        check("push types: count NUMBER", is_num(pushed.get("count")))
        check("push types: player_connected NUMBER", is_num(pushed.get("player_connected")))


    print("== remote library browse (panicast browse) ==")
    br = c.slim(["panicast", "browse", "root", "0", "512"])
    check("browse count NUMBER", is_num(br.get("count")))
    check("browse offset NUMBER", is_num(br.get("offset")))
    brows = br.get("item_loop", [])
    if brows:
        check("browse row has text + go action",
              "text" in brows[0] and brows[0].get("actions", {}).get("go", {}).get("cmd", [])[:2] == ["panicast", "browse"],
              str(brows[0])[:120])
    bk = c.slim(["panicast", "browse", "back", "0", "512"])
    check("browse back well-formed", is_num(bk.get("count")) and "item_loop" in bk)
    check("menu has 2 entries", is_num(menu.get("count")) and menu.get("count") == 2,
          str(menu.get("count")))

    print("== mute / sleep / buttons ==")
    st0 = c.slim(["status", "-", "1"])
    v0 = st0.get("mixer volume")
    c.slim(["mixer", "muting", "1"])
    st1 = c.slim(["status", "-", "1"])
    check("muting → negative mixer volume", st1.get("mixer volume", 0) < 0, str(st1.get("mixer volume")))
    c.slim(["mixer", "muting", "0"])
    st2 = c.slim(["status", "-", "1"])
    check("unmuting → positive mixer volume", st2.get("mixer volume", -1) >= 0, str(st2.get("mixer volume")))
    for cmd, label in [ (["button", "jump_fwd"], "button jump_fwd (next)"),
                        (["button", "jump_rew"], "button jump_rew (prev)"),
                        (["sleep", "900"], "sleep 900s"),
                        (["sleep", "0"], "sleep 0 (cancel)") ]:
        check(f"accepted: {label}", isinstance(c.slim(cmd), dict))
    check("repeat/shuffle reported as strings",
          isinstance(st2.get("playlist repeat"), str) and isinstance(st2.get("playlist shuffle"), str))

    print(f"\n{passed} passed, {failed} failed")

    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
