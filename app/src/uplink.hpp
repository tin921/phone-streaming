// The connection to the phone server: a WebSocket (WinHTTP, TLS from
// Windows). The app stays connected while it is open, to show the service and
// its callers; streaming is a mode of that connection. Captured audio goes up
// as 16 kHz mono PCM, each packet tagged with its sample position in the
// service. The last ten minutes are kept, so after a dropped connection the
// app reconnects, asks the server how far it got and sends the rest.
// Protocol: server/ingest.js.
#pragma once
#include "common.hpp"
#include "json.hpp"

namespace ps {

struct ServerAddress {
    bool secure = true;
    std::wstring host, path = L"/";
    INTERNET_PORT port = 443;
};

inline bool parseServerAddress(std::wstring url, ServerAddress& out) {
    auto starts = [&](const wchar_t* p) { return _wcsnicmp(url.c_str(), p, wcslen(p)) == 0; };
    if (starts(L"wss://")) { out.secure = true; out.port = 443; url = url.substr(6); }
    else if (starts(L"ws://")) { out.secure = false; out.port = 80; url = url.substr(5); }
    else return false;
    size_t slash = url.find(L'/');
    std::wstring hostPort = url.substr(0, slash);
    out.path = slash == std::wstring::npos ? L"/" : url.substr(slash);
    size_t colon = hostPort.rfind(L':');
    if (colon != std::wstring::npos) {
        int port = _wtoi(hostPort.c_str() + colon + 1);
        if (port <= 0 || port > 65535) return false;
        out.port = INTERNET_PORT(port);
        hostPort = hostPort.substr(0, colon);
    }
    out.host = hostPort;
    return !out.host.empty();
}

inline std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) { if (c == '"' || c == '\\') out += '\\'; out += c; }
    return out;
}

// This PC's clock as seconds since 1970 (UTC).
inline double epochNow() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER t{{ft.dwLowDateTime, ft.dwHighDateTime}};
    return double(t.QuadPart) / 1e7 - 11644473600.0;
}

// "2026-11-15T03:08:25.996Z" -> seconds since 1970; 0 when empty.
inline double parseIsoTime(const std::string& s) {
    int y, mo, d, h, mi;
    double sec;
    if (s.empty() || sscanf_s(s.c_str(), "%d-%d-%dT%d:%d:%lf", &y, &mo, &d, &h, &mi, &sec) != 6) return 0;
    tm t{};
    t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = h; t.tm_min = mi; t.tm_sec = 0;
    return double(_mkgmtime64(&t)) + sec;
}

// abort() may be called from another thread at any time, also while open()
// is still connecting: it closes the handles, which ends a blocked call.
class WebSocketClient {
    HINTERNET session = nullptr, connection = nullptr, request = nullptr, socket = nullptr;
    bool aborted = false;
    std::mutex handles;

    // Keeps a new handle unless abort() came first.
    bool keep(HINTERNET& slot, HINTERNET h, std::wstring& error) {
        std::lock_guard<std::mutex> lock(handles);
        if (aborted) { if (h) WinHttpCloseHandle(h); error = L"cancelled"; return false; }
        if (!h) { error = errorText(GetLastError()); return false; }
        slot = h;
        return true;
    }

public:
    ~WebSocketClient() { abort(); }

