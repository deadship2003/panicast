// Y01: terminal QR rendering for the Google OAuth verification URL.
//   Uses libqrencode when available (HAVE_QRENCODE); otherwise renders nothing and the caller
//   falls back to the plain-text user_code. The QR is drawn as a block of "  "/██ rows so it
//   scans from a phone camera aimed at the terminal.
#pragma once

#include <string>
#include <vector>

namespace panicast
{

// Render `text` as a list of display rows (each row uses two terminal columns per QR module,
// so a v3 QR ~29 modules → 58 columns). Returns empty when libqrencode is unavailable or on error.
std::vector<std::string> render_qr_rows(const std::string &text);

// Whether QR rendering is compiled in (HAVE_QRENCODE). For UI fallback decisions.
bool qr_available();

} // namespace panicast
