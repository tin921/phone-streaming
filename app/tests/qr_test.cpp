// Writes QR codes for a few texts as BMP files (decoded by tests/qr_check.py).
//   qr_test.exe <out folder>
#include <cstdio>
#include <string>
#include "../src/qr.hpp"

static bool writeBmp(const std::string& file, const ps::QrCode& q, int scale, int quiet) {
    int side = (q.size + 2 * quiet) * scale;
    int rowBytes = (side * 3 + 3) & ~3;
    FILE* f = nullptr;
    if (fopen_s(&f, file.c_str(), "wb") || !f) return false;
    auto u16 = [&](unsigned v) { fputc(int(v & 0xFF), f); fputc(int((v >> 8) & 0xFF), f); };
    auto u32 = [&](unsigned v) { u16(v & 0xFFFF); u16(v >> 16); };
    fputc('B', f); fputc('M', f); u32(54u + unsigned(rowBytes * side)); u32(0); u32(54);
    u32(40); u32(unsigned(side)); u32(unsigned(side)); u16(1); u16(24); u32(0); u32(unsigned(rowBytes * side)); u32(2835); u32(2835); u32(0); u32(0);
    for (int row = side - 1; row >= 0; --row) {
        for (int col = 0; col < side; ++col) {
            bool d = q.dark(col / scale - quiet, row / scale - quiet);
            for (int k = 0; k < 3; ++k) fputc(d ? 0 : 255, f);
        }
        for (int p = side * 3; p < rowBytes; ++p) fputc(0, f);
    }
    fclose(f);
    return true;
}

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : ".";
    const std::string texts[] = {
        "HELLO",
        "https://phone.example.org/",
        "https://phone.example.org/listen/0123456789abcdef0123456789abcdef",
        "https://phone.example.org/listen/0123456789abcdef0123456789abcdef?x=" + std::string(60, 'a'),
        std::string(250, 'z') + "-end",
    };
    int i = 0;
    for (const std::string& t : texts) {
        ps::QrCode q = ps::QrCode::encode(t);
        std::string file = dir + "\\qr" + std::to_string(i++) + ".bmp";
        if (!writeBmp(file, q, 6, 4)) return 1;
        printf("%s\t%d\t%s\n", file.c_str(), (q.size - 17) / 4, t.c_str());
    }
    return 0;
}
