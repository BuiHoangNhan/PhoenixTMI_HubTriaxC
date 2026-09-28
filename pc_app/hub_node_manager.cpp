#define WIN32_LEAN_AND_MEAN
#define _WIN32_IE 0x0600
#define WINVER 0x0601
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <chrono>
#include <cwchar>
#include <cwctype>
#include <deque>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr int ID_COMBO_PORT = 1001;
constexpr int ID_BUTTON_REFRESH_PORTS = 1002;
constexpr int ID_BUTTON_CONNECT = 1003;
constexpr int ID_EDIT_MAC = 1004;
constexpr int ID_BUTTON_ADD = 1005;
constexpr int ID_BUTTON_REMOVE = 1006;
constexpr int ID_BUTTON_LIST = 1007;
constexpr int ID_BUTTON_POLL = 1008;
constexpr int ID_BUTTON_POLL_ALL = 1009;
constexpr int ID_LIST_NODES = 1010;
constexpr int ID_EDIT_STATUS = 1011;
constexpr int ID_HUB_MAC_VALUE = 1012;
constexpr int ID_TITLE = 1013;
constexpr int ID_SUBTITLE = 1014;
constexpr int ID_COMBO_METRIC = 1015;
constexpr int ID_CHART = 1016;
constexpr int ID_STATUS_LABEL = 1017;
constexpr UINT ID_TIMER_SERIAL = 1;
constexpr size_t MAX_CHART_SAMPLES = 3600;
constexpr ULONGLONG CHART_WINDOW_MS = 60000;

constexpr COLORREF COLOR_BG = RGB(19, 19, 22);
constexpr COLORREF COLOR_PANEL = RGB(31, 31, 35);
constexpr COLORREF COLOR_EDIT = RGB(39, 39, 44);
constexpr COLORREF COLOR_ACCENT = RGB(208, 26, 45);
constexpr COLORREF COLOR_ACCENT_DARK = RGB(142, 17, 31);
constexpr COLORREF COLOR_TEXT = RGB(242, 242, 244);
constexpr COLORREF COLOR_MUTED = RGB(172, 172, 180);

HWND g_window = nullptr;
HWND g_portCombo = nullptr;
HWND g_connectButton = nullptr;
HWND g_macEdit = nullptr;
HWND g_nodeList = nullptr;
HWND g_statusEdit = nullptr;
HWND g_hubMacValue = nullptr;
HBRUSH g_backgroundBrush = nullptr;
HBRUSH g_editBrush = nullptr;
HBRUSH g_panelBrush = nullptr;
HFONT g_uiFont = nullptr;
HFONT g_titleFont = nullptr;
std::string g_rxBuffer;
HWND g_metricCombo = nullptr;
HWND g_chart = nullptr;
std::string g_selectedMac;

struct TelemetrySample {
    ULONGLONG timeMs;
    double adc;
    double pressure;
    double setpoint;
    double volumeRaw;
    double volumeScaled;
};

std::unordered_map<std::string, std::deque<TelemetrySample>> g_samples;