    bool open(const ServerAddress& a, std::wstring& error) {
        HINTERNET s = WinHttpOpen(L"ChurchPhoneStream/0.2", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!s) s = WinHttpOpen(L"ChurchPhoneStream/0.2", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!keep(session, s, error)) return false;
        WinHttpSetTimeouts(session, 10000, 10000, 15000, 8000);
        if (!keep(connection, WinHttpConnect(session, a.host.c_str(), a.port, 0), error)) return false;
        if (!keep(request, WinHttpOpenRequest(connection, L"GET", a.path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              a.secure ? WINHTTP_FLAG_SECURE : 0), error)) return false;
        if (!WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) ||
            !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request, nullptr)) {
            DWORD code = GetLastError();
            std::lock_guard<std::mutex> lock(handles);
            error = aborted ? L"cancelled" : errorText(code);
            return false;
        }
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status != 101) {
            error = status == 503 ? L"the server is not running (HTTP 503)" : L"the server answered HTTP " + std::to_wstring(status);
            return false;
        }
        HINTERNET ws = WinHttpWebSocketCompleteUpgrade(request, 0);
        if (!keep(socket, ws, error)) return false;
        std::lock_guard<std::mutex> lock(handles);
        if (request) WinHttpCloseHandle(request);
        request = nullptr;
        return !aborted;
    }

    // Tells the server this side is closing (a close frame), without waiting.
    void sendClose() {
        HINTERNET ws = current();
        if (ws) WinHttpWebSocketShutdown(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
    }

    bool sendText(const std::string& text) {
        HINTERNET ws = current();
        return ws && WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)text.data(), DWORD(text.size())) == NO_ERROR;
    }

    bool sendBinary(const void* data, size_t size) {
        HINTERNET ws = current();
        return ws && WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, (PVOID)data, DWORD(size)) == NO_ERROR;
    }

    // Blocks for the next text message; false when the connection is gone
    // (closeCode is the server's close code, 0 if none).
    bool receive(std::string& message, std::wstring& error, int& closeCode) {
        std::string text;
        std::vector<BYTE> buffer(65536);
        for (;;) {
            HINTERNET ws = current();
            if (!ws) { error = L"closed"; return false; }
            DWORD read = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
            DWORD r = WinHttpWebSocketReceive(ws, buffer.data(), DWORD(buffer.size()), &read, &type);
            if (r != NO_ERROR) { error = r == ERROR_WINHTTP_TIMEOUT ? L"the server stopped answering" : errorText(r); return false; }
            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                USHORT code = 0;
                char reason[128] = {};
                DWORD length = 0;
                WinHttpWebSocketQueryCloseStatus(ws, &code, reason, sizeof(reason) - 1, &length);
                reason[std::min<DWORD>(length, sizeof(reason) - 1)] = 0;
                closeCode = code;
                if (code == 4001) error = L"the server does not accept this app's key";
                else if (code == 4002) error = L"another copy of the app took over streaming";
                else error = L"the server closed the connection (" + std::to_wstring(code) + (reason[0] ? L", " + widen(reason) : L"") + L")";
                return false;
            }
            text.append(reinterpret_cast<char*>(buffer.data()), read);
            if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) { message = text; return true; }
            if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) text.clear();
        }
    }

    // Closes everything; a call blocked in another thread returns.
    void abort() {
        std::lock_guard<std::mutex> lock(handles);
        aborted = true;
        for (HINTERNET* h : {&socket, &request, &connection, &session})
            if (*h) { WinHttpCloseHandle(*h); *h = nullptr; }
    }

private:
    HINTERNET current() { std::lock_guard<std::mutex> lock(handles); return socket; }
};

// One call (or browser listener) as the server reports it. Times are the
// server's clock, seconds since 1970; 0 = not (yet).
struct CallInfo {
    int id = 0;
    std::wstring from, name, ip, via, start, end;
    double at = 0, connectedAt = 0, endedAt = 0, liveAt = 0;
    int livePresses = 0;
    double startSeconds = -1;
    double heard = -1;          // seconds into the recording, while on the line
};

// "15551230001" -> "(555) 123-0001"; other countries as +digits.
inline std::wstring formatPhone(const std::wstring& raw) {
    std::wstring digits;
    for (wchar_t c : raw) if (c >= L'0' && c <= L'9') digits += c;
    if (digits.empty()) return raw.empty() ? L"no caller ID" : raw;
    if (digits.size() == 11 && digits[0] == L'1') digits = digits.substr(1);
    if (digits.size() == 10) return L"(" + digits.substr(0, 3) + L") " + digits.substr(3, 3) + L"-" + digits.substr(6);
    return L"+" + digits;
}

