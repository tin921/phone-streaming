// QR code encoder (ISO/IEC 18004) for a short text such as a link: byte mode,
// error correction level M, the smallest version that fits, and the mask
// with the lowest penalty score.
#pragma once
#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace ps {

class QrCode {
public:
    int size = 0;                      // modules per side; 0 = nothing (text too long)

    bool dark(int x, int y) const {
        return x >= 0 && y >= 0 && x < size && y < size && modules[size_t(y) * size_t(size) + size_t(x)] != 0;
    }

    static QrCode encode(const std::string& text) {
        QrCode q;
        int ver = 1;
        for (;; ++ver) {
            if (ver > 40) return q;
            if (4 + countBits(ver) + int(text.size()) * 8 <= dataCodewords(ver) * 8) break;
        }
        // Data bits: mode, length, bytes, terminator, padding.
        std::vector<bool> bits;
        auto put = [&](uint32_t v, int n) { for (int i = n - 1; i >= 0; --i) bits.push_back(((v >> i) & 1) != 0); };
        put(4, 4);
        put(uint32_t(text.size()), countBits(ver));
        for (unsigned char c : text) put(c, 8);
        int capacity = dataCodewords(ver) * 8;
        put(0, std::min(4, capacity - int(bits.size())));
        put(0, (8 - int(bits.size()) % 8) % 8);
        for (uint32_t pad = 0xEC; int(bits.size()) < capacity; pad ^= 0xEC ^ 0x11) put(pad, 8);
        std::vector<uint8_t> data(bits.size() / 8, 0);
        for (size_t i = 0; i < bits.size(); ++i) if (bits[i]) data[i >> 3] |= uint8_t(1 << (7 - (i & 7)));

        // Error correction per block, then interleaved.
        int blocks = Blocks[ver], eccLen = EccPerBlock[ver];
        int raw = rawModules(ver) / 8;
        int shortBlocks = blocks - raw % blocks;
        int shortLen = raw / blocks;
        std::vector<uint8_t> divisor = rsDivisor(eccLen);
        std::vector<std::vector<uint8_t>> parts;
        for (int i = 0, k = 0; i < blocks; ++i) {
            int len = shortLen - eccLen + (i < shortBlocks ? 0 : 1);
            std::vector<uint8_t> dat(data.begin() + k, data.begin() + k + len);
            k += len;
            std::vector<uint8_t> ecc = rsRemainder(dat, divisor);
            if (i < shortBlocks) dat.push_back(0);
            dat.insert(dat.end(), ecc.begin(), ecc.end());
            parts.push_back(std::move(dat));
        }
        std::vector<uint8_t> all;
        for (size_t i = 0; i < parts[0].size(); ++i)
            for (size_t j = 0; j < parts.size(); ++j)
                if (i != size_t(shortLen - eccLen) || int(j) >= shortBlocks) all.push_back(parts[j][i]);

        q.size = ver * 4 + 17;
        q.modules.assign(size_t(q.size) * size_t(q.size), 0);
        q.function.assign(q.modules.size(), 0);
        q.drawFunctionPatterns(ver);
        q.drawCodewords(all);
        int best = 0;
        long bestScore = LONG_MAX;
        for (int m = 0; m < 8; ++m) {
            q.applyMask(m);
            q.drawFormatBits(m);
            long score = q.penalty();
            if (score < bestScore) { best = m; bestScore = score; }
            q.applyMask(m);                // XOR again: undone
        }
        q.applyMask(best);
        q.drawFormatBits(best);
        return q;
    }

private:
    std::vector<uint8_t> modules, function;

    // Level M, by version.
    static constexpr int EccPerBlock[41] = {-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26,
                                            26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28};
    static constexpr int Blocks[41] = {-1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5, 5, 8, 9, 9, 10, 10, 11, 13, 14, 16,
                                       17, 17, 18, 20, 21, 23, 25, 26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49};

    static int countBits(int ver) { return ver <= 9 ? 8 : 16; }

