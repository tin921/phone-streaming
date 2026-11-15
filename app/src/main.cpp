// Church Phone Stream: sends what this PC plays to the phone server, for
// callers to hear by phone, and shows who is listening. A separate program
// from OBS: it only listens to Windows' output in shared mode and opens one
// outgoing connection (it is connected while open; Start/Stop stream).
//
// Command line (all optional, for this run only; --key is kept):
//   --server wss://host/ingest   --key KEY   --source default|tone|<device name>   --autostart
#include "common.hpp"
#include "capture.hpp"
#include "uplink.hpp"
#include "settings.hpp"
#include "qr.hpp"

using namespace ps;

namespace {

enum : int {
    IdStart = 101, IdState, IdSource, IdRefresh, IdMeter, IdMeterText,
    IdLine1, IdLine2, IdLine3, IdLinkLabel, IdLink, IdServerLabel, IdServer,
    IdQr, IdGraph, IdCalls, IdLog, IdLogs
};
constexpr UINT_PTR TimerId = 1;
const wchar_t* const ClassName = L"ChurchPhoneStreamWindow";
const wchar_t* const Title = L"Church Phone Stream";

HWND window = nullptr;
HFONT bodyFont = nullptr, bigFont = nullptr, smallFont = nullptr, linkFont = nullptr;
UINT dpi = 96;
Settings stored;            // what settings.ini holds
Settings settings;          // in use: stored + command line
std::wstring cmdServer;     // --server, for this run only
AudioCapture capture;
Uplink uplink;
std::vector<OutputDevice> devices;
uint64_t logVersion = ~0ull;
float meterPeak = -120.f, meterRms = -120.f, peakHold = -120.f;
double peakHoldAt = 0;
bool autostart = false;

Board board;
uint64_t boardVersion = ~0ull;
std::vector<size_t> rows;   // board.calls indexes in list order
int selectedCall = 0;
bool fillingList = false;
std::wstring linkUrl;
QrCode qr;

// This process's CPU and memory.
double cpuOneCore = 0, cpuAllCores = 0, memoryMb = 0, lastCpuAt = 0;
ULONGLONG lastCpuTime = 0;
int processors = 1;

int S(int v) { return MulDiv(v, int(dpi), 96); }

HWND item(int id) { return GetDlgItem(window, id); }

void setText(int id, const std::wstring& text) {
    wchar_t current[1024];
    GetWindowTextW(item(id), current, 1024);
    if (text != current) SetWindowTextW(item(id), text.c_str());
}

std::wstring clockText(double seconds) {
    long s = long(std::max(0.0, seconds));
    wchar_t text[32];
    if (s >= 3600) swprintf_s(text, L"%ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
    else swprintf_s(text, L"%ld:%02ld", s / 60, s % 60);
    return text;
}

std::wstring timeOfDay(double epoch, bool seconds = true) {
    __time64_t t = __time64_t(epoch);
    tm local{};
    if (epoch <= 0 || _localtime64_s(&local, &t)) return L"\x2013";
    wchar_t text[16];
    if (seconds) swprintf_s(text, L"%02d:%02d:%02d", local.tm_hour, local.tm_min, local.tm_sec);
    else swprintf_s(text, L"%02d:%02d", local.tm_hour, local.tm_min);
    return text;
}

double serverNow() { return epochNow() + board.clockOffset; }

bool onLine(const CallInfo& c) { return c.connectedAt > 0 && c.endedAt == 0; }

void makeFonts() {
    for (HFONT f : {bodyFont, bigFont, smallFont, linkFont}) if (f) DeleteObject(f);
    auto font = [](int pt, int weight, bool underline) {
        return CreateFontW(-MulDiv(pt, int(dpi), 72), 0, 0, 0, weight, 0, underline, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    };
    bodyFont = font(9, FW_NORMAL, false);
    bigFont = font(14, FW_SEMIBOLD, false);
    smallFont = font(8, FW_NORMAL, false);
    linkFont = font(9, FW_NORMAL, true);
}

void applyFonts() {
    for (int id = IdStart; id <= IdLogs; ++id) {
        HFONT f = (id == IdStart || id == IdState) ? bigFont : (id == IdLink || id == IdLogs) ? linkFont : bodyFont;
        SendMessageW(item(id), WM_SETFONT, (WPARAM)f, TRUE);
    }
}

void place(int id, int x, int y, int w, int h) {
    SetWindowPos(item(id), nullptr, S(x), S(y), S(std::max(w, 1)), S(std::max(h, 1)), SWP_NOZORDER | SWP_NOACTIVATE);
}

const struct { const wchar_t* title; int width; } Columns[] = {
    {L"Caller", 136}, {L"Via", 52}, {L"IP address", 104}, {L"Called", 62}, {L"Ended", 62},
    {L"Length", 54}, {L"Started", 112}, {L"Now", 150},
};

void layout() {
    RECT rc;
    GetClientRect(window, &rc);
    int w = MulDiv(rc.right, 96, int(dpi)), h = MulDiv(rc.bottom, 96, int(dpi));
    const int m = 12, qrSide = 112;
    int right = w - m - qrSide - m;                // the left column's right edge
    int comboX = std::max(326, right - 262);
    place(IdStart, m, 8, 156, 36);
    place(IdState, 176, 8, comboX - 182, 36);
    place(IdSource, comboX, 13, right - 28 - comboX, 300);
    place(IdRefresh, right - 24, 12, 24, 24);
    place(IdMeter, m, 52, right - m - 66, 16);
    place(IdMeterText, right - 60, 51, 60, 18);
    place(IdLine1, m, 74, right - m, 18);
    place(IdLine2, m, 92, right - m, 18);
    place(IdLine3, m, 110, right - m, 18);
    place(IdQr, w - m - qrSide, 8, qrSide, qrSide);
    place(IdLinkLabel, m, 132, 80, 18);
    place(IdLink, 96, 132, w - m - 96, 18);
    place(IdServerLabel, m, 156, 80, 18);
    int serverW = std::min(380, w - m - 96 - 130);
    place(IdServer, 96, 153, serverW, 22);
    place(IdLogs, 96 + serverW + 14, 156, 116, 18);
    const int graphY = 184, graphH = 150, logH = 72;
    place(IdGraph, m, graphY, w - 2 * m, graphH);
    int listY = graphY + graphH + 8;
    int listH = std::max(60, h - m - logH - 6 - listY);
    place(IdCalls, m, listY, w - 2 * m, listH);
    place(IdLog, m, listY + listH + 6, w - 2 * m, logH);
    HWND list = item(IdCalls);
    int used = 0;
    for (int i = 0; i < 7; ++i) { ListView_SetColumnWidth(list, i, S(Columns[i].width)); used += Columns[i].width; }
    ListView_SetColumnWidth(list, 7, S(std::max(Columns[7].width, w - 2 * m - used - 24)));
    InvalidateRect(window, nullptr, TRUE);
}

void fillSources() {
    HWND combo = item(IdSource);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    devices = listOutputDevices();
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Desktop audio (default speakers)");
    for (const OutputDevice& d : devices) SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)(L"Speakers: " + d.name).c_str());
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Test tone (440 Hz beeps)");
    int select = 0;
    if (settings.source == AudioCapture::TestTone) select = int(devices.size()) + 1;
    for (size_t i = 0; i < devices.size(); ++i) if (devices[i].id == settings.source) select = int(i) + 1;
    SendMessageW(combo, CB_SETCURSEL, select, 0);
}

std::wstring selectedSource() {
    int i = int(SendMessageW(item(IdSource), CB_GETCURSEL, 0, 0));
    if (i <= 0) return AudioCapture::DefaultDevice;
    if (i == int(devices.size()) + 1) return AudioCapture::TestTone;
    return devices[size_t(i) - 1].id;
}

void startCapture() {
    settings.source = selectedSource();
    capture.start(settings.source, [](const int16_t* pcm, size_t n) { uplink.push(pcm, n); });
}

void saveSource() {
    stored.source = settings.source;
    stored.save();
}

void commitServer() {
    wchar_t text[1024];
    GetWindowTextW(item(IdServer), text, 1024);
    std::wstring s = text;
    if (s == settings.server) return;
    ServerAddress a;
    if (!parseServerAddress(s, a)) {
        MessageBoxW(window, L"The server address must start with wss:// (for example wss://phone.example.org/ingest).", Title, MB_ICONWARNING);
        SetWindowTextW(item(IdServer), settings.server.c_str());
        return;
    }
    settings.server = s;
    uplink.setServer(s);
    eventLog().add(L"Server changed to " + s);
    if (s != cmdServer) {
        stored.server = s;
        stored.save();
    }
}

void startStop() {
    if (!uplink.wantsStream()) {
        if (settings.key().empty()) {
            MessageBoxW(window, L"This copy of the app has no key built in. Start it once with --key and the server's INGEST_TOKEN.", Title, MB_ICONWARNING);
            return;
        }
        uplink.startStreaming();
    } else {
        uplink.stopStreaming();
    }
}

void openLink() {
    if (!linkUrl.empty()) ShellExecuteW(window, L"open", linkUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void updateProcessStats() {
    FILETIME created, exited, kernel, user;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        ULONGLONG used = (ULARGE_INTEGER{{kernel.dwLowDateTime, kernel.dwHighDateTime}}).QuadPart +
                         (ULARGE_INTEGER{{user.dwLowDateTime, user.dwHighDateTime}}).QuadPart;
        double now = nowSeconds();
        if (lastCpuAt > 0 && now > lastCpuAt) {
            cpuOneCore = double(used - lastCpuTime) / 1e7 / (now - lastCpuAt) * 100;
            cpuAllCores = cpuOneCore / processors;
        }
        lastCpuTime = used;
        lastCpuAt = now;
    }
    // PROCESS_MEMORY_COUNTERS_EX2 (Windows 10 21H2 and later): the private
    // working set, which Task Manager shows as Memory.
    struct Counters {
        PROCESS_MEMORY_COUNTERS_EX base;
        SIZE_T PrivateWorkingSetSize;
        ULONG64 SharedCommitUsage;
    } c{};
    c.base.cb = sizeof(c);
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&c, sizeof(c))) memoryMb = double(c.PrivateWorkingSetSize) / 1048576.0;
    else {
        c.base.cb = sizeof(c.base);
        if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&c.base, sizeof(c.base))) memoryMb = double(c.base.WorkingSetSize) / 1048576.0;
    }
}