ULONGLONG nowMilliseconds() {
    return static_cast<ULONGLONG>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::string jsonValue(const std::string& line, const std::string& key);

double numericValue(const std::string& line, const std::string& key) {
    const std::string value = jsonValue(line, key);
    if (value.empty()) return 0.0;
    char* end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    return end == value.c_str() || !std::isfinite(parsed) ? 0.0 : parsed;
}

class SerialPort {
public:
    ~SerialPort() { close(); }

    bool open(const std::wstring& port) {
        close();
        const std::wstring path = L"\\\\.\\" + port;
        handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) return false;

        DCB dcb{};
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(handle_, &dcb)) { close(); return false; }
        dcb.BaudRate = CBR_115200;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fDtrControl = DTR_CONTROL_DISABLE;
        dcb.fRtsControl = RTS_CONTROL_DISABLE;
        if (!SetCommState(handle_, &dcb)) { close(); return false; }

        COMMTIMEOUTS timeouts{};
        timeouts.ReadIntervalTimeout = MAXDWORD;
        timeouts.WriteTotalTimeoutConstant = 1000;
        if (!SetCommTimeouts(handle_, &timeouts)) { close(); return false; }
        SetupComm(handle_, 4096, 4096);
        PurgeComm(handle_, PURGE_RXCLEAR | PURGE_TXCLEAR);
        return true;
    }

    void close() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    bool isOpen() const { return handle_ != INVALID_HANDLE_VALUE; }

    bool sendLine(const std::string& command) {
        if (!isOpen()) return false;
        const std::string wire = command + "\r\n";
        DWORD written = 0;
        return WriteFile(handle_, wire.data(), static_cast<DWORD>(wire.size()), &written, nullptr) &&
               written == wire.size();
    }

    std::string readAvailable() {
        if (!isOpen()) return {};
        COMSTAT stat{};
        DWORD errors = 0;
        if (!ClearCommError(handle_, &errors, &stat) || stat.cbInQue == 0) return {};
        std::array<char, 512> buffer{};
        DWORD toRead = (std::min)(stat.cbInQue, static_cast<DWORD>(buffer.size()));
        DWORD received = 0;
        if (!ReadFile(handle_, buffer.data(), toRead, &received, nullptr) || received == 0) return {};
        return std::string(buffer.data(), received);
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

SerialPort g_serial;

HWND addControl(const wchar_t* cls, const wchar_t* text, DWORD style,
                int x, int y, int width, int height, int id) {
    if (wcscmp(cls, L"BUTTON") == 0) style |= BS_OWNERDRAW;
    return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                           x, y, width, height, g_window,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           GetModuleHandleW(nullptr), nullptr);
}

void useFont(HWND control, HFONT font = nullptr) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font ? font : g_uiFont), TRUE);
}

void setStatus(const std::wstring& message) {
    SetWindowTextW(g_statusEdit, message.c_str());
}

void appendStatus(const std::string& message) {
    const std::wstring line(message.begin(), message.end());
    const int end = GetWindowTextLengthW(g_statusEdit);
    SendMessageW(g_statusEdit, EM_SETSEL, end, end);
    SendMessageW(g_statusEdit, EM_REPLACESEL, FALSE,
                 reinterpret_cast<LPARAM>((line + L"\r\n").c_str()));
}