inline std::wstring durationText(double seconds) {
    long s = long(std::max(0.0, seconds));
    wchar_t text[32];
    if (s >= 3600) swprintf_s(text, L"%ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
    else swprintf_s(text, L"%ld:%02ld", s / 60, s % 60);
    return text;
}

// The server's end reasons, as the operator reads them.
inline std::wstring endText(const std::wstring& e) {
    return e == L"no service" ? L"no service to play" : e == L"line full" ? L"refused, line full" : e == L"the end of the recording" ? L"heard to the end"
         : e == L"the recording was replaced" ? L"a new service started" : e == L"no audio stream" ? L"never connected" : e;
}

struct AppConnection {
    std::wstring ip;
    bool streaming = false, you = false;
};

// What the server says about the service and who listens.
struct Board {
    std::wstring serviceId;
    double serviceStart = 0, serviceEnd = 0, serviceSeconds = 0;
    bool live = false;
    std::vector<CallInfo> calls;      // oldest first
    std::vector<int> levels;          // peak dBFS of each second of the recording, -99 = silence
    int callers = 0, browsers = 0;
    std::vector<AppConnection> apps;
    double clockOffset = 0;           // server clock minus this PC's clock, seconds
    std::wstring statusUrl, listenUrl;
};

class Uplink {
public:
    static constexpr int Rate = 16000;
    static constexpr size_t Keep = size_t(Rate) * 600;     // ten minutes for resending
    enum class Link { Connecting, Connected, Trouble };
    struct Stats {
        Link link = Link::Connecting;
        std::wstring linkText = L"Connecting\x2026";
        std::wstring host;
        bool want = false;        // Start pressed (and not Stop)
        bool streaming = false;   // audio is going up now
        bool ending = false;      // Stop pressed; the server has not confirmed yet
        bool otherStreaming = false;
        double kbps = 0, backlogSeconds = 0;
        int rttMs = -1;
        double serverPeakDb = -999;
    };

    ~Uplink() { shutdown(0); }

    // Connects and stays connected until shutdown().
    void begin(const std::wstring& url, const std::string& key) {
        {
            std::lock_guard<std::mutex> lock(m);
            address = url;
            token = key;
        }
        quit = false;
        worker = std::thread([this] { run(); });
    }

    // Another server address: reconnects there (not while streaming).
    void setServer(const std::wstring& url) {
        std::lock_guard<std::mutex> lock(m);
        if (url == address) return;
        address = url;
        reconnect = true;
        board = Board{};
        ++boardVersion;
        if (client) client->abort();
    }

    // Begins a new service: audio from now on, the previous recording replaced.
    void startStreaming() {
        std::lock_guard<std::mutex> lock(m);
        if (want) return;
        ring.assign(Keep, 0);
        written = serviceBase = nextToSend = 0;
        serviceId.clear();        // a Stop not yet delivered is overtaken: 'start' ends that service
        want = true;
        stats.ending = false;
        eventLog().add(L"Streaming started");
    }

    // Ends the service (the server keeps its recording for callers).
    void stopStreaming() {
        std::lock_guard<std::mutex> lock(m);
        if (!want) return;
        want = false;
        stats.ending = !serviceId.empty();
        if (!stats.ending) eventLog().add(L"Streaming stopped");
    }

    bool wantsStream() { std::lock_guard<std::mutex> lock(m); return want; }

    // Closes the connection; when streaming, ends the service first (waits up to waitMs).
    void shutdown(int waitMs) {
        stopStreaming();
        double until = nowSeconds() + waitMs / 1000.0;
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(m);
                if (serviceId.empty() || stats.link != Link::Connected) break;
            }
            if (nowSeconds() >= until) break;
            Sleep(20);
        }
        {
            std::lock_guard<std::mutex> lock(m);
            if (!serviceId.empty() && waitMs > 0) eventLog().add(L"Could not tell the server to end the service; it ends by itself after 3 hours without audio");
            quit = true;
        }
        cv.notify_all();
        // A moment for the connection to say goodbye, then it is cut.
        for (int i = 0; i < 25 && worker.joinable() && !finished; ++i) Sleep(20);
        {
            std::lock_guard<std::mutex> lock(m);
            if (client) client->abort();
        }
        if (worker.joinable()) worker.join();
    }

    void push(const int16_t* samples, size_t n) {
        std::lock_guard<std::mutex> lock(m);
        if (!want || ring.empty()) return;
        for (size_t i = 0; i < n; ++i) ring[size_t((written + i) % Keep)] = samples[i];
        written += n;
    }

    Stats read() {
        std::lock_guard<std::mutex> lock(m);
        Stats s = stats;
        s.want = want;
        s.backlogSeconds = want ? double(written - std::min(written, nextToSend)) / Rate : 0;
        return s;
    }

    uint64_t boardChanges() { std::lock_guard<std::mutex> lock(m); return boardVersion; }
    uint64_t readBoard(Board& out) { std::lock_guard<std::mutex> lock(m); out = board; return boardVersion; }