// --- the call list ---------------------------------------------------------------

std::wstring callText(const CallInfo& c, int column) {
    switch (column) {
    case 0: {
        if (c.via == L"browser") return L"Browser test player";
        std::wstring s = formatPhone(c.from);
        if (!c.name.empty()) s += L"  " + c.name;
        return s;
    }
    case 1: return c.via == L"phone" ? L"Phone" : c.via == L"browser" ? L"Web" : L"Test";
    case 2: return c.ip.empty() ? L"\x2013" : c.ip;
    case 3: return timeOfDay(c.at);
    case 4: return c.endedAt > 0 ? timeOfDay(c.endedAt) : L"\x2013";
    case 5: {
        if (c.connectedAt <= 0) return L"\x2013";
        return clockText((c.endedAt > 0 ? c.endedAt : serverNow()) - c.connectedAt);
    }
    case 6:
        if (c.start == L"beginning") return L"from the start";
        if (c.start == L"live") return L"at live";
        if (c.start == L"resumed") return L"resumed at " + clockText(c.startSeconds);
        return L"\x2013";
    default: {
        std::wstring s;
        if (c.endedAt > 0) s = endText(c.end);
        else if (c.heard >= 0) {
            if (board.live) {
                double behind = board.serviceSeconds - c.heard;
                s = behind < 4 ? L"\x25CF live" : L"\x25CF " + clockText(behind) + L" behind live";
            } else s = L"\x25CF at " + clockText(c.heard) + L" of " + clockText(board.serviceSeconds);
        } else s = c.connectedAt > 0 ? L"\x25CF on the line" : L"connecting\x2026";
        if (c.livePresses > 0) s += c.livePresses == 1 ? L"  (pressed 1)" : L"  (pressed 1 \x00D7" + std::to_wstring(c.livePresses) + L")";
        return s;
    }
    }
}

