#include "pch.h"
#include "StatisticsPage.h"
#include <ws2tcpip.h>
#include <sstream>
#include <algorithm>
#pragma comment(lib, "Ws2_32.lib")

namespace
{
    CString StatisticsConfigPath()
    {
        TCHAR path[MAX_PATH] = {};
        ::GetModuleFileName(nullptr, path, MAX_PATH);
        CString base(path);
        int last = base.ReverseFind(_T('\\'));
        return (last >= 0 ? base.Left(last+1) : CString()) + _T("client_config.ini");
    }
    bool ReadField(const std::string& json, const char* key, std::string& value)
    {
        std::string search = std::string("\"") + key + "\":";
        size_t start = json.find(search);
        if (start == std::string::npos) return false;
        start += search.size();
        while (start < json.size() && json[start] == ' ') ++start;
        bool quoted = start < json.size() && json[start] == '"';
        if (quoted) ++start;
        size_t end = start;
        while (end < json.size() && json[end] != (quoted ? '"' : ',') && json[end] != '}') ++end;
        value = json.substr(start, end-start);
        return true;
    }
    template <size_t N>
    bool ReadCsvInts(const std::string& json, const char* key, std::array<int, N>& target)
    {
        std::string raw;
        if (!ReadField(json, key, raw)) return false;
        std::istringstream stream(raw);
        for (size_t i = 0; i < N; ++i)
        {
            std::string item;
            if (!std::getline(stream, item, ',')) return false;
            try { target[i] = std::stoi(item); } catch (...) { return false; }
        }
        return true;
    }
    bool FetchStatisticsJson(const CString& start, const CString& end, std::string& response)
    {
        // Strictly validate user date fields before inserting them in JSON.
        auto validDate = [](const CString& x) {
            if (x.GetLength() != 10) return false;
            for (int i = 0; i < 10; ++i)
                if (i == 4 || i == 7 ? x[i] != _T('-') : (x[i] < _T('0') || x[i] > _T('9'))) return false;
            return true;
        };
        if (!validDate(start) || !validDate(end)) return false;
        WSADATA wsa = {};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        SOCKET sock = INVALID_SOCKET;
        bool ok = false;
        do
        {
            CString config = StatisticsConfigPath();
            TCHAR ip[128] = {};
            GetPrivateProfileString(_T("SERVER"), _T("IP"), _T("192.168.0.104"), ip,
                _countof(ip), config);
            UINT port = GetPrivateProfileInt(_T("SERVER"), _T("PORT"), 5000, config);
            sockaddr_in address = {};
            address.sin_family = AF_INET;
            address.sin_port = htons(static_cast<u_short>(port));
            CStringA asciiIp(ip);
            if (inet_pton(AF_INET, asciiIp.GetString(), &address.sin_addr) != 1) break;
            sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (sock == INVALID_SOCKET) break;
            DWORD timeout = 1400;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
            u_long mode = 1;
            ioctlsocket(sock, FIONBIO, &mode);
            int status = connect(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address));
            if (status == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) break;
            fd_set fds;
            FD_ZERO(&fds); FD_SET(sock, &fds);
            timeval wait = {1, 0};
            if (select(0, nullptr, &fds, nullptr, &wait) <= 0) break;
            int error = 0; int len = sizeof(error);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &len);
            if (error != 0) break;
            mode = 0;
            ioctlsocket(sock, FIONBIO, &mode);
            CStringA begin(start), finish(end);
            std::string request = "{\"type\":\"STATISTICS_RANGE\",\"start_date\":\""
                + std::string(begin.GetString()) + "\",\"end_date\":\"" + finish.GetString() + "\"}\n";
            if (send(sock, request.data(), static_cast<int>(request.size()), 0) != static_cast<int>(request.size())) break;
            response.clear();
            char buffer[4096];
            while (response.size() < 100000)
            {
                int n = recv(sock, buffer, sizeof(buffer), 0);
                if (n <= 0) break;
                response.append(buffer, static_cast<size_t>(n));
                size_t pos = response.find('\n');
                if (pos != std::string::npos) { response.resize(pos); ok = true; break; }
            }
        } while (false);
        if (sock != INVALID_SOCKET) { shutdown(sock, SD_BOTH); closesocket(sock); }
        WSACleanup();
        return ok;
    }
    constexpr UINT SearchButtonId = 3401;

    const TCHAR* HourLabels[8] =
    {
        _T("09"), _T("10"), _T("11"), _T("12"),
        _T("13"), _T("14"), _T("15"), _T("16")
    };
}

BEGIN_MESSAGE_MAP(CStatisticsPage, CBasePage)
    ON_BN_CLICKED(SearchButtonId, &CStatisticsPage::OnSearch)
    ON_WM_PAINT()
END_MESSAGE_MAP()