private:
    std::mutex m;
    std::condition_variable cv;
    std::thread worker;
    std::atomic<bool> quit{false}, finished{false};
    std::wstring address;
    std::string token;
    bool reconnect = false;
    std::vector<int16_t> ring;
    uint64_t written = 0, serviceBase = 0, nextToSend = 0;
    std::string serviceId;        // the service this app streams (or must still stop)
    bool want = false;
    Stats stats;
    Board board;
    uint64_t boardVersion = 0;
    WebSocketClient* client = nullptr;
    // From the receiving thread:
    bool welcomed = false, gotService = false, gotNoService = false, gotStopped = false, dead = false;
    double serviceInputSamples = 0, lastMessage = 0;
    std::string replyServiceId;
    std::wstring deadReason;
    int closeCode = 0;
    uint64_t bytesSent = 0;

    void setLink(Link l, const std::wstring& text) {
        std::lock_guard<std::mutex> lock(m);
        stats.link = l;
        stats.linkText = text;
        if (l != Link::Connected) { stats.streaming = false; stats.rttMs = -1; stats.kbps = 0; }
    }

    static CallInfo readCall(const Json& c) {
        CallInfo k;
        k.id = int(c["id"].num());
        k.from = widen(c["from"].str());
        k.name = widen(c["name"].str());
        k.ip = widen(c["ip"].str());
        k.via = widen(c["via"].str());
        k.start = widen(c["start"].str());
        k.end = widen(c["end"].str());
        k.at = c["at"].num() / 1000;
        k.connectedAt = c["connectedAt"].num() / 1000;
        k.endedAt = c["endedAt"].num() / 1000;
        k.liveAt = c["liveAt"].num() / 1000;
        k.livePresses = int(c["livePresses"].num());
        k.startSeconds = c["startSeconds"].num(-1);
        return k;
    }

    // Calls into the log (and so the log file) as they connect, press 1 and end.
    static void logCall(const CallInfo* before, const CallInfo& c) {
        std::wstring who = c.via == L"browser" ? L"Browser listener" : (c.via == L"test" ? L"Test call from " : L"Call from ") + formatPhone(c.from);
        if (!c.name.empty()) who += L" " + c.name;
        if (!c.ip.empty()) who += L" (" + c.ip + L")";
        bool wasConnected = before && before->connectedAt > 0, wasEnded = before && before->endedAt > 0;
        if (c.connectedAt > 0 && !wasConnected) {
            std::wstring from = c.start == L"beginning" ? L"from the start" : c.start == L"live" ? L"at live" : L"resumed at " + durationText(c.startSeconds);
            eventLog().add(who + L": listening " + from);
        }
        if (before && c.livePresses > before->livePresses) eventLog().add(who + L": pressed 1, now live");
        if (c.endedAt > 0 && !wasEnded) {
            std::wstring how = c.end == L"hung up" ? L"hung up" : endText(c.end);
            if (c.connectedAt > 0) how += L" after " + durationText(c.endedAt - c.connectedAt);
            eventLog().add(who + L": " + how);
        }
    }

    void readService(const Json& s) {
        if (s.isNull()) { board.serviceId.clear(); board.serviceStart = board.serviceEnd = board.serviceSeconds = 0; board.live = false; return; }
        board.serviceId = widen(s["id"].str());
        board.serviceStart = parseIsoTime(s["startedAt"].str());
        board.serviceEnd = parseIsoTime(s["endedAt"].str());
        board.serviceSeconds = s["seconds"].num();
        board.live = board.serviceEnd == 0;
    }

    void handle(const std::string& text) {
        Json msg = Json::parse(text);
        const std::string& type = msg["type"].str();
        std::lock_guard<std::mutex> lock(m);
        lastMessage = nowSeconds();
        if (type == "welcome") {
            welcomed = true;
            board.statusUrl = widen(msg["links"]["status"].str());
            board.listenUrl = widen(msg["links"]["listen"].str());
            readService(msg["service"]);
            ++boardVersion;
        } else if (type == "service") {
            gotService = true;
            replyServiceId = msg["service"]["id"].str();
            serviceInputSamples = msg["service"]["inputSamples"].num();
            readService(msg["service"]);
            ++boardVersion;
        } else if (type == "no-service") gotNoService = true;
        else if (type == "stopped") { gotStopped = true; readService(msg["service"]); ++boardVersion; }
        else if (type == "pong") stats.rttMs = int((nowSeconds() - msg["t"].num()) * 1000 + 0.5);
        else if (type == "calls") {
            board.calls.clear();
            const Json& list = msg["calls"];
            for (size_t i = 0; i < list.size(); ++i) board.calls.push_back(readCall(list[i]));
            const Json& lv = msg["levels"];
            board.levels.assign(lv.size(), -99);
            for (size_t i = 0; i < lv.size(); ++i) board.levels[i] = int(lv[i].num(-99));
            ++boardVersion;
        } else if (type == "call") {
            CallInfo k = readCall(msg["call"]);
            auto it = std::find_if(board.calls.begin(), board.calls.end(), [&](const CallInfo& c) { return c.id == k.id; });
            logCall(it != board.calls.end() ? &*it : nullptr, k);
            if (it != board.calls.end()) { k.heard = it->heard; *it = k; }
            else board.calls.push_back(k);
            ++boardVersion;
        } else if (type == "status") {
            board.clockOffset = msg["now"].num() / 1000 - epochNow();
            readService(msg["service"]);
            board.callers = int(msg["callers"].num());
            board.browsers = int(msg["browsers"].num());
            for (CallInfo& c : board.calls) c.heard = -1;
            const Json& pos = msg["positions"];
            for (size_t i = 0; i < pos.size(); ++i) {
                int id = int(pos[i]["id"].num());
                for (CallInfo& c : board.calls) if (c.id == id) c.heard = pos[i]["heard"].num(-1);
            }
            board.apps.clear();
            const Json& apps = msg["apps"];
            for (size_t i = 0; i < apps.size(); ++i)
                board.apps.push_back({widen(apps[i]["ip"].str()), apps[i]["streaming"].truthy(), apps[i]["you"].truthy()});
            const Json& lv = msg["levels"];
            size_t from = size_t(std::max(0.0, msg["levelsFrom"].num()));
            if (from <= board.levels.size()) {
                board.levels.resize(from);
                for (size_t i = 0; i < lv.size(); ++i) board.levels.push_back(int(lv[i].num(-99)));
            }
            stats.otherStreaming = msg["otherStreaming"].truthy();
            stats.serverPeakDb = msg["level"].isNull() ? -999 : msg["level"]["peakDb"].num(-999);
            ++boardVersion;
        } else if (type == "error") eventLog().add(L"Server: " + widen(msg["message"].str()));
        cv.notify_all();
    }

    template <class Pred> bool waitFor(Pred pred, double seconds) {
        std::unique_lock<std::mutex> lock(m);
        return cv.wait_for(lock, std::chrono::milliseconds(int(seconds * 1000)), [&] { return pred() || dead || quit; }) && !dead && !quit;
    }

    void waitBackoff(double seconds, const std::wstring& why) {
        for (double left = seconds; left > 0 && !quit; left -= 0.1) {
            {
                std::lock_guard<std::mutex> lock(m);
                if (reconnect) return;
            }
            wchar_t text[400];
            swprintf_s(text, L"%s; retrying in %.0f s", why.c_str(), std::ceil(left));
            setLink(Link::Trouble, text);
            Sleep(100);
        }
    }

    void run() {
        double backoff = 1;
        std::wstring lastProblem;
        while (!quit) {
            std::wstring url;
            {
                std::lock_guard<std::mutex> lock(m);
                url = address;
                reconnect = false;
            }
            ServerAddress server;
            if (!parseServerAddress(url, server)) {
                waitBackoff(2, L"The server address must start with wss:// (or ws:// for a test server)");
                continue;
            }
            { std::lock_guard<std::mutex> lock(m); stats.host = server.host; }
            setLink(Link::Connecting, L"Connecting to " + server.host + L"\x2026");
            WebSocketClient ws;
            {
                std::lock_guard<std::mutex> lock(m);
                if (quit) break;
                client = &ws;           // so that shutdown() or setServer() can cut a slow connect
            }
            std::wstring error;
            if (!ws.open(server, error)) {
                bool again;
                {
                    std::lock_guard<std::mutex> lock(m);
                    client = nullptr;
                    again = reconnect;
                }
                if (quit) break;
                if (again) continue;
                if (error != lastProblem) eventLog().add(L"Cannot connect to " + server.host + L": " + error);
                lastProblem = error;
                waitBackoff(backoff, L"Cannot connect: " + error);
                backoff = std::min(backoff * 2, 15.0);
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(m);
                welcomed = gotService = gotNoService = gotStopped = dead = false;
                closeCode = 0;
                lastMessage = nowSeconds();
                deadReason.clear();
            }
            std::thread receiver([&] {
                std::string msg;
                std::wstring why;
                int code = 0;
                while (ws.receive(msg, why, code)) handle(msg);
                std::lock_guard<std::mutex> lock(m);
                dead = true;
                closeCode = code;
                if (deadReason.empty()) deadReason = why;
                cv.notify_all();
            });
            bool wasUp = session(ws, server, backoff, lastProblem);
            ws.abort();
            receiver.join();
            std::wstring why;
            bool again;
            {
                std::lock_guard<std::mutex> lock(m);
                client = nullptr;
                why = deadReason.empty() ? L"connection lost" : deadReason;
                again = reconnect;
                if (closeCode == 4002 && want) {
                    // Another copy streams now: this one steps back to watching
                    // (reattaching would take the stream back and forth).
                    want = false;
                    serviceId.clear();
                    stats.ending = false;
                    eventLog().add(L"Another copy of the app took over streaming; this one stopped streaming and keeps watching");
                }
            }
            if (quit) break;
            if (again) continue;
            if (wasUp) eventLog().add(L"Connection lost: " + why);
            else if (why != lastProblem) eventLog().add(L"Cannot connect to " + server.host + L": " + why);
            lastProblem = wasUp ? L"" : why;
            if (closeCode == 4002) backoff = 1;
            waitBackoff(backoff, (wasUp ? L"Connection lost: " : L"Cannot connect: ") + why);
            backoff = std::min(backoff * 2, 15.0);
        }
        setLink(Link::Trouble, L"Closed");
        finished = true;
    }

    // Starts a new service, or carries on with this app's service after a reconnect.
    bool attachOrStart(WebSocketClient& ws) {
        std::string id;
        {
            std::lock_guard<std::mutex> lock(m);
            id = serviceId;
            gotService = gotNoService = false;
        }
        if (id.empty()) ws.sendText("{\"type\":\"start\"}");
        else {
            ws.sendText("{\"type\":\"attach\",\"id\":\"" + jsonEscape(id) + "\"}");
            if (!waitFor([&] { return gotService || gotNoService; }, 6)) return false;
            std::lock_guard<std::mutex> lock(m);
            if (gotNoService) {
                eventLog().add(L"The server no longer has this service: starting a new one");
                serviceBase = written;
                gotService = false;
                ws.sendText("{\"type\":\"start\"}");
            }
        }
        if (!waitFor([&] { return gotService; }, 6)) return false;
        std::lock_guard<std::mutex> lock(m);
        bool fresh = serviceId != replyServiceId;
        serviceId = replyServiceId;
        uint64_t oldest = written > Keep ? written - Keep : 0;
        nextToSend = std::clamp<uint64_t>(serviceBase + uint64_t(serviceInputSamples), oldest, written);
        if (fresh) eventLog().add(L"Service started on the server: " + widen(serviceId));
        else eventLog().add(L"Reconnected; resending " + std::to_wstring((written - nextToSend) / Rate) + L" s");
        stats.streaming = true;
        return true;
    }

    // One connection; true if the server welcomed this app.
    bool session(WebSocketClient& ws, const ServerAddress& server, double& backoff, std::wstring& lastProblem) {
        std::string key;
        { std::lock_guard<std::mutex> lock(m); key = token; }
        ws.sendText("{\"type\":\"hello\",\"token\":\"" + jsonEscape(key) + "\",\"client\":\"ChurchPhoneStream/0.2\",\"format\":\"s16le\",\"rate\":16000,\"channels\":1}");
        if (!waitFor([&] { return welcomed; }, 6)) return false;
        backoff = 1;
        if (!lastProblem.empty()) eventLog().add(L"Connected to " + server.host);
        lastProblem.clear();
        setLink(Link::Connected, L"Connected to " + server.host);
        bool attached = false;
        std::vector<char> packet(8 + 3200 * 2);
        double lastPing = 0, lastRate = nowSeconds();
        uint64_t sentAtLastRate = bytesSent;
        for (;;) {
            bool wanted, gone, again;
            std::string mine;
            double heard;
            {
                std::lock_guard<std::mutex> lock(m);
                wanted = want; gone = dead; again = reconnect; heard = lastMessage; mine = serviceId;
            }
            if (quit) { ws.sendClose(); return true; }
            if (gone || again) return true;
            double now = nowSeconds();
            if (now - heard > 8) {
                std::lock_guard<std::mutex> lock(m);
                deadReason = L"no answer from the server for 8 s";
                return true;
            }
            if (!wanted && !mine.empty()) {
                // Stop: end the service (also when Stop was pressed while disconnected).
                { std::lock_guard<std::mutex> lock(m); gotStopped = false; stats.streaming = false; }
                ws.sendText("{\"type\":\"stop\",\"id\":\"" + jsonEscape(mine) + "\"}");
                if (!waitFor([&] { return gotStopped; }, 4)) {
                    std::lock_guard<std::mutex> lock(m);
                    if (deadReason.empty()) deadReason = L"the server did not confirm the end of the service";
                    return true;
                }
                {
                    std::lock_guard<std::mutex> lock(m);
                    if (serviceId == mine) serviceId.clear();
                    stats.ending = false;
                }
                eventLog().add(L"Service ended");
                attached = false;
                continue;
            }
            if (wanted && !attached) {
                if (!attachOrStart(ws)) {
                    std::lock_guard<std::mutex> lock(m);
                    if (deadReason.empty()) deadReason = L"the server did not answer Start";
                    return true;
                }
                attached = true;
            }
            if (!wanted) attached = false;
            // Send what has been captured, 200 ms per packet.
            for (int packets = 0; attached && packets < 25; ++packets) {
                size_t n;
                uint64_t index;
                {
                    std::lock_guard<std::mutex> lock(m);
                    if (!want) break;
                    n = size_t(std::min<uint64_t>(written - nextToSend, 3200));
                    index = nextToSend - serviceBase;
                    for (size_t i = 0; i < n; ++i) {
                        int16_t v = ring[size_t((nextToSend + i) % Keep)];
                        memcpy(&packet[8 + i * 2], &v, 2);
                    }
                }
                if (!n) break;
                memcpy(packet.data(), &index, 8);
                if (!ws.sendBinary(packet.data(), 8 + n * 2)) {
                    std::lock_guard<std::mutex> lock(m);
                    if (deadReason.empty()) deadReason = L"sending failed";
                    return true;
                }
                std::lock_guard<std::mutex> lock(m);
                nextToSend += n;
                bytesSent += 8 + n * 2;
            }
            if (now - lastPing > 5) {
                lastPing = now;
                char ping[80];
                sprintf_s(ping, "{\"type\":\"ping\",\"t\":%.6f}", now);
                ws.sendText(ping);
            }
            if (now - lastRate >= 1) {
                std::lock_guard<std::mutex> lock(m);
                stats.kbps = double(bytesSent - sentAtLastRate) * 8 / 1000 / (now - lastRate);
                sentAtLastRate = bytesSent;
                lastRate = now;
            }
            Sleep(40);
        }
    }
};

} // namespace ps
