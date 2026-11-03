#include "pch.h"
#include "ChatHandoverPage.h"
#include "../TireProductionMFC.h"
#include "../MainFrame.h"
#include <algorithm>
#include <climits>

namespace
{
    CString GetClientConfigPath()
    {
        TCHAR executablePath[MAX_PATH] = {};
        GetModuleFileName(nullptr, executablePath, MAX_PATH);

        CString directory(executablePath);
        const int separator = directory.ReverseFind(_T('\\'));
        if (separator >= 0)
            directory = directory.Left(separator + 1);

        return directory + _T("client_config.ini");
    }

    CString GetServerIp()
    {
        TCHAR value[64] = {};
        GetPrivateProfileString(
            _T("SERVER"),
            _T("IP"),
            _T("192.168.0.104"),
            value,
            _countof(value),
            GetClientConfigPath());
        return CString(value);
    }

    UINT GetServerPort()
    {
        return GetPrivateProfileInt(
            _T("SERVER"),
            _T("PORT"),
            5000,
            GetClientConfigPath());
    }

    CString Utf8ToCString(const std::string& text)
    {
        if (text.empty())
            return _T("");

        const int size = MultiByteToWideChar(
            CP_UTF8,
            0,
            text.data(),
            static_cast<int>(text.size()),
            nullptr,
            0);

        CString result;
        wchar_t* buffer = result.GetBuffer(size);

        MultiByteToWideChar(
            CP_UTF8,
            0,
            text.data(),
            static_cast<int>(text.size()),
            buffer,
            size);

        result.ReleaseBuffer(size);
        return result;
    }

    int HexValue(char ch)
    {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    }

    bool ReadHex4(const std::string& text, size_t pos, unsigned int& value)
    {
        if (pos + 4 > text.size())
            return false;

        value = 0;

        for (size_t i = 0; i < 4; ++i)
        {
            const int hex = HexValue(text[pos + i]);
            if (hex < 0)
                return false;

            value = (value << 4) | static_cast<unsigned int>(hex);
        }

        return true;
    }