BOOL CStatisticsPage::CreatePage(
    CWnd* p,
    const CRect& r,
    UINT id)
{
    if (!CBasePage::CreatePage(p, r, id))
        return FALSE;

    AddTitle(_T("생산 통계 / 그래프"));

    AddLabel(_T("조회기간"), 28, 82, 74, 28);

    m_startDateEdit = AddEdit(105, 77, 150, 32);
    AddLabel(_T("~"), 268, 82, 20, 28);
    m_endDateEdit = AddEdit(292, 77, 150, 32);

    m_searchButton = AddButton(
        _T("조회"),
        458, 75, 88, 36,
        SearchButtonId);

    AddLabel(
        _T("※ 실제 카메라 검사 완료 건만 집계 · 과거 테스트 데이터 제외"),
        566, 82, 380, 28);

    // 기본 조회 기간: 최근 7일
    const CTime now = CTime::GetCurrentTime();
    const CTime start = now - CTimeSpan(6, 0, 0, 0);

    m_startDateEdit->SetWindowText(start.Format(_T("%Y-%m-%d")));
    m_endDateEdit->SetWindowText(now.Format(_T("%Y-%m-%d")));

    // 요약 카드
    m_totalCard.CreateCard(
        _T("총 검사 수\r\n0"),
        this,
        CRect(28, 126, 244, 214));

    m_passCard.CreateCard(
        _T("정상\r\n0"),
        this,
        CRect(260, 126, 476, 214));

    m_failCard.CreateCard(
        _T("불량\r\n0"),
        this,
        CRect(492, 126, 708, 214));

    m_rateCard.CreateCard(
        _T("불량률\r\n0.0 %"),
        this,
        CRect(724, 126, 940, 214));

    ClearRealData();
    UpdateSummary();

    return TRUE;
}

void CStatisticsPage::ClearRealData()
{
    m_hourlyTotal.fill(0); m_hourlyNg.fill(0);
    m_dailyTotal.fill(0); m_dailyNg.fill(0);
    m_total = m_pass = m_fail = 0;
}

void CStatisticsPage::RefreshFromServer(bool showError)
{
    CString start, end;
    if (m_startDateEdit) m_startDateEdit->GetWindowText(start);
    if (m_endDateEdit) m_endDateEdit->GetWindowText(end);
    start.Trim(); end.Trim();
    std::string json, status;
    std::array<int, 8> hourlyTotal{}, hourlyNg{};
    std::array<int, 7> dailyTotal{}, dailyNg{};
    std::string total, passed, failed;
    bool ok = FetchStatisticsJson(start, end, json) &&
        ReadField(json, "status", status) && status == "OK" &&
        ReadField(json, "total", total) && ReadField(json, "passed", passed) &&
        ReadField(json, "failed", failed) &&
        ReadCsvInts(json, "hourly_total", hourlyTotal) &&
        ReadCsvInts(json, "hourly_ng", hourlyNg) &&
        ReadCsvInts(json, "daily_total", dailyTotal) &&
        ReadCsvInts(json, "daily_ng", dailyNg);
    if (!ok)
    {
        ClearRealData();
        m_rangeText = _T("서버 통계 미연결 · 표시할 실검사 데이터 없음");
        UpdateSummary();
        if (showError) AfxMessageBox(_T("실검사 통계 조회 실패: 서버 연결과 날짜를 확인해주세요."), MB_ICONWARNING);
        return;
    }
    try { m_total = std::stoi(total); m_pass = std::stoi(passed); m_fail = std::stoi(failed); }
    catch (...) { ClearRealData(); UpdateSummary(); return; }
    m_hourlyTotal = hourlyTotal; m_hourlyNg = hourlyNg;
    m_dailyTotal = dailyTotal; m_dailyNg = dailyNg;
    m_rangeText.Format(_T("실검사 기간 %s ~ %s"), start.GetString(), end.GetString());
    UpdateSummary();
}

void CStatisticsPage::UpdateSummary()
{
    CString text;

    text.Format(_T("총 검사 수\r\n%d"), m_total);
    m_totalCard.SetText(text);

    text.Format(_T("정상\r\n%d"), m_pass);
    m_passCard.SetText(text);

    text.Format(_T("불량\r\n%d"), m_fail);
    m_failCard.SetText(text);

    const double failRate =
        m_total == 0
        ? 0.0
        : static_cast<double>(m_fail) /
        static_cast<double>(m_total) * 100.0;

    text.Format(_T("불량률\r\n%.1f %%"), failRate);
    m_rateCard.SetText(text);

    Invalidate(FALSE);
}

void CStatisticsPage::OnSearch()
{
    RefreshFromServer(true);
}