std::wstring comboText(HWND combo) {
    const int length = GetWindowTextLengthW(combo);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(combo, &text[0], length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}

void refreshPorts() {
    SendMessageW(g_portCombo, CB_RESETCONTENT, 0, 0);
    for (int i = 1; i <= 64; ++i) {
        std::wstring port = L"COM";
        if (i >= 10) port.push_back(static_cast<wchar_t>(L'0' + i / 10));
        port.push_back(static_cast<wchar_t>(L'0' + i % 10));
        wchar_t target[256];
        if (QueryDosDeviceW(port.c_str(), target, static_cast<DWORD>(std::size(target))))
            SendMessageW(g_portCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(port.c_str()));
    }
    if (SendMessageW(g_portCombo, CB_GETCOUNT, 0, 0) > 0)
        SendMessageW(g_portCombo, CB_SETCURSEL, 0, 0);
}

bool normalizeMac(std::wstring input, std::string& mac) {
    std::wstring digits;
    for (wchar_t c : input) {
        if (c == L':' || c == L'-' || c == L' ') continue;
        if (!iswxdigit(c)) return false;
        digits.push_back(static_cast<wchar_t>(towupper(c)));
    }
    if (digits.size() != 12) return false;
    mac.clear();
    for (size_t i = 0; i < digits.size(); i += 2) {
        if (i != 0) mac.push_back(':');
        mac.push_back(static_cast<char>(digits[i]));
        mac.push_back(static_cast<char>(digits[i + 1]));
    }
    const int firstByte = std::stoi(mac.substr(0, 2), nullptr, 16);
    return firstByte % 2 == 0 && mac != "00:00:00:00:00:00";
}

std::string jsonString(const std::string& line, const std::string& key) {
    const std::string token = "\"" + key + "\":\"";
    size_t start = line.find(token);
    if (start == std::string::npos) return {};
    start += token.size();
    const size_t end = line.find('"', start);
    return end == std::string::npos ? std::string{} : line.substr(start, end - start);
}

std::string jsonValue(const std::string& line, const std::string& key) {
    const std::string token = "\"" + key + "\":";
    size_t start = line.find(token);
    if (start == std::string::npos) return {};
    start += token.size();
    while (start < line.size() && line[start] == ' ') ++start;
    size_t end = line.find_first_of(",}", start);
    if (end == std::string::npos) end = line.size();
    return line.substr(start, end - start);
}

void paintChart(HWND window) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT bounds{};
    GetClientRect(window, &bounds);
    FillRect(dc, &bounds, g_panelBrush);

    const int metric = static_cast<int>(SendMessageW(g_metricCombo, CB_GETCURSEL, 0, 0));
    const wchar_t* metricNames[] = {L"ADC", L"Pressure (kPa)", L"Setpoint (kPa)",
                                    L"Volume raw", L"Volume scaled"};
    const int safeMetric = metric >= 0 && metric < 5 ? metric : 0;
    std::wstring heading = g_selectedMac.empty() ? L"Select a node to plot" :
        std::wstring(g_selectedMac.begin(), g_selectedMac.end()) + L"   /   " + metricNames[safeMetric];
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, COLOR_TEXT);
    TextOutW(dc, 14, 8, heading.c_str(), static_cast<int>(heading.size()));

    RECT plot{62, 34, bounds.right - 18, bounds.bottom - 30};
    if (plot.right <= plot.left + 20 || plot.bottom <= plot.top + 20) {
        EndPaint(window, &paint);
        return;
    }
    HPEN gridPen = CreatePen(PS_SOLID, 1, RGB(65, 65, 72));
    HGDIOBJ oldPen = SelectObject(dc, gridPen);
    for (int i = 0; i <= 4; ++i) {
        const int y = plot.top + (plot.bottom - plot.top) * i / 4;
        MoveToEx(dc, plot.left, y, nullptr);
        LineTo(dc, plot.right, y);
    }
    MoveToEx(dc, plot.left, plot.top, nullptr);
    LineTo(dc, plot.left, plot.bottom);
    LineTo(dc, plot.right, plot.bottom);
    SelectObject(dc, oldPen);
    DeleteObject(gridPen);

    TextOutW(dc, plot.left, plot.bottom + 7, L"60 s ago", 8);
    TextOutW(dc, plot.right - 30, plot.bottom + 7, L"now", 3);

    auto found = g_samples.find(g_selectedMac);
    if (found == g_samples.end() || found->second.empty()) {
        SetTextColor(dc, COLOR_MUTED);
        TextOutW(dc, plot.left + 12, plot.top + 14, L"Waiting for telemetry samples...", 32);
        EndPaint(window, &paint);
        return;
    }

    const ULONGLONG now = nowMilliseconds();
    const ULONGLONG startTime = now > CHART_WINDOW_MS ? now - CHART_WINDOW_MS : 0;
    std::vector<const TelemetrySample*> visible;
    double minimum = 0.0;
    double maximum = 0.0;
    bool first = true;
    for (const auto& sample : found->second) {
        if (sample.timeMs < startTime) continue;
        visible.push_back(&sample);
        const double values[] = {sample.adc, sample.pressure, sample.setpoint,
                                 sample.volumeRaw, sample.volumeScaled};
        const double value = values[safeMetric];
        if (first) { minimum = maximum = value; first = false; }
        else { minimum = (std::min)(minimum, value); maximum = (std::max)(maximum, value); }
    }
    if (visible.empty()) {
        SetTextColor(dc, COLOR_MUTED);
        TextOutW(dc, plot.left + 12, plot.top + 14, L"No samples in the last 60 seconds.", 34);
        EndPaint(window, &paint);
        return;
    }

    if (maximum - minimum < 1e-9) {
        const double padding = (std::abs(maximum) > 1e-6) ? std::abs(maximum) * 0.05 : 1.0;
        minimum -= padding;
        maximum += padding;
    } else {
        const double padding = (maximum - minimum) * 0.08;
        minimum -= padding;
        maximum += padding;
    }
    char number[48];
    for (int i = 0; i <= 4; ++i) {
        const double value = maximum - (maximum - minimum) * i / 4.0;
        std::snprintf(number, sizeof(number), "%.3f", value);
        const std::wstring label(number, number + std::char_traits<char>::length(number));
        const int y = plot.top + (plot.bottom - plot.top) * i / 4 - 7;
        TextOutW(dc, 2, y, label.c_str(), static_cast<int>(label.size()));
    }

    HPEN valuePen = CreatePen(PS_SOLID, 2, COLOR_ACCENT);
    oldPen = SelectObject(dc, valuePen);
    bool started = false;
    int previousX = 0;
    int previousY = 0;
    const ULONGLONG duration = (now - startTime) ? (now - startTime) : 1;
    for (const TelemetrySample* sample : visible) {
        const double values[] = {sample->adc, sample->pressure, sample->setpoint,
                                 sample->volumeRaw, sample->volumeScaled};
        const int x = plot.left + static_cast<int>(
            static_cast<double>(sample->timeMs > startTime ? sample->timeMs - startTime : 0) /
            static_cast<double>(duration) * (plot.right - plot.left));
        const int y = plot.bottom - static_cast<int>((values[safeMetric] - minimum) /
            (maximum - minimum) * (plot.bottom - plot.top));
        if (started) { MoveToEx(dc, previousX, previousY, nullptr); LineTo(dc, x, y); }
        else { Ellipse(dc, x - 2, y - 2, x + 3, y + 3); started = true; }
        previousX = x;
        previousY = y;
    }
    SelectObject(dc, oldPen);
    DeleteObject(valuePen);
    EndPaint(window, &paint);
}

