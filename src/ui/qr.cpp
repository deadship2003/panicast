// Y01: QR rendering implementation. See header.
#include "panicast/ui/qr.h"

#ifdef HAVE_QRENCODE
#include <qrencode.h>
#endif

#include "panicast/core/logger.h"

namespace panicast
{

bool qr_available() {
#ifdef HAVE_QRENCODE
    return true;
#else
    return false;
#endif
}

std::vector<std::string> render_qr_rows(const std::string &text) {
    std::vector<std::string> rows;
#ifdef HAVE_QRENCODE
    QRcode *q = QRcode_encodeString(text.c_str(), 0, QR_ECLEVEL_L, QR_MODE_8, 1);
    if (!q) {
        LOG("[QR] encode failed");
        return rows;
    }
    // Each QR module → 2 terminal columns ("██" = dark, "  " = light) so the aspect ratio scans.
    // Top/bottom quiet border of one light module for scannability.
    int w = q->width;
    auto mod = [&](int x, int y) -> bool {
        if (x < 0 || x >= w || y < 0 || y >= w)
            return false; // border = light
        return (q->data[y * w + x] & 1) != 0;
    };
    for (int y = -1; y <= w; ++y) {
        std::string row;
        row.reserve((w + 2) * 2);
        for (int x = -1; x <= w; ++x) {
            row += mod(x, y) ? "██" : "  ";
        }
        rows.push_back(std::move(row));
    }
    QRcode_free(q);
#else
    (void)text;
#endif
    return rows;
}

} // namespace panicast
