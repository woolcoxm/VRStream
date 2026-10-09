// Reed-Solomon erasure FEC over GF(2^8), column-wise across packets.
//
// Construction (Intel ISA-L style, provably MDS):
//   F = Vandermonde matrix ((K+R) x K) with nodes alpha^0..alpha^(K+R-1)
//   F_top = first K rows (square Vandermonde, invertible)
//   H = F * F_top^-1 = [ I ; V ]   (systematic: first K rows = identity)
// Any K rows of H are invertible because any K rows of F form a square
// Vandermonde matrix with distinct nodes. Therefore losing up to R packets of
// a (K+R) group is always recoverable.
//
// Symbol layout: packets are byte columns. Data packets carry the original
// bytes; repair packet j carries V[j] * data. All packets in a group must be
// the same length (the packetizer zero-pads the logical tail).
#pragma once

#include <cstdint>
#include <vector>

namespace vrstream {

class Fec {
  public:
    // Maximum total symbols per group (GF(256) bound).
    static constexpr size_t kMaxSymbols = 255;

    // Generates R repair packets for K equal-length data packets.
    // data[i] must all have the same length. Returns R packets, each of the
    // same length. K + R must be <= kMaxSymbols and K >= 1.
    static std::vector<std::vector<uint8_t>> encode(
        const std::vector<std::vector<uint8_t>>& data, size_t r);

    // Recovers up to R erased packets. erased[i] = true means data[i] was not
    // received. repairs[j] = repair packet j (may contain nullptr entries for
    // lost repairs). Returns the recovered packets (only erased indices are
    // meaningful). Returns false if more than R packets are missing.
    static bool decode(const std::vector<const std::vector<uint8_t>*>& data,
                       const std::vector<const std::vector<uint8_t>*>& repairs,
                       size_t packetLen, std::vector<std::vector<uint8_t>>& recovered);

    // GF(256) arithmetic exposed for tests.
    static uint8_t gfMul(uint8_t a, uint8_t b);
    static uint8_t gfInv(uint8_t a);
};

}  // namespace vrstream