    static int rawModules(int ver) {
        int r = (16 * ver + 128) * ver + 64;
        if (ver >= 2) {
            int align = ver / 7 + 2;
            r -= (25 * align - 10) * align - 55;
            if (ver >= 7) r -= 36;
        }
        return r;
    }

    static int dataCodewords(int ver) { return rawModules(ver) / 8 - EccPerBlock[ver] * Blocks[ver]; }

    static uint8_t gfMul(uint8_t x, uint8_t y) {
        int z = 0;
        for (int i = 7; i >= 0; --i) {
            z = (z << 1) ^ ((z >> 7) * 0x11D);
            z ^= ((y >> i) & 1) * x;
        }
        return uint8_t(z);
    }

    static std::vector<uint8_t> rsDivisor(int degree) {
        std::vector<uint8_t> r(size_t(degree), 0);
        r.back() = 1;
        uint8_t root = 1;
        for (int i = 0; i < degree; ++i) {
            for (size_t j = 0; j < r.size(); ++j) {
                r[j] = gfMul(r[j], root);
                if (j + 1 < r.size()) r[j] ^= r[j + 1];
            }
            root = gfMul(root, 0x02);
        }
        return r;
    }

    static std::vector<uint8_t> rsRemainder(const std::vector<uint8_t>& data, const std::vector<uint8_t>& divisor) {
        std::vector<uint8_t> r(divisor.size(), 0);
        for (uint8_t b : data) {
            uint8_t factor = uint8_t(b ^ r[0]);
            r.erase(r.begin());
            r.push_back(0);
            for (size_t i = 0; i < r.size(); ++i) r[i] ^= gfMul(divisor[i], factor);
        }
        return r;
    }

    static std::vector<int> alignmentPositions(int ver) {
        if (ver == 1) return {};
        int n = ver / 7 + 2;
        int step = ver == 32 ? 26 : (ver * 4 + n * 2 + 1) / (n * 2 - 2) * 2;
        std::vector<int> r{6};
        for (int i = 0, pos = ver * 4 + 10; i < n - 1; ++i, pos -= step) r.insert(r.begin() + 1, pos);
        return r;
    }

    size_t at(int x, int y) const { return size_t(y) * size_t(size) + size_t(x); }
    void setFunction(int x, int y, bool d) { modules[at(x, y)] = d ? 1 : 0; function[at(x, y)] = 1; }

