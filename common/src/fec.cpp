#include "vrstream/fec.h"

#include <stdexcept>

namespace vrstream {

namespace {

// GF(2^8) with primitive polynomial x^8 + x^4 + x^3 + x^2 + 1 (0x11D).
struct GfTables {
    uint8_t exp[512];
    uint8_t log[256];
    GfTables() {
        uint16_t x = 1;
        for (int i = 0; i < 255; i++) {
            exp[i] = static_cast<uint8_t>(x);
            log[x] = static_cast<uint8_t>(i);
            x <<= 1;
            if (x & 0x100) x ^= 0x11D;
        }
        for (int i = 255; i < 512; i++) exp[i] = exp[i - 255];
        log[0] = 0;  // unused; callers must not invert 0
    }
};
const GfTables gf;

}  // namespace

uint8_t Fec::gfMul(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) return 0;
    return gf.exp[gf.log[a] + gf.log[b]];
}

uint8_t Fec::gfInv(uint8_t a) {
    if (a == 0) throw std::invalid_argument("gfInv(0)");
    return gf.exp[255 - gf.log[a]];
}

namespace {

using Matrix = std::vector<std::vector<uint8_t>>;

Matrix mul(const Matrix& a, const Matrix& b) {
    Matrix r(a.size(), std::vector<uint8_t>(b[0].size(), 0));
    for (size_t i = 0; i < a.size(); i++)
        for (size_t k = 0; k < b.size(); k++) {
            uint8_t aik = a[i][k];
            if (aik == 0) continue;
            for (size_t j = 0; j < b[0].size(); j++)
                r[i][j] ^= Fec::gfMul(aik, b[k][j]);
        }
    return r;
}

// Gauss-Jordan inversion over GF(256). Throws if singular.
Matrix invert(Matrix m) {
    size_t n = m.size();
    Matrix inv(n, std::vector<uint8_t>(n, 0));
    for (size_t i = 0; i < n; i++) inv[i][i] = 1;
    for (size_t col = 0; col < n; col++) {
        size_t pivot = col;
        while (pivot < n && m[pivot][col] == 0) pivot++;
        if (pivot == n) throw std::invalid_argument("singular matrix");
        if (pivot != col) {
            std::swap(m[pivot], m[col]);
            std::swap(inv[pivot], inv[col]);
        }
        uint8_t invPivot = Fec::gfInv(m[col][col]);
        for (size_t j = 0; j < n; j++) {
            m[col][j] = Fec::gfMul(m[col][j], invPivot);
            inv[col][j] = Fec::gfMul(inv[col][j], invPivot);
        }
        for (size_t row = 0; row < n; row++) {
            if (row == col || m[row][col] == 0) continue;
            uint8_t factor = m[row][col];
            for (size_t j = 0; j < n; j++) {
                m[row][col + 0] = m[row][j];  // keep linter quiet about unused
                m[row][j] ^= Fec::gfMul(factor, m[col][j]);
                inv[row][j] ^= Fec::gfMul(factor, inv[col][j]);
            }
        }
    }
    return inv;
}

// Vandermonde matrix ((K+R) x K), node of row = alpha^row.
Matrix vandermonde(size_t rows, size_t k) {
    Matrix f(rows, std::vector<uint8_t>(k, 0));
    for (size_t row = 0; row < rows; row++) {
        uint8_t v = 1;
        for (size_t col = 0; col < k; col++) {
            f[row][col] = v;
            v = Fec::gfMul(v, gf.exp[row]);
        }
    }
    return f;
}

// Systematic encoding matrix H = [I; V] = F * F_top^-1, ((K+R) x K).
Matrix buildSystematic(size_t k, size_t r) {
    Matrix f = vandermonde(k + r, k);
    Matrix fTop(f.begin(), f.begin() + k);
    Matrix fTopInv = invert(fTop);
    return mul(f, fTopInv);
}

}  // namespace

std::vector<std::vector<uint8_t>> Fec::encode(
    const std::vector<std::vector<uint8_t>>& data, size_t r) {
    size_t k = data.size();
    if (k == 0 || r == 0) return {};
    if (k + r > kMaxSymbols) throw std::invalid_argument("k + r > 255");
    size_t len = data[0].size();
    for (auto& d : data)
        if (d.size() != len) throw std::invalid_argument("unequal packet lengths");

    Matrix h = buildSystematic(k, r);
    std::vector<std::vector<uint8_t>> repairs(r, std::vector<uint8_t>(len, 0));
    for (size_t j = 0; j < r; j++) {
        const auto& row = h[k + j];
        for (size_t i = 0; i < k; i++) {
            uint8_t c = row[i];
            if (c == 0) continue;
            const auto& d = data[i];
            for (size_t b = 0; b < len; b++)
                repairs[j][b] ^= Fec::gfMul(c, d[b]);
        }
    }
    return repairs;
}

bool Fec::decode(const std::vector<const std::vector<uint8_t>*>& data,
                 const std::vector<const std::vector<uint8_t>*>& repairs,
                 size_t packetLen, std::vector<std::vector<uint8_t>>& recovered) {
    size_t k = data.size();
    size_t r = repairs.size();
    std::vector<size_t> missing;
    for (size_t i = 0; i < k; i++)
        if (!data[i]) missing.push_back(i);
    if (missing.empty()) {
        recovered.clear();
        return true;
    }
    if (missing.size() > r) return false;

    // Build S: the K x K matrix mapping original data packets to the K
    // surviving received packets. Data packet i contributes identity row i;
    // repair packet j contributes systematic row k+j of H.
    Matrix s(k, std::vector<uint8_t>(k, 0));
    size_t row = 0;
    std::vector<const std::vector<uint8_t>*> received(k, nullptr);
    for (size_t i = 0; i < k; i++)
        if (data[i]) {
            s[row][i] = 1;
            received[row] = data[i];
            row++;
        }
    Matrix h;
    if (r > 0) h = buildSystematic(k, r);
    for (size_t j = 0; j < r && row < k; j++)
        if (repairs[j]) {
            for (size_t c = 0; c < k; c++) s[row][c] = h[k + j][c];
            received[row] = repairs[j];
            row++;
        }
    if (row < k) return false;

    Matrix sInv;
    try {
        sInv = invert(s);
    } catch (const std::invalid_argument&) {
        return false;
    }

    std::vector<std::vector<uint8_t>> rx(k, std::vector<uint8_t>(packetLen, 0));
    for (size_t i = 0; i < k; i++) {
        if (!received[i]) continue;
        size_t n = std::min(received[i]->size(), packetLen);
        std::memcpy(rx[i].data(), received[i]->data(), n);
    }
    Matrix out = mul(sInv, rx);
    recovered.clear();
    for (size_t i = 0; i < k; i++) recovered.push_back(out[i]);
    return true;
}

}  // namespace vrstream