LRESULT CALLBACK chartProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    (void)wParam;
    (void)lParam;
    if (message == WM_PAINT) { paintChart(window); return 0; }
    if (message == WM_ERASEBKGND) return 1;
    return DefWindowProcW(window, message, wParam, lParam);
}

int findRow(const std::string& mac) {
    const int count = ListView_GetItemCount(g_nodeList);
    for (int row = 0; row < count; ++row) {
        wchar_t text[32]{};
        ListView_GetItemText(g_nodeList, row, 0, text, static_cast<int>(std::size(text)));
        if (std::string(text, text + wcslen(text)) == mac) return row;
    }
    return -1;
}

void setCell(int row, int column, const std::string& value) {
    const std::wstring text(value.begin(), value.end());
    ListView_SetItemText(g_nodeList, row, column, const_cast<wchar_t*>(text.c_str()));
}

int ensureRow(const std::string& mac) {
    int row = findRow(mac);
    if (row >= 0) return row;
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = ListView_GetItemCount(g_nodeList);
    std::wstring text(mac.begin(), mac.end());
    item.pszText = &text[0];
    return ListView_InsertItem(g_nodeList, &item);
}

void handleHubLine(const std::string& line) {
    const std::string type = jsonString(line, "type");
    if (type == "node") {
        const std::string mac = jsonString(line, "mac");
        if (!mac.empty()) {
            const int row = ensureRow(mac);
            setCell(row, 1, "Ready");
        }
    } else if (type == "telemetry") {
        const std::string mac = jsonString(line, "mac");
        if (mac.empty()) return;
        const int row = ensureRow(mac);
        setCell(row, 1, jsonValue(line, "uptime_ms") + " ms");
        setCell(row, 2, jsonValue(line, "adc"));
        setCell(row, 3, jsonValue(line, "pressure_kpa") + " kPa");
        setCell(row, 4, jsonValue(line, "setpoint_kpa") + " kPa");
        setCell(row, 5, jsonValue(line, "volume_raw"));
        setCell(row, 6, jsonValue(line, "volume_scaled"));
        const std::string flags = jsonValue(line, "flags");
        const unsigned value = flags.empty() ? 0U : static_cast<unsigned>(std::stoul(flags));
        setCell(row, 7, value == 0 ? "Normal" :
                ((value & 1U) ? "UP " : "") + std::string((value & 2U) ? "DOWN" : ""));
        auto& history = g_samples[mac];
        history.push_back(TelemetrySample{nowMilliseconds(), numericValue(line, "adc"),
            numericValue(line, "pressure_kpa"), numericValue(line, "setpoint_kpa"),
            numericValue(line, "volume_raw"), numericValue(line, "volume_scaled")});
        while (history.size() > MAX_CHART_SAMPLES) history.pop_front();
        if (g_selectedMac.empty()) {
            g_selectedMac = mac;
            ListView_SetItemState(g_nodeList, row, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
        }
        if (g_chart) InvalidateRect(g_chart, nullptr, FALSE);
        setStatus(L"Telemetry updated.");
    } else if (type == "hub") {
        const std::string mac = jsonString(line, "mac");
        const std::wstring macText(mac.begin(), mac.end());
        SetWindowTextW(g_hubMacValue, macText.c_str());
        setStatus(std::wstring(L"Connected. Hub MAC: ") + std::wstring(mac.begin(), mac.end()));
    } else if (type == "ok") {
        const std::string command = jsonString(line, "command");
        if (!command.empty()) appendStatus("Command completed: " + command);
    } else if (type == "timeout") {
        appendStatus("Node did not reply: " + jsonString(line, "mac"));
    } else if (type == "error") {
        appendStatus("Hub error: " + jsonString(line, "message") + " " + jsonString(line, "error"));
    } else if (type == "nodes" && jsonValue(line, "count") == "0") {
        ListView_DeleteAllItems(g_nodeList);
        setStatus(L"No nodes configured.");
    }
}

void processSerial() {
    if (!g_serial.isOpen()) return;
    g_rxBuffer += g_serial.readAvailable();
    size_t newline;
    while ((newline = g_rxBuffer.find('\n')) != std::string::npos) {
        std::string line = g_rxBuffer.substr(0, newline);
        g_rxBuffer.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line.front() == '{') handleHubLine(line);
    }
    if (g_rxBuffer.size() > 4096) g_rxBuffer.clear();
}

