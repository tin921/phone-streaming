// Church Phone Stream: shared Windows headers and small helpers.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <share.h>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ps {

inline std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

inline std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

inline double nowSeconds() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) / double(freq.QuadPart);
}

inline std::wstring errorText(DWORD code) {
    wchar_t* text = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    HMODULE winhttp = GetModuleHandleW(L"winhttp.dll");
    if (code >= 12000 && code < 12200 && winhttp) flags = (flags & ~FORMAT_MESSAGE_FROM_SYSTEM) | FORMAT_MESSAGE_FROM_HMODULE;
    FormatMessageW(flags, winhttp, code, 0, (LPWSTR)&text, 0, nullptr);
    std::wstring s = text ? text : L"";
    if (text) LocalFree(text);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ' || s.back() == L'.')) s.pop_back();
    if (s.empty()) s = L"error " + std::to_wstring(code);
    return s;
}

template <class T> struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { reset(); }
    void reset() { if (p) { p->Release(); p = nullptr; } }
    T** out() { reset(); return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// The app's folder: %APPDATA%\ChurchPhoneStream (settings, logs).
inline std::wstring appFolder() {
    PWSTR appData = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) dir = std::wstring(appData) + L"\\ChurchPhoneStream";
    if (appData) CoTaskMemFree(appData);
    return dir;
}

inline std::wstring logFolder() { std::wstring d = appFolder(); return d.empty() ? d : d + L"\\logs"; }

// Recent events for the window's log, newest last; every line also goes to
// %APPDATA%\ChurchPhoneStream\logs\phonestream-YYYY-MM-DD.log (UTF-8, one
// file a day, kept 60 days).
class EventLog {
    std::mutex m;
    std::deque<std::wstring> lines;
    uint64_t version = 0;
    FILE* file = nullptr;
    int fileDay = -1;
    bool folderReady = false;

    void writeFile(const SYSTEMTIME& t, const std::wstring& line) {
        std::wstring dir = logFolder();
        if (dir.empty()) return;
        if (!folderReady) {
            SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
            folderReady = true;
            // Older than 60 days: deleted.
            WIN32_FIND_DATAW f;
            HANDLE h = FindFirstFileW((dir + L"\\phonestream-*.log").c_str(), &f);
            if (h != INVALID_HANDLE_VALUE) {
                FILETIME now;
                GetSystemTimeAsFileTime(&now);
                ULONGLONG limit = ULARGE_INTEGER{{now.dwLowDateTime, now.dwHighDateTime}}.QuadPart - 60ULL * 86400ULL * 10000000ULL;
                do {
                    if (ULARGE_INTEGER{{f.ftLastWriteTime.dwLowDateTime, f.ftLastWriteTime.dwHighDateTime}}.QuadPart < limit)
                        DeleteFileW((dir + L"\\" + f.cFileName).c_str());
                } while (FindNextFileW(h, &f));
                FindClose(h);
            }
        }
        int day = t.wYear * 10000 + t.wMonth * 100 + t.wDay;
        if (day != fileDay) {
            if (file) fclose(file);
            wchar_t name[64];
            swprintf_s(name, L"\\phonestream-%04d-%02d-%02d.log", t.wYear, t.wMonth, t.wDay);
            file = _wfsopen((dir + name).c_str(), L"ab", _SH_DENYNO);
            fileDay = day;
        }
        if (!file) return;
        int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), int(line.size()), nullptr, 0, nullptr, nullptr);
        std::string utf8(size_t(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), int(line.size()), utf8.data(), n, nullptr, nullptr);
        utf8 += "\r\n";
        fwrite(utf8.data(), 1, utf8.size(), file);
        fflush(file);
    }

public:
    ~EventLog() { if (file) fclose(file); }

    void add(const std::wstring& text) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t stamp[16];
        swprintf_s(stamp, L"%02d:%02d:%02d  ", t.wHour, t.wMinute, t.wSecond);
        std::lock_guard<std::mutex> lock(m);
        lines.push_back(stamp + text);
        while (lines.size() > 200) lines.pop_front();
        ++version;
        writeFile(t, lines.back());
    }
    uint64_t read(std::deque<std::wstring>& out) {
        std::lock_guard<std::mutex> lock(m);
        out = lines;
        return version;
    }
    uint64_t current() { std::lock_guard<std::mutex> lock(m); return version; }
};

inline EventLog& eventLog() { static EventLog log; return log; }

} // namespace ps