void fillCallList() {
    // On the line first, then newest first.
    std::vector<size_t> order;
    for (size_t i = 0; i < board.calls.size(); ++i) order.push_back(i);
    std::sort(order.begin(), order.end(), [](size_t a, size_t b) {
        bool la = onLine(board.calls[a]), lb = onLine(board.calls[b]);
        if (la != lb) return la;
        return board.calls[a].id > board.calls[b].id;
    });
    HWND list = item(IdCalls);
    if (order != rows) {
        rows = order;
        fillingList = true;
        ListView_SetItemCountEx(list, int(rows.size()), LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
        ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        for (size_t r = 0; r < rows.size(); ++r)
            if (selectedCall && board.calls[rows[r]].id == selectedCall) ListView_SetItemState(list, int(r), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        fillingList = false;
    }
    InvalidateRect(list, nullptr, FALSE);
}

// --- drawing ---------------------------------------------------------------------

struct Canvas {
    HDC dc, mem;
    HBITMAP bmp;
    HGDIOBJ old;
    int w, h;
    std::vector<std::pair<COLORREF, HBRUSH>> brushes;
    Canvas(HDC target, int width, int height) : dc(target), w(width), h(height) {
        mem = CreateCompatibleDC(dc);
        bmp = CreateCompatibleBitmap(dc, w, h);
        old = SelectObject(mem, bmp);
    }
    ~Canvas() {
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        for (auto& b : brushes) DeleteObject(b.second);
    }
    void fill(int x0, int y0, int x1, int y1, COLORREF c) {
        if (x1 <= x0 || y1 <= y0) return;
        HBRUSH brush = nullptr;
        for (auto& b : brushes) if (b.first == c) { brush = b.second; break; }
        if (!brush) { brush = CreateSolidBrush(c); brushes.emplace_back(c, brush); }
        RECT f{x0, y0, x1, y1};
        FillRect(mem, &f, brush);
    }
    void text(int x, int y, const std::wstring& s, COLORREF c, UINT align = TA_LEFT) {
        SetTextColor(mem, c);
        SetTextAlign(mem, align | TA_TOP);
        TextOutW(mem, x, y, s.c_str(), int(s.size()));
    }
    void blit(const RECT& r) { BitBlt(dc, r.left, r.top, w, h, mem, 0, 0, SRCCOPY); }
};

void drawMeter(const DRAWITEMSTRUCT* d) {
    RECT r = d->rcItem;
    Canvas cv(d->hDC, r.right - r.left, r.bottom - r.top);
    int w = cv.w, h = cv.h;
    cv.fill(0, 0, w, h, RGB(30, 34, 40));
    auto x = [&](float db) { return int((std::clamp(db, -60.f, 0.f) + 60.f) / 60.f * float(w)); };
    int peakX = x(meterPeak), rmsX = x(meterRms), y18 = x(-18), y6 = x(-6);
    auto bar = [&](int to, int y0, int y1, COLORREF g, COLORREF ye, COLORREF re) {
        cv.fill(0, y0, std::min(to, y18), y1, g);
        if (to > y18) cv.fill(y18, y0, std::min(to, y6), y1, ye);
        if (to > y6) cv.fill(y6, y0, to, y1, re);
    };
    bar(peakX, S(2), h - S(2), RGB(46, 110, 70), RGB(130, 120, 40), RGB(140, 50, 50));
    bar(rmsX, S(4), h - S(4), RGB(70, 190, 110), RGB(225, 200, 60), RGB(230, 70, 70));
    int hold = x(peakHold);
    if (peakHold > -60) cv.fill(std::max(0, hold - S(2)), S(1), hold, h - S(1), RGB(235, 235, 235));
    for (int db = -48; db < 0; db += 6) cv.fill(x(float(db)), h - S(4), x(float(db)) + 1, h, RGB(110, 118, 128));
    cv.blit(r);
}

void drawQr(const DRAWITEMSTRUCT* d) {
    RECT r = d->rcItem;
    Canvas cv(d->hDC, r.right - r.left, r.bottom - r.top);
    cv.fill(0, 0, cv.w, cv.h, GetSysColor(COLOR_BTNFACE));
    if (qr.size == 0) {
        cv.fill(0, 0, cv.w, cv.h, RGB(232, 234, 238));
        SelectObject(cv.mem, smallFont);
        SetBkMode(cv.mem, TRANSPARENT);
        cv.text(cv.w / 2, cv.h / 2 - S(7), L"no link yet", RGB(120, 126, 136), TA_CENTER);
    } else {
        const int quiet = 2;
        int cells = qr.size + 2 * quiet;
        int scale = std::max(1, std::min(cv.w, cv.h) / cells);
        int side = cells * scale, x0 = (cv.w - side) / 2, y0 = (cv.h - side) / 2;
        cv.fill(x0, y0, x0 + side, y0 + side, RGB(255, 255, 255));
        for (int y = 0; y < qr.size; ++y)
            for (int x = 0; x < qr.size; ++x)
                if (qr.dark(x, y)) {
                    int px = x0 + (x + quiet) * scale, py = y0 + (y + quiet) * scale;
                    cv.fill(px, py, px + scale, py + scale, RGB(0, 0, 0));
                }
    }
    cv.blit(r);
}

void drawGraph(const DRAWITEMSTRUCT* d) {
    RECT r = d->rcItem;
    Canvas cv(d->hDC, r.right - r.left, r.bottom - r.top);
    int w = cv.w, h = cv.h;
    cv.fill(0, 0, w, h, RGB(200, 205, 212));
    cv.fill(1, 1, w - 1, h - 1, RGB(252, 252, 253));
    SelectObject(cv.mem, smallFont);
    SetBkMode(cv.mem, TRANSPARENT);
    const COLORREF ink = RGB(90, 98, 110), grid = RGB(232, 235, 240);
    if (board.serviceId.empty()) {
        cv.text(w / 2, h / 2 - S(7), L"No service on the server yet. Start streaming begins one.", ink, TA_CENTER);
        cv.blit(r);
        return;
    }
    double now = serverNow();
    double t0 = board.serviceStart > 0 ? board.serviceStart : now - 60;
    double t1 = now;
    bool anyOnLine = std::any_of(board.calls.begin(), board.calls.end(), onLine);
    if (!board.live && !anyOnLine) {
        t1 = std::max(board.serviceEnd, t0 + board.serviceSeconds);
        for (const CallInfo& c : board.calls) t1 = std::max(t1, std::max(c.at, c.endedAt));
    }
    if (t1 - t0 < 120) t1 = t0 + 120;
    const int x0 = S(64), x1 = w - S(10), top = S(6), axis = S(16), y1 = h - axis;
    auto X = [&](double t) { return x0 + int((t - t0) / (t1 - t0) * double(x1 - x0)); };
    auto T = [&](int x) { return t0 + double(x - x0) / double(x1 - x0) * (t1 - t0); };
    int bandsH = y1 - top;
    int audioTop = top, audioH = bandsH * 26 / 100;
    int listenTop = audioTop + audioH + S(4), listenH = bandsH * 30 / 100;
    int callsTop = listenTop + listenH + S(4), callsH = y1 - callsTop;

    // Time grid and axis.
    static const int steps[] = {60, 120, 300, 600, 900, 1800, 3600, 7200, 10800, 21600};
    int step = steps[0];
    for (int s : steps) { step = s; if (double(s) / (t1 - t0) * (x1 - x0) >= S(70)) break; }
    for (double t = std::ceil(t0 / step) * step; t <= t1; t += step) {
        int x = X(t);
        cv.fill(x, top, x + 1, y1, grid);
        cv.fill(x, y1, x + 1, y1 + S(3), ink);
        cv.text(x, y1 + S(2), timeOfDay(t, false), ink, TA_CENTER);
    }
    cv.fill(x0, y1, x1, y1 + 1, RGB(190, 196, 204));
    cv.text(S(8), audioTop + audioH / 2 - S(7), L"Audio", ink);
    cv.text(S(8), listenTop + listenH / 2 - S(7), L"Listening", ink);
    cv.text(S(8), callsTop + std::min(callsH, S(14)) / 2 - S(4), L"Calls", ink);

    // Audio: peak of each second of the recording.
    for (int x = x0; x < x1; ++x) {
        double a = T(x) - t0, b = T(x + 1) - t0;
        int best = -99;
        for (long i = std::max(0L, long(a)); i < long(std::ceil(b)) && i < long(board.levels.size()); ++i) best = std::max(best, board.levels[size_t(i)]);
        if (best <= -55) continue;
        int bar = int((std::min(best, 0) + 60) / 60.0 * audioH);
        COLORREF c = best > -6 ? RGB(214, 96, 77) : best > -18 ? RGB(110, 150, 200) : RGB(150, 175, 210);
        cv.fill(x, audioTop + audioH - bar, x + 1, audioTop + audioH, c);
    }

    // Listening: callers and browser listeners on the line.
    std::vector<int> count(size_t(std::max(0, x1 - x0)), 0);
    int most = 0;
    for (const CallInfo& c : board.calls) {
        if (c.connectedAt <= 0) continue;
        int a = std::max(x0, X(c.connectedAt)), b = std::min(x1, X(c.endedAt > 0 ? c.endedAt : now) + 1);
        for (int x = a; x < b; ++x) most = std::max(most, ++count[size_t(x - x0)]);
    }
    for (int x = x0; x < x1; ++x) {
        int n = count[size_t(x - x0)];
        if (!n) continue;
        int bar = std::max(S(2), int(double(n) / std::max(most, 1) * listenH));
        cv.fill(x, listenTop + listenH - bar, x + 1, listenTop + listenH, RGB(76, 170, 112));
    }
    cv.fill(x0, listenTop + listenH, x1, listenTop + listenH + 1, grid);
    if (most > 0) cv.text(x0 - S(4), listenTop - S(1), std::to_wstring(most), ink, TA_RIGHT);

    // Calls: one bar per call, packed into lanes; green after pressing 1.
    std::vector<size_t> order;
    for (size_t i = 0; i < board.calls.size(); ++i) order.push_back(i);
    auto startOf = [](const CallInfo& c) { return c.connectedAt > 0 ? c.connectedAt : c.at; };
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return startOf(board.calls[a]) < startOf(board.calls[b]); });
    std::vector<double> laneEnd;
    std::vector<int> laneOf(board.calls.size(), 0);
    double gap = (t1 - t0) * 0.004;
    for (size_t i : order) {
        const CallInfo& c = board.calls[i];
        double s = startOf(c), e = c.endedAt > 0 ? c.endedAt : now;
        size_t lane = 0;
        while (lane < laneEnd.size() && laneEnd[lane] + gap > s) ++lane;
        if (lane == laneEnd.size()) laneEnd.push_back(e); else laneEnd[lane] = e;
        laneOf[i] = int(lane);
    }
    int lanes = std::max<int>(1, int(laneEnd.size()));
    int laneH = std::clamp(callsH / lanes, S(3), S(10));
    for (size_t i : order) {
        const CallInfo& c = board.calls[i];
        int y = callsTop + laneOf[i] * laneH;
        if (y + laneH > y1) continue;
        int ya = y + 1, yb = y + std::max(2, laneH - 1);
        if (c.connectedAt <= 0) {        // refused or never connected
            int x = X(c.at);
            cv.fill(x - S(2), ya, x + S(2), yb, RGB(200, 70, 60));
            continue;
        }
        double e = c.endedAt > 0 ? c.endedAt : now;
        int a = X(c.connectedAt), b = std::max(a + S(2), X(e));
        COLORREF base = c.via == L"phone" ? RGB(58, 116, 210) : c.via == L"browser" ? RGB(146, 98, 196) : RGB(140, 146, 158);
        cv.fill(a, ya, b, yb, base);
        if (c.liveAt > 0) cv.fill(std::max(a, X(c.liveAt)), ya, b, yb, RGB(60, 165, 100));
        if (c.id == selectedCall) {
            cv.fill(a - 1, ya - 1, b + 1, ya, RGB(20, 24, 30));
            cv.fill(a - 1, yb, b + 1, yb + 1, RGB(20, 24, 30));
            cv.fill(a - 1, ya - 1, a, yb + 1, RGB(20, 24, 30));
            cv.fill(b, ya - 1, b + 1, yb + 1, RGB(20, 24, 30));
        }
    }
    if (board.live) {
        int x = X(now);
        for (int y = top; y < y1; y += S(4)) cv.fill(x, y, x + 1, y + S(2), RGB(200, 70, 60));
    }
    cv.blit(r);
}

