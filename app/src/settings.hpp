// Settings in %APPDATA%\ChurchPhoneStream\settings.ini. The app key is built
// in (PS_INGEST_KEY); a different one can be given once with --key, and is
// then kept encrypted for this Windows user (DPAPI).
#pragma once
#include "common.hpp"

#ifndef PS_INGEST_KEY
#define PS_INGEST_KEY ""
#endif
#ifndef PS_DEFAULT_SERVER
#define PS_DEFAULT_SERVER "wss://phone.example.org/ingest"
#endif

namespace ps {

struct Settings {
    std::wstring server = widen(PS_DEFAULT_SERVER);
    std::wstring source;          // "" = Windows' default speakers, "tone", or a device id
    std::string keyOverride;      // empty = the built-in key

    std::string key() const { return keyOverride.empty() ? std::string(PS_INGEST_KEY) : keyOverride; }

    static std::wstring folder() { return appFolder(); }

    static std::string protect(const std::string& plain) {
        DATA_BLOB in{DWORD(plain.size()), (BYTE*)plain.data()}, out{};
        if (!CryptProtectData(&in, L"ChurchPhoneStream", nullptr, nullptr, nullptr, 0, &out)) return {};
        std::string hex;
        char b[3];
        for (DWORD i = 0; i < out.cbData; ++i) { sprintf_s(b, "%02x", out.pbData[i]); hex += b; }
        LocalFree(out.pbData);
        return hex;
    }

    static std::string unprotect(const std::string& hex) {
        std::vector<BYTE> bytes;
        for (size_t i = 0; i + 1 < hex.size(); i += 2) bytes.push_back(BYTE(strtoul(hex.substr(i, 2).c_str(), nullptr, 16)));
        DATA_BLOB in{DWORD(bytes.size()), bytes.data()}, out{};
        if (bytes.empty() || !CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) return {};
        std::string plain(reinterpret_cast<char*>(out.pbData), out.cbData);
        LocalFree(out.pbData);
        return plain;
    }

    void load() {
        FILE* f = nullptr;
        if (_wfopen_s(&f, (folder() + L"\\settings.ini").c_str(), L"rb") || !f) return;
        char line[2048];
        while (fgets(line, sizeof(line), f)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            size_t eq = s.find('=');
            if (eq == std::string::npos) continue;
            std::string k = s.substr(0, eq), v = s.substr(eq + 1);
            if (k == "server" && !v.empty()) server = widen(v);
            else if (k == "source") source = widen(v);
            else if (k == "key") keyOverride = unprotect(v);
        }
        fclose(f);
    }

    void save() const {
        std::wstring dir = folder();
        if (dir.empty()) return;
        CreateDirectoryW(dir.c_str(), nullptr);
        FILE* f = nullptr;
        if (_wfopen_s(&f, (dir + L"\\settings.ini").c_str(), L"wb") || !f) return;
        fprintf(f, "server=%s\nsource=%s\n", narrow(server).c_str(), narrow(source).c_str());
        if (!keyOverride.empty()) fprintf(f, "key=%s\n", protect(keyOverride).c_str());
        fclose(f);
    }
};

} // namespace ps