    void AppendUtf8(std::string& out, unsigned int codePoint)
    {
        if (codePoint <= 0x7F)
        {
            out.push_back(static_cast<char>(codePoint));
        }
        else if (codePoint <= 0x7FF)
        {
            out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
        else if (codePoint <= 0xFFFF)
        {
            out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
            out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
    }

    bool ExtractJsonString(
        const std::string& json,
        const std::string& key,
        std::string& value)
    {
        const std::string token = "\"" + key + "\"";

        size_t pos = json.find(token);
        if (pos == std::string::npos)
            return false;

        pos = json.find(':', pos + token.size());
        if (pos == std::string::npos)
            return false;

        pos = json.find('"', pos + 1);
        if (pos == std::string::npos)
            return false;

        ++pos;

        std::string out;

        while (pos < json.size())
        {
            const char ch = json[pos++];

            if (ch == '"')
            {
                value = out;
                return true;
            }

            if (ch != '\\')
            {
                out.push_back(ch);
                continue;
            }

            if (pos >= json.size())
                return false;

            const char escaped = json[pos++];

            switch (escaped)
            {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;

            case 'u':
            {
                unsigned int first = 0;
                if (!ReadHex4(json, pos, first))
                    return false;

                pos += 4;
                unsigned int codePoint = first;

                if (first >= 0xD800 && first <= 0xDBFF)
                {
                    if (pos + 6 <= json.size() &&
                        json[pos] == '\\' &&
                        json[pos + 1] == 'u')
                    {
                        unsigned int second = 0;
                        if (ReadHex4(json, pos + 2, second) &&
                            second >= 0xDC00 && second <= 0xDFFF)
                        {
                            codePoint =
                                0x10000 +
                                ((first - 0xD800) << 10) +
                                (second - 0xDC00);

                            pos += 6;
                        }
                    }
                }

                AppendUtf8(out, codePoint);
                break;
            }

            default:
                out.push_back(escaped);
                break;
            }
        }

        return false;
    }

    bool ExtractJsonBool(
        const std::string& json,
        const std::string& key,
        bool& value)
    {
        const std::string token = "\"" + key + "\"";

        size_t pos = json.find(token);
        if (pos == std::string::npos)
            return false;

        pos = json.find(':', pos + token.size());
        if (pos == std::string::npos)
            return false;

        ++pos;

        while (pos < json.size() &&
            (json[pos] == ' ' ||
                json[pos] == '\t' ||
                json[pos] == '\r' ||
                json[pos] == '\n'))
        {
            ++pos;
        }

        if (json.compare(pos, 4, "true") == 0)
        {
            value = true;
            return true;
        }

        if (json.compare(pos, 5, "false") == 0)
        {
            value = false;
            return true;
        }

        return false;
    }

    CString FormatChatTime(
        const CString& createdAt,
        bool history)
    {
        if (createdAt.GetLength() >= 16)
        {
            if (history)
                return createdAt.Mid(5, 11);

            if (createdAt.GetLength() >= 19)
                return createdAt.Mid(11, 8);
        }

        return CTime::GetCurrentTime().Format(_T("%H:%M:%S"));
    }

    bool ExtractJsonUInt64(const std::string& json, const std::string& key, ULONGLONG& out)
    {
        const std::string token = "\"" + key + "\"";
        size_t pos = json.find(token);
        if (pos == std::string::npos) return false;
        pos = json.find(':', pos + token.size());
        if (pos == std::string::npos) return false;
        ++pos;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
        if (pos == json.size() || json[pos] < '0' || json[pos] > '9') return false;
        ULONGLONG value = 0;
        while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9')
        {
            const unsigned int digit = static_cast<unsigned int>(json[pos++] - '0');
            if (value > (ULLONG_MAX - digit) / 10) return false;
            value = value * 10 + digit;
        }
        out = value;
        return true;
    }

    std::string PercentDecodeDashboard(const std::string& text)
    {
        std::string result;
        for (size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == '%' && i + 2 < text.size())
            {
                int hi = HexValue(text[i + 1]);
                int lo = HexValue(text[i + 2]);
                if (hi >= 0 && lo >= 0)
                {
                    result.push_back(static_cast<char>((hi << 4) | lo));
                    i += 2;
                    continue;
                }
            }
            result.push_back(text[i]);
        }
        return result;
    }

    CString RoleName(const CString& role)
    {
        if (role == _T("ADMIN"))
            return _T("관리자");

        if (role == _T("OPERATOR"))
            return _T("현장");

        return role.IsEmpty() ? _T("-") : role;
    }
}

BEGIN_MESSAGE_MAP(CChatBubbleView, CWnd)
    ON_WM_PAINT()
    ON_WM_SIZE()
    ON_WM_VSCROLL()
    ON_WM_MOUSEWHEEL()
END_MESSAGE_MAP()

BOOL CChatBubbleView::CreateView(CWnd* parent, const CRect& rect, UINT id)
{
    CString cls = AfxRegisterWndClass(CS_DBLCLKS, ::LoadCursor(nullptr, IDC_ARROW),
        (HBRUSH)::GetStockObject(WHITE_BRUSH), nullptr);
    BOOL ok = CreateEx(WS_EX_CLIENTEDGE, cls, _T("Chat"),
        WS_CHILD | WS_VISIBLE | WS_VSCROLL, rect, parent, id);
    if (ok)
    {
        m_senderFont.CreatePointFont(90, _T("Malgun Gothic"));
        m_bodyFont.CreatePointFont(105, _T("Malgun Gothic"));
    }
    return ok;
}

void CChatBubbleView::AddMessage(const CString& time, const CString& sender,
    const CString& body, bool mine)
{
    m_messages.push_back({ time, sender, body, mine });
    if (m_messages.size() > 1000) m_messages.erase(m_messages.begin());
    UpdateScroll();
    m_scroll = (std::max)(0, m_contentHeight - 1);
    UpdateScroll();
    Invalidate(FALSE);
}

void CChatBubbleView::ClearMessages()
{
    m_messages.clear(); m_scroll = 0;
    UpdateScroll(); Invalidate(FALSE);
}

void CChatBubbleView::UpdateScroll()
{
    CRect rc; GetClientRect(&rc);
    CClientDC dc(this);
    CFont* old = dc.SelectObject(&m_bodyFont);
    int width = (std::max)(160, (std::min)(620, rc.Width() * 72 / 100));
    int y = 12;
    for (const auto& msg : m_messages)
    {
        CRect measured(0, 0, width - 32, 0);
        dc.DrawText(msg.body, measured, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        y += (std::max)(24, measured.Height()) + 64;
    }
    dc.SelectObject(old);
    m_contentHeight = y;
    m_scroll = (std::min)(m_scroll, (std::max)(0, m_contentHeight - rc.Height()));
    SCROLLINFO si = {};
    si.cbSize = sizeof(si); si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0; si.nMax = (std::max)(rc.Height(), m_contentHeight) - 1;
    si.nPage = (UINT)(std::max)(1, rc.Height()); si.nPos = m_scroll;
    SetScrollInfo(SB_VERT, &si, TRUE);
}

void CChatBubbleView::OnPaint()
{
    CPaintDC dc(this);
    CRect rc; GetClientRect(&rc);
    dc.FillSolidRect(rc, RGB(255, 255, 255));
    dc.SetBkMode(TRANSPARENT);
    const int maxWidth = (std::max)(160, (std::min)(620, rc.Width() * 72 / 100));
    int y = 12 - m_scroll;
    for (const auto& msg : m_messages)
    {
        CFont* old = dc.SelectObject(&m_bodyFont);
        CRect measured(0, 0, maxWidth - 32, 0);
        dc.DrawText(msg.body, measured, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        const int textHeight = (std::max)(24, measured.Height());
        const int bubbleHeight = textHeight + 43;
        const int bubbleWidth = maxWidth;
        const int x = msg.mine ? (std::max)(12, rc.Width() - bubbleWidth - 22) : 14;
        CRect bubble(x, y, x + bubbleWidth, y + bubbleHeight);
        if (bubble.bottom >= 0 && bubble.top <= rc.bottom)
        {
            dc.FillSolidRect(bubble, msg.mine ? RGB(234, 243, 255) : RGB(247, 249, 252));
            dc.SelectObject(&m_senderFont);
            dc.SetTextColor(msg.mine ? RGB(37, 99, 235) : RGB(100, 116, 139));
            CString header = msg.sender + _T("    ") + msg.time;
            CRect headerRect(x + 14, y + 8, x + bubbleWidth - 12, y + 27);
            dc.DrawText(header, headerRect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            dc.SelectObject(&m_bodyFont);
            dc.SetTextColor(RGB(30, 41, 59));
            CRect bodyRect(x + 14, y + 29, x + bubbleWidth - 14, y + bubbleHeight - 8);
            dc.DrawText(msg.body, bodyRect, DT_WORDBREAK | DT_NOPREFIX);
        }
        dc.SelectObject(old);
        y += bubbleHeight + 21;
    }
}

void CChatBubbleView::OnSize(UINT type, int cx, int cy)
{
    CWnd::OnSize(type, cx, cy);
    if (GetSafeHwnd() && m_bodyFont.GetSafeHandle()) UpdateScroll();
}
void CChatBubbleView::OnVScroll(UINT code, UINT pos, CScrollBar* bar)
{
    CRect rc; GetClientRect(&rc);
    SCROLLINFO si = {}; si.cbSize = sizeof(si); si.fMask = SIF_ALL;
    GetScrollInfo(SB_VERT, &si);
    int next = m_scroll;
    switch (code)
    {
    case SB_LINEUP: next -= 40; break;
    case SB_LINEDOWN: next += 40; break;
    case SB_PAGEUP: next -= rc.Height(); break;
    case SB_PAGEDOWN: next += rc.Height(); break;
    case SB_THUMBTRACK: next = si.nTrackPos; break;
    case SB_TOP: next = 0; break;
    case SB_BOTTOM: next = m_contentHeight; break;
    }
    m_scroll = (std::max)(0, (std::min)(next, (std::max)(0, m_contentHeight - rc.Height())));
    UpdateScroll(); Invalidate(FALSE);
    CWnd::OnVScroll(code, pos, bar);
}
BOOL CChatBubbleView::OnMouseWheel(UINT flags, short delta, CPoint point)
{
    CRect rc; GetClientRect(&rc);
    m_scroll = (std::max)(0, (std::min)(m_scroll - (delta / 120) * 72,
        (std::max)(0, m_contentHeight - rc.Height())));
    UpdateScroll(); Invalidate(FALSE);
    return TRUE;
}

BEGIN_MESSAGE_MAP(CChatHandoverPage, CBasePage)
    ON_BN_CLICKED(IDC_BTN_CHAT_SEND, &CChatHandoverPage::OnBnClickedSend)
    ON_BN_CLICKED(IDC_BTN_CHAT_RECONNECT, &CChatHandoverPage::OnBnClickedReconnect)
    ON_BN_CLICKED(IDC_BTN_HANDOVER_REGISTER, &CChatHandoverPage::OnBnClickedHandoverRegister)
    ON_BN_CLICKED(IDC_BTN_HANDOVER_CONFIRM, &CChatHandoverPage::OnBnClickedHandoverConfirm)
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_LIST_HANDOVER, &CChatHandoverPage::OnHandoverItemChanged)
    ON_MESSAGE(WM_CHAT_SOCKET_PACKET, &CChatHandoverPage::OnSocketPacket)
    ON_MESSAGE(WM_CHAT_SOCKET_STATE, &CChatHandoverPage::OnSocketState)
END_MESSAGE_MAP()

CChatHandoverPage::CChatHandoverPage()
{
    m_pSocket = new CTireClientSocket(this);
}

CChatHandoverPage::~CChatHandoverPage()
{
    if (m_pSocket)
    {
        m_pSocket->Close();
        delete m_pSocket;
        m_pSocket = nullptr;
    }
}

BOOL CChatHandoverPage::CreatePage(CWnd* p, const CRect& r, UINT id)
{
    if (!CBasePage::CreatePage(p, r, id))
        return FALSE;

    // 관리자/현장 작업자 화면에 공통으로 사용하는 간결한 채팅 레이아웃.
    AddTitle(_T("채팅 · 인수인계"));
    AddLabel(_T("대화"), 28, 78, 200, 28);
    m_pConnectionLabel = AddLabel(_T("● 서버 연결 확인 중"), 376, 79, 165, 26);
    AddButton(_T("재연결"), 544, 74, 90, 34, IDC_BTN_CHAT_RECONNECT);

    // 현장 작업자와 동일하게 대화는 좌/우 말풍선으로 표시합니다.
    m_pChatView = new CChatBubbleView();
    m_pChatView->CreateView(this, CRect(28, 120, 634, 532), 10110);

    m_pChatEdit = AddEdit(28, 547, 474, 44);
    AddButton(_T("전송"), 514, 547, 120, 44, IDC_BTN_CHAT_SEND);

    AddLabel(_T("인수인계"), 654, 78, 260, 28);
    m_pHandoverList = AddList(654, 120, 302, 310);
    m_pHandoverList->SetDlgCtrlID(IDC_LIST_HANDOVER);
    m_pHandoverList->SetExtendedStyle(
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    m_pHandoverList->InsertColumn(0, _T("상태"), LVCFMT_LEFT, 82);
    m_pHandoverList->InsertColumn(1, _T("내용"), LVCFMT_LEFT, 196);
    AddButton(_T("인수인계 등록"), 654, 445, 145, 42,
        IDC_BTN_HANDOVER_REGISTER);
    AddButton(_T("확인 완료"), 811, 445, 145, 42,
        IDC_BTN_HANDOVER_CONFIRM);
    m_pHandoverDetail = AddBox(_T("인수인계 항목을 선택하세요."),
        654, 505, 302, 104);

    m_pSocket->ConnectToServer(GetServerIp(), GetServerPort());
    return TRUE;
}

BOOL CChatHandoverPage::PreTranslateMessage(MSG* pMsg)
{
    if (pMsg &&
        pMsg->message == WM_KEYDOWN &&
        pMsg->wParam == VK_RETURN &&
        m_pChatEdit &&
        ::GetFocus() == m_pChatEdit->GetSafeHwnd())
    {
        SendCurrentMessage();
        return TRUE;
    }

    return CBasePage::PreTranslateMessage(pMsg);
}

void CChatHandoverPage::SendCurrentMessage()
{
    if (!m_pChatEdit || !m_pSocket)
        return;

    CString message;
    m_pChatEdit->GetWindowText(message);
    message.Trim();

    if (message.IsEmpty())
        return;

    if (!m_pSocket->IsConnected())
    {
        SetConnectionState(
            false,
            _T("● 서버 미연결 · 재연결 필요"));

        AfxMessageBox(
            _T("중앙 서버에 연결되어 있지 않습니다. 재연결 버튼을 눌러주세요."));

        return;
    }

    if (!m_pSocket->SendChat(
        g_userName,
        g_userRole,
        message))
    {
        SetConnectionState(
            false,
            _T("● 메시지 전송 실패"));

        AfxMessageBox(
            _T("메시지 전송에 실패했습니다."));

        return;
    }

    m_pChatEdit->SetWindowText(_T(""));
    m_pChatEdit->SetFocus();
}

void CChatHandoverPage::OnBnClickedSend()
{
    SendCurrentMessage();
}

void CChatHandoverPage::RequestDashboardStats()
{
    if (m_pSocket && m_pSocket->IsConnected())
        m_pSocket->SendRaw("{\"type\":\"DASHBOARD_STATS\"}");
}

bool CChatHandoverPage::SubmitDemoInspection(
    const CString& verdict, const CString& defectType)
{
    if (!m_pSocket || !m_pSocket->IsConnected())
        return false;

    // 이 화면은 고정된 판정 코드만 전송하므로 임의 문자열을 JSON에 삽입하지 않습니다.
    if (verdict == _T("OK"))
        return m_pSocket->SendRaw(
            "{\"type\":\"DEMO_INSPECTION\",\"result\":\"OK\"}");
    if (verdict != _T("NG"))
        return false;
    if (defectType == _T("WHEEL_DAMAGE"))
        return m_pSocket->SendRaw(
            "{\"type\":\"DEMO_INSPECTION\",\"result\":\"NG\",\"defect_type\":\"WHEEL_DAMAGE\"}");
    if (defectType == _T("TIRE_DAMAGE"))
        return m_pSocket->SendRaw(
            "{\"type\":\"DEMO_INSPECTION\",\"result\":\"NG\",\"defect_type\":\"TIRE_DAMAGE\"}");
    if (defectType == _T("BOTH_DAMAGE"))
        return m_pSocket->SendRaw(
            "{\"type\":\"DEMO_INSPECTION\",\"result\":\"NG\",\"defect_type\":\"BOTH_DAMAGE\"}");
    return false;
}

void CChatHandoverPage::OnBnClickedReconnect()
{
    if (!m_pSocket)
        return;

    m_pSocket->ConnectToServer(GetServerIp(), GetServerPort());
}

LRESULT CChatHandoverPage::OnSocketPacket(WPARAM, LPARAM lParam)
{
    std::string* packet = reinterpret_cast<std::string*>(lParam);
    if (!packet)
        return 0;

    ProcessReceivedPacket(*packet);
    delete packet;
    return 0;
}

LRESULT CChatHandoverPage::OnSocketState(WPARAM wParam, LPARAM lParam)
{
    CString* text = reinterpret_cast<CString*>(lParam);

    const bool connected = (wParam != 0);

    SetConnectionState(
        connected,
        text ? *text : CString());

    if (connected &&
        m_pSocket &&
        m_pChatView)
    {
        m_pChatView->ClearMessages();
        m_loadedHistoryCount = 0;

        m_pSocket->SendRaw(
            "{\"type\":\"CHAT_HISTORY\"}");

        ReloadHandovers();
        RequestDashboardStats();
    }

    if (text)
        delete text;

    return 0;
}

void CChatHandoverPage::ProcessReceivedPacket(const std::string& jsonString)
{
    std::string type;

    if (!ExtractJsonString(
        jsonString,
        "type",
        type))
    {
        return;
    }

    if (type == "HEARTBEAT") {
        CMainFrame* frame = (CMainFrame*)AfxGetMainWnd();
        if (frame) frame->OnHeartbeatReceived();
        return;
    }

    if (type == "HANDOVER_ITEM")
    {
        HandoverItem item;
        if (HandoverMerge::ParseItem(jsonString, item))
            UpsertHandover(item);
        return;
    }

    if (type == "HANDOVER_LIST_END")
        return;

    if (type == "HANDOVER_RESULT" ||
        type == "HANDOVER_CHECK_RESULT")
    {
        ReloadHandovers();
        return;
    }

    if (type == "CHAT_HISTORY_END")
    {
        m_loadedHistoryCount = 0;
        return;
    }

    if (type == "CHAT")
    {
        std::string userUtf8;
        std::string roleUtf8;
        std::string messageUtf8;
        std::string createdUtf8;

        bool isHistory = false;

        ExtractJsonString(
            jsonString,
            "user_name",
            userUtf8);

        ExtractJsonString(
            jsonString,
            "role",
            roleUtf8);

        ExtractJsonString(
            jsonString,
            "message",
            messageUtf8);

        ExtractJsonString(
            jsonString,
            "created_at",
            createdUtf8);

        ExtractJsonBool(
            jsonString,
            "history",
            isHistory);

        const CString user =
            Utf8ToCString(userUtf8);

        const CString role =
            RoleName(
                Utf8ToCString(roleUtf8));

        const CString message =
            Utf8ToCString(messageUtf8);

        const CString createdAt =
            Utf8ToCString(createdUtf8);

        const CString time =
            FormatChatTime(
                createdAt,
                isHistory);

        if (isHistory)
            ++m_loadedHistoryCount;

        AddChatMessage(
            time,
            role,
            user,
            message);

        return;
    }

    CMainFrame* frame =
        dynamic_cast<CMainFrame*>(
            GetParent());

    if (!frame)
        return;

    if (type == "DASHBOARD_STATS_RESULT")
    {
        ULONGLONG total = 0, passed = 0, failed = 0, wheel = 0, tire = 0, both = 0, wrong = 0, unknown = 0;
        if (!ExtractJsonUInt64(jsonString, "total", total) ||
            !ExtractJsonUInt64(jsonString, "passed", passed) ||
            !ExtractJsonUInt64(jsonString, "failed", failed) ||
            !ExtractJsonUInt64(jsonString, "wheel", wheel) ||
            !ExtractJsonUInt64(jsonString, "tire", tire) ||
            !ExtractJsonUInt64(jsonString, "both", both) ||
            !ExtractJsonUInt64(jsonString, "wrong", wrong) ||
            !ExtractJsonUInt64(jsonString, "unknown", unknown)) return;

        std::vector<DashboardInspectionRow> rows;
        std::string recent;
        if (ExtractJsonString(jsonString, "recent", recent))
        {
            size_t begin = 0;
            while (begin < recent.size() && rows.size() < 50)
            {
                size_t lineEnd = recent.find('\n', begin);
                const std::string line = recent.substr(begin,
                    lineEnd == std::string::npos ? std::string::npos : lineEnd - begin);
                std::vector<std::string> parts;
                size_t from = 0;
                for (int i = 0; i < 4; ++i)
                {
                    size_t sep = line.find('|', from);
                    if (sep == std::string::npos) break;
                    parts.push_back(PercentDecodeDashboard(line.substr(from, sep - from)));
                    from = sep + 1;
                }
                if (parts.size() == 4)
                {
                    parts.push_back(PercentDecodeDashboard(line.substr(from)));
                    DashboardInspectionRow row;
                    row.time = Utf8ToCString(parts[0]);
                    row.tireId = Utf8ToCString(parts[1]);
                    row.verdict = Utf8ToCString(parts[2]);
                    row.defectType = Utf8ToCString(parts[3]);
                    row.detail = Utf8ToCString(parts[4]);
                    rows.push_back(row);
                }
                if (lineEnd == std::string::npos) break;
                begin = lineEnd + 1;
            }
        }
        frame->ApplyDashboardStats(total, passed, failed,
            wheel, tire, both, wrong, unknown, rows);
        return;
    }

    if (type == "DEMO_INSPECTION_RESULT")
    {
        std::string status, message;
        ExtractJsonString(jsonString, "status", status);
        if (status != "OK")
        {
            ExtractJsonString(jsonString, "message", message);
            AfxMessageBox(_T("가상 검사 저장 실패: ") + Utf8ToCString(message),
                MB_OK | MB_ICONWARNING);
        }
        return;
    }

    if (type == "INSPECTION")
    {
        // 검사 한 건 저장 완료 통지 -> DB 재조회 (중복 증가 방지).
        RequestDashboardStats();
        return;
    }

    if (type == "EQUIPMENT")
    {
        std::string equipmentUtf8;
        std::string statusUtf8;
        std::string messageUtf8;

        ExtractJsonString(
            jsonString,
            "equipment",
            equipmentUtf8);

        ExtractJsonString(
            jsonString,
            "status",
            statusUtf8);

        ExtractJsonString(
            jsonString,
            "message",
            messageUtf8);

        frame->ApplyEquipmentEvent(
            Utf8ToCString(equipmentUtf8),
            Utf8ToCString(statusUtf8),
            Utf8ToCString(messageUtf8));
    }
}

void CChatHandoverPage::ReloadHandovers()
{
    if (!m_pSocket || !m_pSocket->IsConnected() || !m_pHandoverList)
        return;

    m_handovers.clear();
    m_pHandoverList->DeleteAllItems();
    m_selectedHandoverId = 0;

    if (m_pHandoverDetail)
        m_pHandoverDetail->SetText(_T("목록에서 인수인계를 선택해주세요."));

    m_pSocket->SendRaw(HandoverMerge::BuildListRequestPacket());
}

void CChatHandoverPage::OnBnClickedHandoverRegister()
{
    CHandoverRegisterDialog dialog(g_userName, this);
    if (dialog.DoModal() != IDOK)
        return;

    if (!m_pSocket || !m_pSocket->IsConnected())
    {
        AfxMessageBox(_T("중앙 서버에 연결되지 않았습니다."));
        return;
    }

    if (!m_pSocket->SendRaw(
        HandoverMerge::BuildRegisterPacket(dialog.GetData())))
    {
        AfxMessageBox(_T("인수인계 전송에 실패했습니다."));
    }
}

void CChatHandoverPage::OnBnClickedHandoverConfirm()
{
    const HandoverItem* item = FindHandover(m_selectedHandoverId);
    if (!item)
    {
        AfxMessageBox(_T("확인할 인수인계를 선택해주세요."));
        return;
    }

    if (item->checked)
    {
        AfxMessageBox(_T("이미 확인 완료된 항목입니다."));
        return;
    }

    if (!m_pSocket || !m_pSocket->IsConnected())
    {
        AfxMessageBox(_T("중앙 서버에 연결되지 않았습니다."));
        return;
    }

    if (AfxMessageBox(_T("확인 완료 처리할까요?"),
        MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;

    if (!m_pSocket->SendRaw(
        HandoverMerge::BuildConfirmPacket(item->id, g_userName)))
    {
        AfxMessageBox(_T("확인 완료 전송에 실패했습니다."));
    }
}

void CChatHandoverPage::OnHandoverItemChanged(NMHDR* header, LRESULT* result)
{
    const NMLISTVIEW* view = reinterpret_cast<NMLISTVIEW*>(header);

    if (view && view->iItem >= 0 &&
        (view->uNewState & LVIS_SELECTED) != 0)
    {
        m_selectedHandoverId = static_cast<long long>(
            m_pHandoverList->GetItemData(view->iItem));

        const HandoverItem* item = FindHandover(m_selectedHandoverId);
        if (item && m_pHandoverDetail)
            m_pHandoverDetail->SetText(HandoverMerge::BuildDetailText(*item));
    }

    if (result)
        *result = 0;
}

const HandoverItem* CChatHandoverPage::FindHandover(long long id) const
{
    for (const auto& item : m_handovers)
    {
        if (item.id == id)
            return &item;
    }

    return nullptr;
}

void CChatHandoverPage::UpsertHandover(const HandoverItem& item)
{
    if (!m_pHandoverList)
        return;

    int row = -1;

    for (int index = 0; index < m_pHandoverList->GetItemCount(); ++index)
    {
        if (static_cast<long long>(m_pHandoverList->GetItemData(index)) == item.id)
        {
            row = index;
            break;
        }
    }

    bool replaced = false;
    for (auto& current : m_handovers)
    {
        if (current.id == item.id)
        {
            current = item;
            replaced = true;
            break;
        }
    }

    if (!replaced)
        m_handovers.push_back(item);

    if (row < 0)
    {
        row = m_pHandoverList->GetItemCount();
        m_pHandoverList->InsertItem(row, _T(""));
        m_pHandoverList->SetItemData(row, static_cast<DWORD_PTR>(item.id));
    }

    m_pHandoverList->SetItemText(
        row, 0, item.checked ? _T("확인완료") : _T("미확인"));
    m_pHandoverList->SetItemText(
        row, 1, HandoverMerge::BuildListText(item));
}

void CChatHandoverPage::AddChatMessage(
    const CString& time,
    const CString& role,
    const CString& user,
    const CString& message)
{
    if (!m_pChatView) return;
    CString sender = role;
    if (!sender.IsEmpty() && !user.IsEmpty()) sender += _T(" · ");
    sender += user;
    const bool mine = (user == g_userName && !user.IsEmpty());
    m_pChatView->AddMessage(time, sender, message, mine);
}

void CChatHandoverPage::SetConnectionState(bool connected, const CString& text)
{
    if (!m_pConnectionLabel)
        return;

    // 화면에는 주소/포트를 표시하지 않습니다.
    CString display = connected
        ? _T("● 서버 연결됨")
        : _T("● 서버 연결 끊김");

    m_pConnectionLabel->SetWindowText(display);

    m_pConnectionLabel->SetFont(GetFont());
    m_pConnectionLabel->Invalidate();
}