// --- refresh -------------------------------------------------------------------

void refresh(bool everySecond) {
    float p = capture.peakDb(), rm = capture.rmsDb();
    meterPeak = std::max(p, meterPeak - 1.5f);
    meterRms = std::max(rm, meterRms - 1.5f);
    double nowS = nowSeconds();
    if (p >= peakHold || nowS - peakHoldAt > 1.5) { peakHold = p; peakHoldAt = nowS; }
    InvalidateRect(item(IdMeter), nullptr, FALSE);
    wchar_t text[512];
    double quiet = capture.secondsSinceSound();
    if (quiet > 10) swprintf_s(text, L"silent %.0f s", quiet);
    else swprintf_s(text, L"%.0f dB", std::max(-60.f, meterPeak));
    setText(IdMeterText, text);

    Uplink::Stats s = uplink.read();
    setText(IdStart, s.want ? L"Stop streaming" : L"Start streaming");
    std::wstring state;
    if (s.want) state = s.streaming ? L"\x25CF  LIVE" : s.link == Uplink::Link::Trouble ? L"Reconnecting\x2026" : L"Connecting\x2026";
    else state = s.ending ? L"Ending\x2026" : L"Stopped";
    if (state != L"") setText(IdState, state);
    EnableWindow(item(IdServer), !s.want && !s.ending);

    std::wstring line = s.linkText;
    if (s.link == Uplink::Link::Connected) {
        if (s.rttMs >= 0) line += L"  \x00B7  round trip " + std::to_wstring(s.rttMs) + L" ms";
        if (s.streaming) {
            swprintf_s(text, L"  \x00B7  sending %.0f kbps", s.kbps);
            line += text;
            if (s.backlogSeconds > 0.5) { swprintf_s(text, L" (%.1f s to catch up)", s.backlogSeconds); line += text; }
            if (s.serverPeakDb > -998) { swprintf_s(text, L"  \x00B7  server hears peak %.0f dB", s.serverPeakDb); line += text; }
            else line += L"  \x00B7  server hears silence";
        }
        if (s.otherStreaming) line += L"  \x00B7  another copy of the app is streaming";
    }
    setText(IdLine1, line);

    if (everySecond) updateProcessStats();
    uint64_t v = uplink.boardChanges();
    if (v != boardVersion) {
        boardVersion = uplink.readBoard(board);
        fillCallList();
        InvalidateRect(item(IdGraph), nullptr, FALSE);
        std::wstring url = !board.listenUrl.empty() ? board.listenUrl : board.statusUrl;
        if (url != linkUrl) {
            linkUrl = url;
            qr = QrCode::encode(narrow(url));
            InvalidateRect(item(IdQr), nullptr, FALSE);
        }
        setText(IdLinkLabel, !board.listenUrl.empty() || board.statusUrl.empty() ? L"Test player" : L"Status page");
        setText(IdLink, linkUrl.empty() ? L"\x2013 (shown once connected)" : linkUrl);
    } else if (everySecond) {
        InvalidateRect(item(IdGraph), nullptr, FALSE);
        InvalidateRect(item(IdCalls), nullptr, FALSE);     // lengths and positions move on
    }
    std::wstring service;
    if (board.serviceId.empty()) service = s.link == Uplink::Link::Connected ? L"No service on the server" : L"\x2013";
    else {
        int calls = 0;
        for (const CallInfo& c : board.calls) if (c.via != L"browser" && c.connectedAt > 0) ++calls;
        service = (board.live ? L"Service live, " : L"Service ended, ") + clockText(board.serviceSeconds) + L" recorded  \x00B7  " +
                  std::to_wstring(board.callers) + L" on the phone";
        if (board.browsers) service += L", " + std::to_wstring(board.browsers) + L" in a browser";
        service += L"  \x00B7  " + std::to_wstring(calls) + (calls == 1 ? L" call" : L" calls") + L" so far";
    }
    setText(IdLine2, service);
    swprintf_s(text, L"This app: CPU %.1f%% of one core, %.2f%% of all %d  \x00B7  memory %.1f MB", cpuOneCore, cpuAllCores, processors, memoryMb);
    std::wstring app = text;
    std::wstring others;
    int otherCount = 0;
    for (const AppConnection& a : board.apps)
        if (!a.you) { ++otherCount; if (otherCount <= 2) others += (others.empty() ? L"" : L", ") + a.ip + (a.streaming ? L" streaming" : L""); }
    if (otherCount) app += L"  \x00B7  also open: " + others + (otherCount > 2 ? L" +" + std::to_wstring(otherCount - 2) : L"");
    setText(IdLine3, app);

    if (eventLog().current() != logVersion) {
        std::deque<std::wstring> lines;
        logVersion = eventLog().read(lines);
        HWND list = item(IdLog);
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        for (const auto& l : lines) SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)l.c_str());
        SendMessageW(list, LB_SETTOPINDEX, lines.empty() ? 0 : lines.size() - 1, 0);
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, TRUE);
    }
}

