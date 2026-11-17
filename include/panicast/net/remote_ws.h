// WebSocket frontend (N06): RFC6455 handshake + frame codec + a socketpair bridge that feeds the
//   SAME PRP line-protocol RemoteSession used by raw TCP. One HTTP listener (port = tcp_port + 1)
//   serves the embedded BS web client (GET /) AND upgrades to WebSocket (WS) — so a browser opens
//   http://host:port/ and speaks PRP over WS text frames.
//
//   Bridge model (per WS connection): a socketpair (s1, s2) — RemoteSession runs on s1 (reads PRP
//   commands, writes PRP responses); the WS bridge thread runs on s2 (decodes WS frames → writes
//   PRP lines to s2; reads PRP responses from s2 → encodes WS text frames → sends to the browser).
//   RemoteSession is unchanged — it just sees a raw byte stream on s1.
#pragma once

#include <string>

namespace panicast
{

class RemoteServer;

// Serve one WS/HTTP connection (runs on a worker thread). Does the HTTP parse; if it's a WS
//   upgrade, handshakes + bridges PRP over WS (registering the session for idle push); otherwise
//   serves the embedded BS client for GET / (or 404). Blocks until the connection closes.
void ws_serve_connection(int client_fd, RemoteServer &server, bool localhost);

} // namespace panicast