void CStatisticsPage::DrawPanel(
    CDC& dc,
    const CRect& rect,
    const CString& title)
{
    CBrush background(UiTheme::CardBg());
    CPen border(PS_SOLID, 1, UiTheme::Border());

    CBrush* oldBrush = dc.SelectObject(&background);
    CPen* oldPen = dc.SelectObject(&border);

    dc.RoundRect(
        rect.left,
        rect.top,
        rect.right - 1,
        rect.bottom - 1,
        14,
        14);

    dc.SelectObject(oldBrush);
    dc.SelectObject(oldPen);

    dc.SetBkMode(TRANSPARENT);
    dc.SetTextColor(UiTheme::Text());

    CFont* oldFont = dc.SelectObject(&m_labelFont);

    CRect titleRect(
        rect.left + 22,
        rect.top + 16,
        rect.right - 22,
        rect.top + 44);

    dc.DrawText(
        title,
        &titleRect,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    dc.SelectObject(oldFont);
}

void CStatisticsPage::DrawHourlyGraph(
    CDC& dc,
    const CRect& rect)
{
    DrawPanel(dc, rect, _T("시간별 생산량"));

    const CRect chart(
        rect.left + 42,
        rect.top + 58,
        rect.right - 24,
        rect.bottom - 34);

    int maxValue = 1;
    for (int value : m_hourlyTotal)
    {
        if (value > maxValue)
            maxValue = value;
    }

    const int count = static_cast<int>(m_hourlyTotal.size());
    const int slotWidth = chart.Width() / count;
    const int barWidth = slotWidth / 2;

    CPen axisPen(PS_SOLID, 1, UiTheme::Border());
    CPen* oldPen = dc.SelectObject(&axisPen);

    dc.MoveTo(chart.left, chart.bottom);
    dc.LineTo(chart.right, chart.bottom);

    dc.SelectObject(oldPen);

    CFont* oldFont = dc.SelectObject(&m_normalFont);
    dc.SetBkMode(TRANSPARENT);

    for (int i = 0; i < count; ++i)
    {
        const int value = m_hourlyTotal[i];
        const int height =
            value * (chart.Height() - 24) / maxValue;

        const int x =
            chart.left + i * slotWidth +
            (slotWidth - barWidth) / 2;

        const int y = chart.bottom - height;

        dc.FillSolidRect(
            x,
            y,
            barWidth,
            height,
            UiTheme::Accent());

        CString valueText;
        valueText.Format(_T("%d"), value);

        CRect valueRect(
            x - 8,
            y - 22,
            x + barWidth + 8,
            y);

        dc.SetTextColor(UiTheme::Text());
        dc.DrawText(
            valueText,
            &valueRect,
            DT_CENTER | DT_SINGLELINE | DT_VCENTER);

        CString hourText;
        hourText.Format(_T("%s시"), HourLabels[i]);

        CRect labelRect(
            chart.left + i * slotWidth,
            chart.bottom + 4,
            chart.left + (i + 1) * slotWidth,
            chart.bottom + 27);

        dc.SetTextColor(UiTheme::Muted());
        dc.DrawText(
            hourText,
            &labelRect,
            DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }

    dc.SelectObject(oldFont);
}

void CStatisticsPage::DrawPassNgGraph(
    CDC& dc,
    const CRect& rect)
{
    DrawPanel(dc, rect, _T("정상 / NG 비율"));

    const double passRate =
        m_total == 0
        ? 0.0
        : static_cast<double>(m_pass) /
        static_cast<double>(m_total);

    const double failRate =
        m_total == 0
        ? 0.0
        : static_cast<double>(m_fail) /
        static_cast<double>(m_total);

    const int barLeft = rect.left + 34;
    const int barTop = rect.top + 92;
    const int barWidth = rect.Width() - 68;
    const int barHeight = 38;

    dc.FillSolidRect(
        barLeft,
        barTop,
        barWidth,
        barHeight,
        RGB(235, 240, 246));

    const int passWidth =
        static_cast<int>(barWidth * passRate);

    if (passWidth > 0)
    {
        dc.FillSolidRect(
            barLeft,
            barTop,
            passWidth,
            barHeight,
            UiTheme::Success());
    }

    const int failWidth = barWidth - passWidth;

    if (failWidth > 0)
    {
        dc.FillSolidRect(
            barLeft + passWidth,
            barTop,
            failWidth,
            barHeight,
            UiTheme::Danger());
    }

    CFont* oldFont = dc.SelectObject(&m_labelFont);
    dc.SetBkMode(TRANSPARENT);

    CString passText;
    passText.Format(
        _T("정상  %d건  (%.1f%%)"),
        m_pass,
        passRate * 100.0);

    CString failText;
    failText.Format(
        _T("NG  %d건  (%.1f%%)"),
        m_fail,
        failRate * 100.0);

    CRect passRect(
        rect.left + 34,
        rect.top + 154,
        rect.right - 34,
        rect.top + 180);

    CRect failRect(
        rect.left + 34,
        rect.top + 184,
        rect.right - 34,
        rect.top + 210);

    dc.SetTextColor(UiTheme::Success());
    dc.DrawText(
        passText,
        &passRect,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    dc.SetTextColor(UiTheme::Danger());
    dc.DrawText(
        failText,
        &failRect,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    dc.SelectObject(oldFont);
}

void CStatisticsPage::DrawDailyGraph(
    CDC& dc,
    const CRect& rect)
{
    DrawPanel(dc, rect, _T("최근 7일 생산량 / NG 추이"));

    const CRect chart(
        rect.left + 54,
        rect.top + 58,
        rect.right - 28,
        rect.bottom - 38);

    int maxValue = 1;
    for (int value : m_dailyTotal)
    {
        if (value > maxValue)
            maxValue = value;
    }

    const int count = static_cast<int>(m_dailyTotal.size());
    const int slotWidth = chart.Width() / count;
    const int totalBarWidth = slotWidth / 3;
    const int ngBarWidth = totalBarWidth / 2;

    CPen axisPen(PS_SOLID, 1, UiTheme::Border());
    CPen* oldPen = dc.SelectObject(&axisPen);

    dc.MoveTo(chart.left, chart.bottom);
    dc.LineTo(chart.right, chart.bottom);

    dc.SelectObject(oldPen);

    const CTime now = CTime::GetCurrentTime();
    CFont* oldFont = dc.SelectObject(&m_normalFont);
    dc.SetBkMode(TRANSPARENT);

    for (int i = 0; i < count; ++i)
    {
        const int total = m_dailyTotal[i];
        const int ng = m_dailyNg[i];

        const int totalHeight =
            total * (chart.Height() - 28) / maxValue;

        const int ngHeight =
            ng * (chart.Height() - 28) / maxValue;

        const int centerX =
            chart.left + i * slotWidth + slotWidth / 2;

        const int totalX = centerX - totalBarWidth;
        const int ngX = centerX + 4;

        dc.FillSolidRect(
            totalX,
            chart.bottom - totalHeight,
            totalBarWidth,
            totalHeight,
            UiTheme::Accent());

        if (ngHeight > 0)
        {
            dc.FillSolidRect(
                ngX,
                chart.bottom - ngHeight,
                ngBarWidth,
                ngHeight,
                UiTheme::Danger());
        }

        CString countText;
        countText.Format(_T("%d"), total);

        CRect countRect(
            totalX - 8,
            chart.bottom - totalHeight - 22,
            totalX + totalBarWidth + 8,
            chart.bottom - totalHeight);

        dc.SetTextColor(UiTheme::Text());
        dc.DrawText(
            countText,
            &countRect,
            DT_CENTER | DT_SINGLELINE | DT_VCENTER);

        const CTime date =
            now - CTimeSpan(6 - i, 0, 0, 0);

        const CString dateText = date.Format(_T("%m/%d"));

        CRect labelRect(
            chart.left + i * slotWidth,
            chart.bottom + 5,
            chart.left + (i + 1) * slotWidth,
            chart.bottom + 28);

        dc.SetTextColor(UiTheme::Muted());
        dc.DrawText(
            dateText,
            &labelRect,
            DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }

    // 범례
    const int legendY = rect.top + 17;
    const int legendX = rect.right - 255;

    dc.FillSolidRect(
        legendX,
        legendY + 6,
        14,
        14,
        UiTheme::Accent());

    dc.SetTextColor(UiTheme::Muted());

    CRect totalLegend(
        legendX + 20,
        legendY,
        legendX + 90,
        legendY + 26);

    dc.DrawText(
        _T("총 검사"),
        &totalLegend,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    dc.FillSolidRect(
        legendX + 100,
        legendY + 6,
        14,
        14,
        UiTheme::Danger());

    CRect ngLegend(
        legendX + 120,
        legendY,
        legendX + 175,
        legendY + 26);

    dc.DrawText(
        _T("NG"),
        &ngLegend,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    dc.SelectObject(oldFont);
}

void CStatisticsPage::OnPaint()
{
    CPaintDC dc(this);

    DrawHourlyGraph(
        dc,
        CRect(28, 236, 478, 452));

    DrawPassNgGraph(
        dc,
        CRect(494, 236, 940, 452));

    DrawDailyGraph(
        dc,
        CRect(28, 470, 940, 666));

    dc.SetBkMode(TRANSPARENT);
    dc.SetTextColor(UiTheme::Muted());

    CFont* oldFont = dc.SelectObject(&m_normalFont);

    CRect rangeRect(28, 674, 940, 700);

    dc.DrawText(
        m_rangeText,
        &rangeRect,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    dc.SelectObject(oldFont);
}