void createControls(HINSTANCE inst) {
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
        return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, window, (HMENU)(INT_PTR)id, inst, nullptr);
    };
    make(L"BUTTON", L"Start streaming", BS_PUSHBUTTON | WS_TABSTOP, IdStart);
    make(L"STATIC", L"Stopped", SS_LEFT | SS_CENTERIMAGE, IdState);
    make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, IdSource);
    make(L"BUTTON", L"\x21BB", BS_PUSHBUTTON | WS_TABSTOP, IdRefresh);
    make(L"STATIC", L"", SS_OWNERDRAW, IdMeter);
    make(L"STATIC", L"", SS_RIGHT, IdMeterText);
    for (int id : {IdLine1, IdLine2, IdLine3}) make(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, id);
    make(L"STATIC", L"Test player", SS_LEFT, IdLinkLabel);
    make(L"STATIC", L"", SS_LEFT | SS_NOTIFY | SS_ENDELLIPSIS | SS_NOPREFIX, IdLink);
    make(L"STATIC", L"Server", SS_LEFT, IdServerLabel);
    make(L"EDIT", settings.server.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, IdServer, WS_EX_CLIENTEDGE);
    make(L"STATIC", L"", SS_OWNERDRAW | SS_NOTIFY, IdQr);
    make(L"STATIC", L"", SS_OWNERDRAW, IdGraph);
    HWND list = make(WC_LISTVIEWW, L"", LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, IdCalls, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    for (int i = 0; i < 8; ++i) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = const_cast<wchar_t*>(Columns[i].title);
        col.cx = S(Columns[i].width);
        ListView_InsertColumn(list, i, &col);
    }
    make(L"LISTBOX", L"", LBS_NOINTEGRALHEIGHT | LBS_NOSEL | WS_VSCROLL, IdLog, WS_EX_CLIENTEDGE);
    make(L"STATIC", L"Open log folder", SS_LEFT | SS_NOTIFY, IdLogs);
    fillSources();
    applyFonts();
}

