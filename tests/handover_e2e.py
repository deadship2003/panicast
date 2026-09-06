import base64, json, os, signal, socket, struct, subprocess, time
HOST, PORT = "127.0.0.1", 9190
AUTH = base64.b64encode(b"panicast:panicast").decode()
PLAYER = "00:00:00:00:84:21"
TH = "/home/xx/.claude/jobs/ffa13f01/tmp/panicast-test"

def post(sock, msgs, timeout=6):
    sock.settimeout(timeout)
    body = json.dumps(msgs)
    sock.sendall((f"POST /cometd HTTP/1.1\r\nHost: {HOST}\r\nAuthorization: Basic {AUTH}\r\n"
        f"Content-Type: application/json\r\nContent-Length: {len(body)}\r\nConnection: keep-alive\r\n\r\n{body}").encode())
    buf = b""
    while b"\r\n\r\n" not in buf: buf += sock.recv(65536)
    head, rest = buf.split(b"\r\n\r\n", 1)
    clen = next(int(l.split(b":",1)[1]) for l in head.split(b"\r\n") if l.lower().startswith(b"content-length:"))
    while len(rest) < clen: rest += sock.recv(65536)
    return json.loads(rest[:clen])

rq = socket.create_connection((HOST, PORT), timeout=6)
cid = post(rq, [{"channel":"/meta/handshake","id":"1","supportedConnectionTypes":["streaming"],"version":"1.0"}])[0]["clientId"]
ls = socket.create_connection((HOST, PORT), timeout=6)
body = json.dumps([{"channel":"/meta/connect","id":"8","connectionType":"streaming","clientId":cid,"advice":{"timeout":0}}])
ls.sendall((f"POST /cometd HTTP/1.1\r\nHost: {HOST}\r\nAuthorization: Basic {AUTH}\r\n"
    f"Content-Type: application/json\r\nContent-Length: {len(body)}\r\nConnection: keep-alive\r\n\r\n{body}").encode())
ls_buf = b""
while b"\r\n\r\n" not in ls_buf: ls_buf += ls.recv(65536)
assert b"chunked" in ls_buf.lower(), "stream not held"

us_path = "/tmp/panicast-hs-test.sock"
try: os.unlink(us_path)
except FileNotFoundError: pass
us = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); us.bind(us_path); us.listen(1)
trig = socket.create_connection((HOST, PORT), timeout=6)
post(trig, [{"channel":"/slim/request","id":"99","data":{"response":"/ho",
            "request":[PLAYER,["panicast","handover",us_path]]}}])
us.settimeout(4)
conn, _ = us.accept()
data, anc, _, _ = conn.recvmsg(65536, socket.CMSG_SPACE(64*4))
fds = []
for lvl, typ, cd in anc:
    if lvl == socket.SOL_SOCKET and typ == socket.SCM_RIGHTS:
        fds = list(struct.unpack("%di" % (len(cd)//4), cd))
meta = json.loads(data.split(b"\n")[0])
print("fd-pass: %d fds, %d conns, listen_idx=%d" % (len(fds), len(meta.get("conns",[])), meta.get("listen")))
conn.close(); us.close(); os.unlink(us_path)

apid = int(open(f"{TH}/.local/share/panicast/panicast-daemon.pid").read().strip())
os.kill(apid, signal.SIGTERM)
for _ in range(60):
    try: os.kill(apid, 0); time.sleep(0.25)
    except ProcessLookupError: break
print("daemon A exited; phone conns alive (fd dups)")

intent = "/run/user/%d/panicast-handover.intent" % os.getuid()
open(intent, "w").close()
b = subprocess.Popen(["/home/xx/panicast/build/panicast", "--daemon"], env=dict(os.environ, HOME=TH),
                     stdout=open(f"{TH}/daemonB.out","w"), stderr=subprocess.STDOUT)
hs_path = "/run/user/%d/panicast-daemon-hs.sock" % os.getuid()
deadline = time.time() + 12
while not os.path.exists(hs_path) and time.time() < deadline: time.sleep(0.05)
print("B boot sock:", os.path.exists(hs_path))
ds = None
for _ in range(40):  # fresh socket per attempt (a refused stream socket is unusable)
    try:
        c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        c.settimeout(4)
        c.connect(hs_path)
        ds = c
        break
    except (ConnectionRefusedError, FileNotFoundError):
        c.close()
        time.sleep(0.1)
if ds is None:
    raise RuntimeError("daemon boot socket never accepted")
ds.sendmsg([(json.dumps(meta)+"\n").encode()],
           [(socket.SOL_SOCKET, socket.SCM_RIGHTS, struct.pack("%di" % len(fds), *fds))])
ds.shutdown(socket.SHUT_WR); ds.close()
print("fds forwarded to B")

rq.settimeout(6)
st = post(rq, [{"channel":"/slim/request","id":"100","data":{"response":"/v",
               "request":[PLAYER,["status","-","1"]]}}])
print("ORIGINAL rq socket served by B → mode:", st[1]["data"].get("mode"))
ls.settimeout(25); got = b""
try:
    while b"}]" not in got: got += ls.recv(65536)
except socket.timeout: pass
print("ORIGINAL stream still pushing:", bool(got.strip()))
os.kill(b.pid, signal.SIGTERM)