    void drawFunctionPatterns(int ver) {
        int n = size;
        for (int i = 0; i < n; ++i) { setFunction(6, i, i % 2 == 0); setFunction(i, 6, i % 2 == 0); }
        auto finder = [&](int cx, int cy) {
            for (int dy = -4; dy <= 4; ++dy)
                for (int dx = -4; dx <= 4; ++dx) {
                    int x = cx + dx, y = cy + dy;
                    if (x < 0 || y < 0 || x >= n || y >= n) continue;
                    int d = std::max(std::abs(dx), std::abs(dy));
                    setFunction(x, y, d != 2 && d != 4);
                }
        };
        finder(3, 3);
        finder(n - 4, 3);
        finder(3, n - 4);
        std::vector<int> al = alignmentPositions(ver);
        int na = int(al.size());
        for (int i = 0; i < na; ++i)
            for (int j = 0; j < na; ++j) {
                if ((i == 0 && j == 0) || (i == 0 && j == na - 1) || (i == na - 1 && j == 0)) continue;
                for (int dy = -2; dy <= 2; ++dy)
                    for (int dx = -2; dx <= 2; ++dx) setFunction(al[size_t(i)] + dx, al[size_t(j)] + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
            }
        drawFormatBits(0);
        if (ver >= 7) {
            int rem = ver;
            for (int i = 0; i < 12; ++i) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
            long b = long(ver) << 12 | rem;
            for (int i = 0; i < 18; ++i) {
                bool d = ((b >> i) & 1) != 0;
                int a = n - 11 + i % 3, c = i / 3;
                setFunction(a, c, d);
                setFunction(c, a, d);
            }
        }
    }

    void drawFormatBits(int mask) {
        int n = size;
        int d = (0 << 3) | mask;           // level M is 00
        int rem = d;
        for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
        int b = ((d << 10) | rem) ^ 0x5412;
        auto bit = [&](int i) { return ((b >> i) & 1) != 0; };
        for (int i = 0; i <= 5; ++i) setFunction(8, i, bit(i));
        setFunction(8, 7, bit(6));
        setFunction(8, 8, bit(7));
        setFunction(7, 8, bit(8));
        for (int i = 9; i < 15; ++i) setFunction(14 - i, 8, bit(i));
        for (int i = 0; i < 8; ++i) setFunction(n - 1 - i, 8, bit(i));
        for (int i = 8; i < 15; ++i) setFunction(8, n - 15 + i, bit(i));
        setFunction(8, n - 8, true);
    }

    void drawCodewords(const std::vector<uint8_t>& all) {
        int n = size;
        size_t i = 0;
        for (int right = n - 1; right >= 1; right -= 2) {
            if (right == 6) right = 5;
            for (int vert = 0; vert < n; ++vert)
                for (int j = 0; j < 2; ++j) {
                    int x = right - j;
                    bool upward = ((right + 1) & 2) == 0;
                    int y = upward ? n - 1 - vert : vert;
                    if (!function[at(x, y)] && i < all.size() * 8) {
                        modules[at(x, y)] = (all[i >> 3] >> (7 - (i & 7))) & 1;
                        ++i;
                    }
                }
        }
    }

    void applyMask(int m) {
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                bool inv;
                switch (m) {
                    case 0: inv = (x + y) % 2 == 0; break;
                    case 1: inv = y % 2 == 0; break;
                    case 2: inv = x % 3 == 0; break;
                    case 3: inv = (x + y) % 3 == 0; break;
                    case 4: inv = (x / 3 + y / 2) % 2 == 0; break;
                    case 5: inv = x * y % 2 + x * y % 3 == 0; break;
                    case 6: inv = (x * y % 2 + x * y % 3) % 2 == 0; break;
                    default: inv = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
                }
                if (inv && !function[at(x, y)]) modules[at(x, y)] ^= 1;
            }
    }

    long penalty() const {
        int n = size;
        long score = 0;
        static const bool finderLike[7] = {true, false, true, true, true, false, true};
        for (int pass = 0; pass < 2; ++pass)
            for (int a = 0; a < n; ++a) {
                auto get = [&](int b) { return b >= 0 && b < n && (pass == 0 ? dark(b, a) : dark(a, b)); };
                int run = 0;
                bool color = false;
                for (int b = 0; b < n; ++b) {
                    bool d = get(b);
                    if (b == 0 || d != color) {
                        if (run >= 5) score += run - 2;
                        color = d;
                        run = 1;
                    } else ++run;
                }
                if (run >= 5) score += run - 2;
                for (int b = 0; b + 7 <= n; ++b) {
                    bool match = true;
                    for (int k = 0; k < 7 && match; ++k) match = get(b + k) == finderLike[k];
                    if (!match) continue;
                    bool before = true, after = true;
                    for (int k = 1; k <= 4; ++k) { before = before && !get(b - k); after = after && !get(b + 6 + k); }
                    score += 40 * (long(before) + long(after));
                }
            }
        for (int y = 0; y + 1 < n; ++y)
            for (int x = 0; x + 1 < n; ++x) {
                bool c = dark(x, y);
                if (c == dark(x + 1, y) && c == dark(x, y + 1) && c == dark(x + 1, y + 1)) score += 3;
            }
        long darkCount = 0, total = long(n) * n;
        for (uint8_t v : modules) darkCount += v;
        long k = (std::labs(darkCount * 20 - total * 10) + total - 1) / total - 1;
        score += k * 10;
        return score;
    }
};

} // namespace ps