void sizeWindow(bool first) {
    RECT r{0, 0, S(780), S(640)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    if (first) SetWindowPos(window, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        window = hwnd;
        dpi = GetDpiForWindow(hwnd);
        makeFonts();
        createControls(((CREATESTRUCTW*)lp)->hInstance);
        sizeWindow(true);
        layout();
        startCapture();
        uplink.begin(settings.server, settings.key());
        updateProcessStats();
        SetTimer(hwnd, TimerId, 100, nullptr);
        if (autostart) PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IdStart, BN_CLICKED), 0);
        return 0;
    case WM_SIZE:
        if (item(IdLog)) layout();
        return 0;
    case WM_GETMINMAXINFO: {
        RECT r{0, 0, S(660), S(540)};
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
        ((MINMAXINFO*)lp)->ptMinTrackSize = {r.right - r.left, r.bottom - r.top};
        return 0;
    }
    case WM_TIMER: {
        static int ticks = 0;
        refresh(++ticks % 10 == 0);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == IdStart && code == BN_CLICKED) startStop();
        else if (id == IdRefresh && code == BN_CLICKED) { fillSources(); startCapture(); saveSource(); }
        else if (id == IdSource && code == CBN_SELCHANGE) { startCapture(); saveSource(); }
        else if (id == IdServer && code == EN_KILLFOCUS) commitServer();
        else if (id == IDOK && GetFocus() == item(IdServer)) commitServer();
        else if ((id == IdLink || id == IdQr) && code == STN_CLICKED) openLink();
        else if (id == IdLogs && code == STN_CLICKED) {
            std::wstring dir = logFolder();
            SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
            ShellExecuteW(hwnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR* n = (NMHDR*)lp;
        if (n->idFrom != IdCalls) break;
        if (n->code == LVN_GETDISPINFOW) {
            NMLVDISPINFOW* di = (NMLVDISPINFOW*)lp;
            static std::wstring cell;
            if ((di->item.mask & LVIF_TEXT) && di->item.iItem >= 0 && size_t(di->item.iItem) < rows.size()) {
                cell = callText(board.calls[rows[size_t(di->item.iItem)]], di->item.iSubItem);
                di->item.pszText = const_cast<wchar_t*>(cell.c_str());
            }
            return 0;
        }
        if (n->code == LVN_ITEMCHANGED && !fillingList) {
            NMLISTVIEW* lv = (NMLISTVIEW*)lp;
            if ((lv->uChanged & LVIF_STATE) && (lv->uNewState & LVIS_SELECTED) && lv->iItem >= 0 && size_t(lv->iItem) < rows.size()) {
                selectedCall = board.calls[rows[size_t(lv->iItem)]].id;
                InvalidateRect(item(IdGraph), nullptr, FALSE);
            }
            return 0;
        }
        if (n->code == NM_CUSTOMDRAW) {
            NMLVCUSTOMDRAW* cd = (NMLVCUSTOMDRAW*)lp;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT && cd->nmcd.dwItemSpec < rows.size()) {
                const CallInfo& c = board.calls[rows[cd->nmcd.dwItemSpec]];
                if (onLine(c)) cd->clrText = RGB(20, 120, 60);
                else if (c.connectedAt <= 0 && c.endedAt > 0) cd->clrText = RGB(170, 60, 50);
                else cd->clrText = RGB(70, 76, 86);
                return CDRF_DODEFAULT;
            }
            return CDRF_DODEFAULT;
        }
        break;
    }
    case WM_SETCURSOR:
        if ((((HWND)wp == item(IdLink) || (HWND)wp == item(IdQr)) && !linkUrl.empty()) || (HWND)wp == item(IdLogs)) {
            SetCursor(LoadCursor(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_DRAWITEM:
        if (wp == IdMeter) { drawMeter((const DRAWITEMSTRUCT*)lp); return TRUE; }
        if (wp == IdQr) { drawQr((const DRAWITEMSTRUCT*)lp); return TRUE; }
        if (wp == IdGraph) { drawGraph((const DRAWITEMSTRUCT*)lp); return TRUE; }
        break;
    case WM_CTLCOLORSTATIC: {
        HWND ctl = (HWND)lp;
        COLORREF c = 0;
        bool custom = false;
        if (ctl == item(IdState)) {
            Uplink::Stats s = uplink.read();
            c = s.want ? (s.streaming ? RGB(20, 140, 70) : RGB(190, 120, 0)) : s.ending ? RGB(190, 120, 0) : RGB(110, 110, 110);
            custom = true;
        } else if ((ctl == item(IdLink) && !linkUrl.empty()) || ctl == item(IdLogs)) {
            c = RGB(0, 95, 184);
            custom = true;
        }
        if (custom) {
            SetTextColor((HDC)wp, c);
            SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    }
    case WM_DPICHANGED: {
        dpi = HIWORD(wp);
        makeFonts();
        applyFonts();
        RECT* suggested = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOZORDER);
        layout();
        return 0;
    }
    case WM_CLOSE: {
        Uplink::Stats s = uplink.read();
        if (s.want) {
            if (MessageBoxW(hwnd, L"Streaming is on. Stop streaming (this ends the service) and close?", Title, MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
        }
        SetCursor(LoadCursor(nullptr, IDC_WAIT));
        uplink.shutdown(s.want || s.ending ? 6000 : 0);
        capture.stop();
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, TimerId);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void readCommandLine() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto value = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--server") settings.server = cmdServer = value();
        else if (a == L"--key") {
            settings.keyOverride = stored.keyOverride = narrow(value());
            stored.save();
        } else if (a == L"--autostart") autostart = true;
        else if (a == L"--source") {
            std::wstring v = value();
            if (v == L"tone") settings.source = AudioCapture::TestTone;
            else if (v == L"default") settings.source = AudioCapture::DefaultDevice;
            else for (const OutputDevice& d : listOutputDevices()) if (d.name.find(v) != std::wstring::npos) { settings.source = d.id; break; }
        }
    }
    LocalFree(argv);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\ChurchPhoneStream.Single");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(ClassName, nullptr)) { ShowWindow(other, SW_RESTORE); SetForegroundWindow(other); }
        return 0;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX cc{sizeof(cc), ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&cc);
    processors = std::max<int>(1, int(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
    stored.load();
    settings = stored;
    readCommandLine();
    eventLog().add(std::wstring(L"Church Phone Stream started, server ") + settings.server + (settings.keyOverride.empty() ? L", built-in key" : L", key given with --key"));

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = ClassName;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, ClassName, Title, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, 800, 680, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    capture.stop();
    uplink.shutdown(0);
    eventLog().add(L"Church Phone Stream closed");
    CoUninitialize();
    if (single) CloseHandle(single);
    return 0;
}