bool sendCommand(const std::string& command) {
    if (!g_serial.isOpen()) {
        MessageBoxW(g_window, L"Connect to the hub first.", L"Not connected", MB_ICONWARNING);
        return false;
    }
    if (!g_serial.sendLine(command)) {
        setStatus(L"Failed to send UART command.");
        return false;
    }
    return true;
}

void createListColumns() {
    const wchar_t* titles[] = {L"Node MAC", L"Status / uptime", L"ADC", L"Pressure",
        L"Setpoint", L"Volume raw", L"Volume scaled", L"Limit flags"};
    const int widths[] = {150, 130, 70, 105, 105, 100, 110, 100};
    for (int i = 0; i < static_cast<int>(std::size(titles)); ++i) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<wchar_t*>(titles[i]);
        column.cx = widths[i];
        column.iSubItem = i;
        ListView_InsertColumn(g_nodeList, i, &column);
    }
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        g_window = window;
        HWND title = addControl(L"STATIC", L"PHOENIX TMI", SS_LEFT, 22, 16, 260, 34, ID_TITLE);
        HWND subtitle = addControl(L"STATIC", L"ESP-NOW HUB  /  NODE MANAGER", SS_LEFT, 24, 49, 300, 22, ID_SUBTITLE);
        useFont(title, g_titleFont);
        useFont(subtitle);

        CreateWindowW(L"STATIC", L"Hub COM port", WS_CHILD | WS_VISIBLE,
                      24, 91, 102, 24, window, nullptr, nullptr, nullptr);
        g_portCombo = addControl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 128, 87, 130, 250, ID_COMBO_PORT);
        addControl(L"BUTTON", L"Refresh ports", BS_PUSHBUTTON, 268, 87, 112, 30, ID_BUTTON_REFRESH_PORTS);
        g_connectButton = addControl(L"BUTTON", L"Connect", BS_PUSHBUTTON, 390, 87, 104, 30, ID_BUTTON_CONNECT);

        CreateWindowW(L"STATIC", L"Node MAC", WS_CHILD | WS_VISIBLE,
                      24, 139, 78, 24, window, nullptr, nullptr, nullptr);
        g_macEdit = addControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 104, 135, 190, 28, ID_EDIT_MAC);
        addControl(L"BUTTON", L"Add node", BS_PUSHBUTTON, 304, 134, 92, 32, ID_BUTTON_ADD);
        addControl(L"BUTTON", L"Remove", BS_PUSHBUTTON, 404, 134, 86, 32, ID_BUTTON_REMOVE);
        addControl(L"BUTTON", L"Refresh list", BS_PUSHBUTTON, 498, 134, 102, 32, ID_BUTTON_LIST);
        addControl(L"BUTTON", L"Poll selected", BS_PUSHBUTTON, 608, 134, 112, 32, ID_BUTTON_POLL);
        addControl(L"BUTTON", L"Poll all", BS_PUSHBUTTON, 728, 134, 88, 32, ID_BUTTON_POLL_ALL);

        CreateWindowW(L"STATIC", L"HUB STATION MAC", WS_CHILD | WS_VISIBLE,
                      24, 181, 130, 24, window, nullptr, nullptr, nullptr);
        g_hubMacValue = addControl(L"EDIT", L"Connect to read hub MAC", WS_BORDER | ES_AUTOHSCROLL | ES_READONLY,
                                   160, 177, 205, 28, ID_HUB_MAC_VALUE);

        g_nodeList = addControl(WC_LISTVIEWW, L"", WS_BORDER | LVS_REPORT | LVS_SINGLESEL |
                                LVS_SHOWSELALWAYS, 24, 218, 900, 220, ID_LIST_NODES);
        ListView_SetExtendedListViewStyle(g_nodeList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        ListView_SetBkColor(g_nodeList, COLOR_PANEL);
        ListView_SetTextBkColor(g_nodeList, COLOR_PANEL);
        ListView_SetTextColor(g_nodeList, COLOR_TEXT);
        useFont(g_nodeList);
        createListColumns();

        HWND chartLabel = CreateWindowW(L"STATIC", L"LIVE TREND  /  LAST 60 SECONDS",
                      WS_CHILD | WS_VISIBLE, 24, 452, 275, 24, window, nullptr, nullptr, nullptr);
        useFont(chartLabel);
        CreateWindowW(L"STATIC", L"Signal", WS_CHILD | WS_VISIBLE,
                      310, 452, 48, 24, window, nullptr, nullptr, nullptr);
        g_metricCombo = addControl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                                   360, 448, 190, 180, ID_COMBO_METRIC);
        const wchar_t* metrics[] = {L"ADC", L"Pressure (kPa)", L"Setpoint (kPa)",
                                    L"Volume raw", L"Volume scaled"};
        for (const wchar_t* metric : metrics)
            SendMessageW(g_metricCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(metric));
        SendMessageW(g_metricCombo, CB_SETCURSEL, 0, 0);
        g_chart = CreateWindowExW(0, L"TriaxTelemetryChart", L"",
                    WS_CHILD | WS_VISIBLE | WS_BORDER, 24, 480, 900, 260,
                    window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CHART)),
                    GetModuleHandleW(nullptr), nullptr);
        useFont(g_metricCombo);
        HWND statusLabel = CreateWindowW(L"STATIC", L"STATUS", WS_CHILD | WS_VISIBLE,
                      24, 0, 64, 22, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_STATUS_LABEL)),
                      nullptr, nullptr);
        useFont(statusLabel);
        g_statusEdit = addControl(L"EDIT", L"Select a COM port and connect to the hub.",
                                  WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                                  94, 0, 830, 54, ID_EDIT_STATUS);
        for (HWND child = GetWindow(window, GW_CHILD); child != nullptr; child = GetWindow(child, GW_HWNDNEXT))
            if (child != title && child != subtitle) useFont(child);

        refreshPorts();
        SetTimer(window, ID_TIMER_SERIAL, 50, nullptr);
        return 0;
    }
    case WM_SIZE: {
        const int width = LOWORD(lParam);
        const int height = HIWORD(lParam);
        if (g_nodeList) MoveWindow(g_nodeList, 24, 218, (std::max)(400, width - 48), 220, TRUE);
        if (g_chart) MoveWindow(g_chart, 24, 480, (std::max)(400, width - 48), (std::max)(100, height - 600), TRUE);
        HWND statusLabel = GetDlgItem(window, ID_STATUS_LABEL);
        if (statusLabel) MoveWindow(statusLabel, 24, height - 61, 64, 22, TRUE);
        if (g_statusEdit) MoveWindow(g_statusEdit, 94, height - 66, (std::max)(250, width - 118), 50, TRUE);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        HWND control = reinterpret_cast<HWND>(lParam);
        SetTextColor(dc, control == GetDlgItem(window, ID_SUBTITLE) ? COLOR_ACCENT : COLOR_TEXT);
        SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(g_backgroundBrush);
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, COLOR_TEXT);
        SetBkColor(dc, COLOR_EDIT);
        return reinterpret_cast<LRESULT>(g_editBrush);
    }
    case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, COLOR_TEXT);
        SetBkColor(dc, COLOR_BG);
        return reinterpret_cast<LRESULT>(g_backgroundBrush);
    }
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (item->CtlType == ODT_BUTTON) {
            const bool primary = item->CtlID == ID_BUTTON_CONNECT || item->CtlID == ID_BUTTON_ADD ||
                                 item->CtlID == ID_BUTTON_POLL_ALL;
            COLORREF fill = primary ? COLOR_ACCENT : COLOR_PANEL;
            if ((item->itemState & ODS_SELECTED) != 0 && primary) fill = COLOR_ACCENT_DARK;
            if ((item->itemState & ODS_DISABLED) != 0) fill = RGB(55, 55, 60);
            HBRUSH brush = CreateSolidBrush(fill);
            FillRect(item->hDC, &item->rcItem, brush);
            DeleteObject(brush);
            HPEN pen = CreatePen(PS_SOLID, 1, primary ? COLOR_ACCENT : RGB(65, 65, 72));
            HGDIOBJ oldPen = SelectObject(item->hDC, pen);
            HGDIOBJ oldBrush = SelectObject(item->hDC, GetStockObject(NULL_BRUSH));
            Rectangle(item->hDC, item->rcItem.left, item->rcItem.top,
                      item->rcItem.right, item->rcItem.bottom);
            SelectObject(item->hDC, oldBrush);
            SelectObject(item->hDC, oldPen);
            DeleteObject(pen);
            SetBkMode(item->hDC, TRANSPARENT);
            SetTextColor(item->hDC, COLOR_TEXT);
            wchar_t caption[64]{};
            GetWindowTextW(item->hwndItem, caption, static_cast<int>(std::size(caption)));
            RECT rect = item->rcItem;
            DrawTextW(item->hDC, caption, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return TRUE;
        }
        break;
    }
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<const NMHDR*>(lParam);
        if (header->idFrom == ID_LIST_NODES && header->code == LVN_ITEMCHANGED) {
            const auto* change = reinterpret_cast<const NMLISTVIEW*>(lParam);
            if ((change->uChanged & LVIF_STATE) != 0 && (change->uNewState & LVIS_SELECTED) != 0) {
                wchar_t mac[32]{};
                ListView_GetItemText(g_nodeList, change->iItem, 0, mac, static_cast<int>(std::size(mac)));
                g_selectedMac.assign(mac, mac + wcslen(mac));
                if (g_chart) InvalidateRect(g_chart, nullptr, FALSE);
            }
        }
        if (header->idFrom == ID_LIST_NODES &&
            static_cast<UINT>(header->code) == static_cast<UINT>(NM_CUSTOMDRAW)) {
            auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
            if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return CDRF_NOTIFYSUBITEMDRAW;
            if (draw->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
                const bool selected = (draw->nmcd.uItemState & CDIS_SELECTED) != 0;
                draw->clrText = COLOR_TEXT;
                draw->clrTextBk = selected ? COLOR_ACCENT_DARK : COLOR_PANEL;
                return CDRF_NEWFONT;
            }
        }
        return 0;
    }
    case WM_TIMER:
        if (wParam == ID_TIMER_SERIAL) processSerial();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == ID_COMBO_METRIC && HIWORD(wParam) == CBN_SELCHANGE) {
            if (g_chart) InvalidateRect(g_chart, nullptr, FALSE);
            return 0;
        }
        switch (LOWORD(wParam)) {
        case ID_BUTTON_REFRESH_PORTS:
            refreshPorts();
            setStatus(L"COM port list refreshed.");
            return 0;
        case ID_BUTTON_CONNECT:
            if (g_serial.isOpen()) {
                g_serial.close();
                SetWindowTextW(g_connectButton, L"Connect");
                setStatus(L"Disconnected.");
            } else {
                const std::wstring port = comboText(g_portCombo);
                if (port.empty()) { setStatus(L"No COM port selected."); return 0; }
                if (!g_serial.open(port)) {
                    setStatus(L"Cannot open port. Close Serial Monitor and check the COM port.");
                    return 0;
                }
                SetWindowTextW(g_connectButton, L"Disconnect");
                setStatus(L"Connected. Loading nodes; live telemetry will update automatically...");
                ListView_DeleteAllItems(g_nodeList);
                sendCommand("list");
            }
            return 0;
        case ID_BUTTON_ADD:
        case ID_BUTTON_REMOVE: {
            const std::wstring input = comboText(g_macEdit);
            std::string mac;
            if (!normalizeMac(input, mac)) {
                MessageBoxW(window, L"Enter a valid unicast MAC address, e.g. 24:6F:28:12:34:56.",
                            L"Invalid MAC", MB_ICONWARNING);
                return 0;
            }
            const bool add = LOWORD(wParam) == ID_BUTTON_ADD;
            if (sendCommand(std::string(add ? "add " : "del ") + mac)) sendCommand("list");
            return 0;
        }
        case ID_BUTTON_LIST:
            if (sendCommand("list")) {
                ListView_DeleteAllItems(g_nodeList);
                setStatus(L"Refreshing node list...");
            }
            return 0;
        case ID_BUTTON_POLL: {
            const int row = ListView_GetNextItem(g_nodeList, -1, LVNI_SELECTED);
            if (row < 0) { setStatus(L"Select a node first."); return 0; }
            wchar_t text[32]{};
            ListView_GetItemText(g_nodeList, row, 0, text, static_cast<int>(std::size(text)));
            const std::string mac(text, text + wcslen(text));
            if (sendCommand("poll " + mac)) setStatus(L"Waiting for node telemetry...");
            return 0;
        }
        case ID_BUTTON_POLL_ALL:
            if (sendCommand("poll all")) setStatus(L"Polling configured nodes...");
            return 0;
        }
        return 0;
    case WM_DESTROY:
        KillTimer(window, ID_TIMER_SERIAL);
        g_serial.close();
        if (g_backgroundBrush) DeleteObject(g_backgroundBrush);
        if (g_editBrush) DeleteObject(g_editBrush);
        if (g_panelBrush) DeleteObject(g_panelBrush);
        if (g_uiFont) DeleteObject(g_uiFont);
        if (g_titleFont) DeleteObject(g_titleFont);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&controls);
    g_backgroundBrush = CreateSolidBrush(COLOR_BG);
    g_editBrush = CreateSolidBrush(COLOR_EDIT);
    g_panelBrush = CreateSolidBrush(COLOR_PANEL);
    g_uiFont = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_titleFont = CreateFontW(-25, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    const wchar_t className[] = L"TriaxHubNodeManagerWindow";
    WNDCLASSW wc{};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance;
    wc.lpszClassName = className;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = g_backgroundBrush;
    if (!RegisterClassW(&wc)) return 1;

    WNDCLASSW chartClass{};
    chartClass.lpfnWndProc = chartProc;
    chartClass.hInstance = instance;
    chartClass.lpszClassName = L"TriaxTelemetryChart";
    chartClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    chartClass.hbrBackground = g_panelBrush;
    if (!RegisterClassW(&chartClass)) return 1;

    HWND window = CreateWindowExW(0, className, L"Triax ESP-NOW Hub - Node Manager",
                                  WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                  1100, 900, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) return 1;
    ShowWindow(window, show);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
