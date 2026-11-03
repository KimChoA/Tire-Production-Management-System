#include "../pch.h"
#include "DailyReportPage.h"
#include "../MainFrame.h"
#include "../TireProductionMFC.h"
#include <afxdlgs.h>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cstdint>
#include <map>
#include <ws2tcpip.h>
#include <cwctype>
#pragma comment(lib, "Ws2_32.lib")

namespace
{
    COLORREF C_NAVY = RGB(28, 67, 108);
    COLORREF C_BLUE = RGB(78, 157, 224);
    COLORREF C_SKY = RGB(234, 244, 253);
    COLORREF C_GREEN = RGB(49, 163, 109);
    COLORREF C_RED = RGB(217, 90, 90);
    COLORREF C_ORANGE = RGB(231, 163, 60);
    COLORREF C_INK = RGB(30, 43, 58);
    COLORREF C_MUTED = RGB(108, 122, 137);
    COLORREF C_LINE = RGB(217, 226, 236);
    COLORREF C_PAPER = RGB(247, 249, 252);

    CString XmlEscape(CString value)
    {
        // XML 1.0에서 허용되지 않는 C0 제어문자를 제거합니다.
        CString cleaned;
        for (int i = 0; i < value.GetLength(); ++i)
        {
            const TCHAR ch = value[i];
            if (ch == _T('\t') || ch == _T('\n') || ch == _T('\r') || ch >= 0x20)
                cleaned += ch;
        }
        value = cleaned;
        value.Replace(_T("&"), _T("&amp;"));
        value.Replace(_T("<"), _T("&lt;"));
        value.Replace(_T(">"), _T("&gt;"));
        value.Replace(_T("\""), _T("&quot;"));
        value.Replace(_T("'"), _T("&apos;"));
        return value;
    }

    std::string ToUtf8(const CString& text)
    {
        if (text.IsEmpty())
            return {};

        int len = ::WideCharToMultiByte(
            CP_UTF8, 0, text.GetString(), text.GetLength(),
            nullptr, 0, nullptr, nullptr);

        std::string result(static_cast<size_t>(len), '\0');
        ::WideCharToMultiByte(
            CP_UTF8, 0, text.GetString(), text.GetLength(),
            result.data(), len, nullptr, nullptr);
        return result;
    }

    // XLSX는 표준 ZIP(OPC) 컨테이너입니다.
    // Windows PowerShell/한글 경로/압축 엔트리 경로에 의존하지 않도록
    // ZIP STORE(method=0)를 직접 기록합니다. 파일명은 항상 '/' 구분자를 사용합니다.
    struct XlsxPart
    {
        std::string name;
        std::string content;
    };

    void AppendZipU16(std::string& out, unsigned int value)
    {
        out.push_back(static_cast<char>(value & 0xffu));
        out.push_back(static_cast<char>((value >> 8) & 0xffu));
    }

    void AppendZipU32(std::string& out, std::uint32_t value)
    {
        AppendZipU16(out, static_cast<unsigned int>(value & 0xffffu));
        AppendZipU16(out, static_cast<unsigned int>((value >> 16) & 0xffffu));
    }

    std::uint32_t XlsxCrc32(const std::string& bytes)
    {
        std::uint32_t crc = 0xffffffffu;
        for (unsigned char b : bytes)
        {
            crc ^= static_cast<std::uint32_t>(b);
            for (int i = 0; i < 8; ++i)
                crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
        }
        return crc ^ 0xffffffffu;
    }

    bool SaveXlsxParts(const CString& filePath, const std::vector<XlsxPart>& parts)
    {
        if (parts.empty() || parts.size() > 65535)
            return false;

        struct CentralRecord
        {
            std::string name;
            std::uint32_t crc;
            std::uint32_t size;
            std::uint32_t offset;
        };

        std::vector<CentralRecord> central;
        std::string output;
        for (const auto& part : parts)
        {
            if (part.name.empty() || part.name.size() > 65535 ||
                part.content.size() > 0xffffffffull ||
                output.size() > 0xffffffffull ||
                part.name.find('\\') != std::string::npos)
                return false;

            const std::uint32_t size = static_cast<std::uint32_t>(part.content.size());
            const std::uint32_t crc = XlsxCrc32(part.content);
            central.push_back({ part.name, crc, size, static_cast<std::uint32_t>(output.size()) });
            AppendZipU32(output, 0x04034b50u); // local file header
            AppendZipU16(output, 20);          // version needed
            AppendZipU16(output, 0);           // flags
            AppendZipU16(output, 0);           // compression method: STORE
            AppendZipU16(output, 0);           // modified time
            AppendZipU16(output, 0);           // modified date
            AppendZipU32(output, crc);
            AppendZipU32(output, size);
            AppendZipU32(output, size);
            AppendZipU16(output, static_cast<unsigned int>(part.name.size()));
            AppendZipU16(output, 0);           // extra field length
            output += part.name;
            output += part.content;
        }

        if (output.size() > 0xffffffffull)
            return false;
        const std::uint32_t centralStart = static_cast<std::uint32_t>(output.size());
        for (const auto& record : central)
        {
            AppendZipU32(output, 0x02014b50u); // central file header
            AppendZipU16(output, 20);           // version made by
            AppendZipU16(output, 20);           // version needed
            AppendZipU16(output, 0);            // flags
            AppendZipU16(output, 0);            // method
            AppendZipU16(output, 0);            // time
            AppendZipU16(output, 0);            // date
            AppendZipU32(output, record.crc);
            AppendZipU32(output, record.size);
            AppendZipU32(output, record.size);
            AppendZipU16(output, static_cast<unsigned int>(record.name.size()));
            AppendZipU16(output, 0);            // extra field
            AppendZipU16(output, 0);            // comment length
            AppendZipU16(output, 0);            // disk start
            AppendZipU16(output, 0);            // internal attributes
            AppendZipU32(output, 0);            // external attributes
            AppendZipU32(output, record.offset);
            output += record.name;
        }
        if (output.size() > 0xffffffffull)
            return false;
        const std::uint32_t centralSize = static_cast<std::uint32_t>(output.size() - centralStart);
        AppendZipU32(output, 0x06054b50u); // end of central directory
        AppendZipU16(output, 0);
        AppendZipU16(output, 0);
        AppendZipU16(output, static_cast<unsigned int>(central.size()));
        AppendZipU16(output, static_cast<unsigned int>(central.size()));
        AppendZipU32(output, centralSize);
        AppendZipU32(output, centralStart);
        AppendZipU16(output, 0);

        try
        {
            CFile file(filePath, CFile::modeCreate | CFile::modeWrite | CFile::typeBinary);
            if (output.size() > 0xffffffffu)
                return false;
            file.Write(output.data(), static_cast<UINT>(output.size()));
            file.Close();
            return true;
        }
        catch (CFileException* ex)
        {
            ex->Delete();
            return false;
        }
    }

    // 서버의 보고서 전용 응답은 단순한 JSON 스칼라와 URL 인코딩된 행으로 구성합니다.
    // 다른 화면의 상시 연결 소켓은 사용하지 않고 별도 TCP 연결을 사용합니다.
    bool ReadJsonString(const std::string& json, const char* key, std::string& value)
    {
        std::string token = "\"" + std::string(key) + "\"";
        size_t pos = json.find(token);
        if (pos == std::string::npos) return false;
        pos = json.find(':', pos + token.size());
        if (pos == std::string::npos) return false;
        pos = json.find('"', pos + 1);
        if (pos == std::string::npos) return false;
        value.clear();
        for (++pos; pos < json.size(); ++pos)
        {
            unsigned char c = static_cast<unsigned char>(json[pos]);
            if (c == '"') return true;
            if (c != '\\') { value.push_back(static_cast<char>(c)); continue; }
            if (++pos >= json.size()) return false;
            switch (json[pos])
            {
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            case '"': value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            default: return false;
            }
        }
        return false;
    }

    bool ReadJsonLong(const std::string& json, const char* key, long long& value)
    {
        std::string token = "\"" + std::string(key) + "\"";
        size_t pos = json.find(token);
        if (pos == std::string::npos) return false;
        pos = json.find(':', pos + token.size());
        if (pos == std::string::npos) return false;
        ++pos;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
        size_t end = pos;
        while (end < json.size() && (json[end] == '-' || (json[end] >= '0' && json[end] <= '9'))) ++end;
        if (end == pos) return false;
        try { value = std::stoll(json.substr(pos, end - pos)); return true; }
        catch (...) { return false; }
    }

    std::string PercentDecode(const std::string& input)
    {
        std::string out;
        out.reserve(input.size());
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
            };
        for (size_t i = 0; i < input.size(); ++i)
        {
            if (input[i] == '%' && i + 2 < input.size())
            {
                int a = hex(input[i + 1]), b = hex(input[i + 2]);
                if (a >= 0 && b >= 0)
                {
                    out.push_back(static_cast<char>((a << 4) | b));
                    i += 2;
                    continue;
                }
            }
            out.push_back(input[i]);
        }
        return out;
    }

    CString Utf8CStr(const std::string& input)
    {
        if (input.empty()) return _T("");
        int count = ::MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
        if (count <= 0) return _T("");
        CString out;
        wchar_t* buffer = out.GetBuffer(count);
        ::MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), buffer, count);
        out.ReleaseBuffer(count);
        return out;
    }

    CString GetReportServerConfig()
    {
        TCHAR path[MAX_PATH] = {};
        ::GetModuleFileName(nullptr, path, MAX_PATH);
        CString directory(path);
        int last = directory.ReverseFind(_T('\\'));
        return (last >= 0 ? directory.Left(last + 1) : CString()) + _T("client_config.ini");
    }

    bool FetchReportLine(const CString& date, std::string& json)
    {
        WSADATA wsa = {};
        if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        SOCKET sock = INVALID_SOCKET;
        bool ok = false;
        do
        {
            CString config = GetReportServerConfig();
            TCHAR host[128] = {};
            GetPrivateProfileString(_T("SERVER"), _T("IP"), _T("192.168.0.104"),
                host, _countof(host), config);
            UINT port = GetPrivateProfileInt(_T("SERVER"), _T("PORT"), 5000, config);
            // 프로젝트 설정은 IPv4 주소를 사용합니다.
            sockaddr_in addr = {};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(static_cast<u_short>(port));
            CStringA hostA(host);
            if (::inet_pton(AF_INET, hostA.GetString(), &addr.sin_addr) != 1) break;
            sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (sock == INVALID_SOCKET) break;
            u_long nonBlocking = 1;
            ::ioctlsocket(sock, FIONBIO, &nonBlocking);
            int connectStatus = ::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
            if (connectStatus == SOCKET_ERROR && ::WSAGetLastError() != WSAEWOULDBLOCK) break;
            fd_set writeSet;
            FD_ZERO(&writeSet);
            FD_SET(sock, &writeSet);
            timeval connectTimeout = { 1, 0 };
            if (::select(0, nullptr, &writeSet, nullptr, &connectTimeout) <= 0) break;
            int soErr = 0; int soLen = sizeof(soErr);
            ::getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &soLen);
            if (soErr != 0) break;
            nonBlocking = 0;
            ::ioctlsocket(sock, FIONBIO, &nonBlocking);
            DWORD timeout = 2000;
            ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
            ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
            std::string request = "{\"type\":\"REPORT_INSPECTIONS\",\"date\":\"" + ToUtf8(date) + "\"}\n";
            int offset = 0;
            while (offset < static_cast<int>(request.size()))
            {
                int sent = ::send(sock, request.data() + offset, static_cast<int>(request.size()) - offset, 0);
                if (sent <= 0) break;
                offset += sent;
            }
            if (offset != static_cast<int>(request.size())) break;
            json.clear();
            char buffer[8192];
            while (json.size() < 4 * 1024 * 1024)
            {
                int n = ::recv(sock, buffer, sizeof(buffer), 0);
                if (n <= 0) break;
                json.append(buffer, static_cast<size_t>(n));
                size_t newline = json.find('\n');
                if (newline != std::string::npos) { json.resize(newline); ok = true; break; }
            }
        } while (false);
        if (sock != INVALID_SOCKET) { ::shutdown(sock, SD_BOTH); ::closesocket(sock); }
        ::WSACleanup();
        return ok;
    }

    CString DefectTypeDisplay(CString code)
    {
        code.MakeUpper();
        if (code == _T("WHEEL_DAMAGE")) return _T("휠 손상");
        if (code == _T("TIRE_DAMAGE")) return _T("타이어 고무 손상");
        if (code == _T("BOTH_DAMAGE")) return _T("휠·타이어 동시 손상");
        return _T("미분류");
    }

    void FillRoundRect(CDC& dc, const CRect& rect, COLORREF fill, COLORREF border, int radius)
    {
        CPen pen(PS_SOLID, 1, border);
        CBrush brush(fill);
        CPen* oldPen = dc.SelectObject(&pen);
        CBrush* oldBrush = dc.SelectObject(&brush);
        dc.RoundRect(rect, CPoint(radius, radius));
        dc.SelectObject(oldBrush);
        dc.SelectObject(oldPen);
    }

    void FillRectColor(CDC& dc, const CRect& rect, COLORREF fill)
    {
        CBrush brush(fill);
        dc.FillRect(rect, &brush);
    }

    void DrawTextEx(
        CDC& dc,
        const CString& text,
        const CRect& rect,
        CFont& font,
        COLORREF color,
        UINT format)
    {
        CFont* oldFont = dc.SelectObject(&font);
        COLORREF oldColor = dc.SetTextColor(color);
        int oldBk = dc.SetBkMode(TRANSPARENT);
        CRect r = rect;
        dc.DrawText(text, &r, format | DT_NOPREFIX);
        dc.SetBkMode(oldBk);
        dc.SetTextColor(oldColor);
        dc.SelectObject(oldFont);
    }

    void DrawSectionTitle(
        CDC& dc,
        int number,
        const CString& title,
        int left,
        int right,
        int y,
        int unit,
        CFont& numFont,
        CFont& titleFont)
    {
        CString num;
        num.Format(_T("%02d"), number);
        DrawTextEx(dc, num, CRect(left, y, left + 35 * unit, y + 22 * unit),
            numFont, C_NAVY, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        DrawTextEx(dc, title, CRect(left + 40 * unit, y, right, y + 22 * unit),
            titleFont, C_INK, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        CPen linePen(PS_SOLID, 1, C_LINE);
        CPen* old = dc.SelectObject(&linePen);
        dc.MoveTo(left, y + 23 * unit);
        dc.LineTo(right, y + 23 * unit);
        dc.SelectObject(old);
    }

    CString OneLine(CString value)
    {
        value.Replace(_T("\r"), _T(" "));
        value.Replace(_T("\n"), _T(" "));
        while (value.Find(_T("  ")) >= 0)
            value.Replace(_T("  "), _T(" "));
        value.Trim();
        return value;
    }

    class CReportPreviewDialog : public CDialog
    {
    public:
        CReportPreviewDialog(const CDailyReportPage* page, CWnd* parent)
            : CDialog(), m_page(page)
        {
            BuildTemplate();
            InitModalIndirect(reinterpret_cast<LPCDLGTEMPLATE>(m_template.data()), parent);
        }

    protected:
        BOOL OnInitDialog() override
        {
            CDialog::OnInitDialog();
            SetWindowText(_T("인쇄 미리보기"));
            SetWindowPos(nullptr, 0, 0, 900, 760,
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            CenterWindow(GetParent());

            m_previous.Create(_T("이전 페이지"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                CRect(470, 690, 580, 725), this, 22001);
            m_next.Create(_T("다음 페이지"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                CRect(590, 690, 700, 725), this, 22002);
            m_close.Create(_T("닫기"),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                CRect(785, 690, 865, 725), this, IDCANCEL);
            return TRUE;
        }

        afx_msg void OnPaint()
        {
            CPaintDC dc(this);
            CRect client;
            GetClientRect(&client);
            FillRectColor(dc, client, RGB(224, 229, 235));

            // A4 비율로 화면에 축소 표시
            const double a4 = 210.0 / 297.0;
            int top = 18;
            int bottom = 665;
            int height = bottom - top;
            int width = static_cast<int>(height * a4);
            int left = (client.Width() - width) / 2;
            CRect paper(left, top, left + width, bottom);

            FillRectColor(dc, paper, RGB(255, 255, 255));
            if (m_page)
                m_page->DrawReportPage(dc, paper, m_currentPage);
        }

        afx_msg void OnPrevious() { if (m_currentPage > 0) { --m_currentPage; Invalidate(FALSE); } }
        afx_msg void OnNext() { if (m_page && m_currentPage + 1 < m_page->GetReportPageCount()) { ++m_currentPage; Invalidate(FALSE); } }
        DECLARE_MESSAGE_MAP()

    private:
        int m_currentPage = 0;
        CButton m_previous, m_next;
        void BuildTemplate()
        {
            DLGTEMPLATE dlg = {};
            dlg.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT;
            dlg.cdit = 0;
            dlg.cx = 450;
            dlg.cy = 380;

            const BYTE* p = reinterpret_cast<const BYTE*>(&dlg);
            m_template.assign(p, p + sizeof(dlg));
            AppendWord(0);
            AppendWord(0);
            AppendWideString(L"인쇄 미리보기");
            AppendWord(9);
            AppendWideString(L"Segoe UI");
        }

        void AppendWord(WORD value)
        {
            const BYTE* p = reinterpret_cast<const BYTE*>(&value);
            m_template.insert(m_template.end(), p, p + sizeof(value));
        }

        void AppendWideString(const wchar_t* text)
        {
            size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);
            const BYTE* p = reinterpret_cast<const BYTE*>(text);
            m_template.insert(m_template.end(), p, p + bytes);
        }

        const CDailyReportPage* m_page = nullptr;
        std::vector<BYTE> m_template;
        CButton m_close;
    };

    BEGIN_MESSAGE_MAP(CReportPreviewDialog, CDialog)
        ON_BN_CLICKED(22001, &CReportPreviewDialog::OnPrevious)
        ON_BN_CLICKED(22002, &CReportPreviewDialog::OnNext)
        ON_WM_PAINT()
    END_MESSAGE_MAP()
}

BEGIN_MESSAGE_MAP(CDailyReportPage, CBasePage)
    ON_BN_CLICKED(IDC_REPORT_SEARCH, &CDailyReportPage::OnSearch)
    ON_BN_CLICKED(IDC_REPORT_PREVIEW, &CDailyReportPage::OnPreview)
    ON_BN_CLICKED(IDC_REPORT_PDF, &CDailyReportPage::OnPdf)
    ON_BN_CLICKED(IDC_REPORT_EXCEL, &CDailyReportPage::OnExcel)
    ON_BN_CLICKED(IDC_REPORT_PRINT, &CDailyReportPage::OnPrint)
END_MESSAGE_MAP()

BOOL CDailyReportPage::CreatePage(CWnd* p, const CRect& r, UINT id)
{
    if (!CBasePage::CreatePage(p, r, id))
        return FALSE;

    AddTitle(_T("일일 생산 보고서"));

    AddLabel(_T("생산일자"), 28, 83, 80);
    m_dateEdit = AddEdit(112, 78, 190, 32);
    m_dateEdit->SetWindowText(CTime::GetCurrentTime().Format(_T("%Y-%m-%d")));
    AddButton(_T("조회"), 324, 76, 92, 36, IDC_REPORT_SEARCH);

    m_totalCard.CreateCard(_T("총 검사\r\n0"), this, CRect(28, 132, 233, 220));
    m_passCard.CreateCard(_T("정상\r\n0"), this, CRect(249, 132, 454, 220));
    m_failCard.CreateCard(_T("NG\r\n0"), this, CRect(470, 132, 675, 220));
    m_rateCard.CreateCard(_T("불량률\r\n0.0 %"), this, CRect(691, 132, 896, 220));

    AddLabel(_T("보고서 데이터 미리보기"), 28, 244, 210, 28);
    // Multiline EDIT scrolling caused stale glyphs/overdraw on some Windows setups.
    // A standard, non-owner-drawn list box paints each report line independently.
    m_previewList = new CListBox();
    if (m_previewList->Create(WS_CHILD | WS_VISIBLE | WS_BORDER |
        WS_VSCROLL | WS_HSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOSEL,
        CRect(28, 274, 896, 552), this, 3606))
    {
        m_previewList->SetFont(&m_normalFont, TRUE);
        // The default listbox item height was computed before the 10pt font
        // was assigned. On DPI-scaled displays it is smaller than the glyphs,
        // so scrolling paints multiple rows on top of one another.
        CClientDC previewDc(m_previewList);
        CFont* previousFont = previewDc.SelectObject(&m_normalFont);
        TEXTMETRIC metrics = {};
        previewDc.GetTextMetrics(&metrics);
        previewDc.SelectObject(previousFont);
        const int textHeight = metrics.tmHeight + metrics.tmExternalLeading + 6;
        const int rowHeight = textHeight > 22 ? textHeight : 22;
        m_previewList->SetItemHeight(0, rowHeight);
    }

    AddButton(_T("인쇄 미리보기"), 360, 578, 132, 38, IDC_REPORT_PREVIEW);
    AddButton(_T("PDF 저장"), 508, 578, 116, 38, IDC_REPORT_PDF);
    AddButton(_T("Excel 저장"), 640, 578, 116, 38, IDC_REPORT_EXCEL);
    AddButton(_T("인쇄"), 772, 578, 124, 38, IDC_REPORT_PRINT);

    RefreshReport();
    return TRUE;
}

CString CDailyReportPage::GetReportDate() const
{
    CString date;
    if (m_dateEdit)
        m_dateEdit->GetWindowText(date);
    date.Trim();
    return date;
}

bool CDailyReportPage::ExtractCount(
    const CString& message,
    LPCTSTR label,
    long long& value)
{
    int pos = message.Find(label);
    if (pos < 0)
        return false;

    pos += static_cast<int>(_tcslen(label));
    while (pos < message.GetLength() &&
        (message[pos] == _T(' ') || message[pos] == _T('\t')))
        ++pos;

    CString digits;
    while (pos < message.GetLength() && _istdigit(message[pos]))
        digits += message[pos++];

    if (digits.IsEmpty())
        return false;

    value = _ttoi64(digits);
    return true;
}

bool CDailyReportPage::LoadServerReport(const CString& date, ReportViewData& report) const
{
    std::string json;
    if (!FetchReportLine(date, json)) return false;
    std::string status, type, hourly, records;
    if (!ReadJsonString(json, "status", status) || status != "OK" ||
        !ReadJsonString(json, "type", type) || type != "REPORT_INSPECTIONS_RESULT") return false;

    long long total = 0, passed = 0, failed = 0;
    long long wheel = 0, tire = 0, both = 0, wrong = 0, unknown = 0;
    if (!ReadJsonLong(json, "total", total) || !ReadJsonLong(json, "passed", passed) ||
        !ReadJsonLong(json, "failed", failed) || !ReadJsonLong(json, "wheel", wheel) ||
        !ReadJsonLong(json, "tire", tire) || !ReadJsonLong(json, "both", both) ||
        !ReadJsonLong(json, "wrong", wrong) ||
        !ReadJsonLong(json, "unknown", unknown) ||
        !ReadJsonString(json, "hourly", hourly) || !ReadJsonString(json, "records", records)) return false;
    report = ReportViewData();
    report.date = date;
    report.fromDatabase = true;
    // Optional fields: old servers remain compatible, but unavailable data is marked.
    report.equipmentStatsAvailable = ReadJsonLong(json, "estop_count", report.estopCount)
        && ReadJsonLong(json, "recheck_count", report.recheckCount)
        && ReadJsonLong(json, "clear_count", report.clearCount)
        && ReadJsonLong(json, "estop_seconds", report.estopSeconds)
        && ReadJsonLong(json, "recheck_seconds", report.recheckSeconds)
        && ReadJsonLong(json, "active_estop", report.activeEstop)
        && ReadJsonLong(json, "active_recheck", report.activeRecheck)
        && ReadJsonLong(json, "yolo_count", report.yoloCount)
        && ReadJsonLong(json, "yolo_seconds", report.yoloSeconds)
        && ReadJsonLong(json, "active_yolo", report.activeYolo);
    if (report.equipmentStatsAvailable)
    {
        ReadJsonLong(json, "estop_review_count", report.estopReviewCount);
        ReadJsonLong(json, "recheck_review_count", report.recheckReviewCount);
        ReadJsonLong(json, "yolo_review_count", report.yoloReviewCount);
        ReadJsonLong(json, "estop_valid_closed", report.estopValidClosed);
        ReadJsonLong(json, "recheck_valid_closed", report.recheckValidClosed);
        ReadJsonLong(json, "yolo_valid_closed", report.yoloValidClosed);
    }
    report.total = total; report.pass = passed; report.fail = failed;
    report.wheelDamage = wheel; report.tireDamage = tire;
    report.bothDamage = both; report.wrongTire = wrong; report.unclassified = unknown;
    report.hourlyLabel = { _T("09"), _T("10"), _T("11"), _T("13"), _T("14"), _T("15"), _T("16"), _T("17") };
    report.hourlyTotal.assign(8, 0);
    report.hourlyPass.assign(8, 0);
    report.hourlyFail.assign(8, 0);
    std::istringstream hours(hourly);
    std::string piece;
    while (std::getline(hours, piece, ';'))
    {
        std::istringstream line(piece);
        std::string v[4];
        if (!std::getline(line, v[0], ',') || !std::getline(line, v[1], ',') ||
            !std::getline(line, v[2], ',') || !std::getline(line, v[3], ',')) continue;
        for (int i = 0; i < 8; ++i)
        {
            if (v[0] == ToUtf8(report.hourlyLabel[i]))
            {
                try {
                    report.hourlyTotal[i] = static_cast<int>(std::stoll(v[1]));
                    report.hourlyPass[i] = static_cast<int>(std::stoll(v[2]));
                    report.hourlyFail[i] = static_cast<int>(std::stoll(v[3]));
                }
                catch (...) {}
                break;
            }
        }
    }
    std::istringstream rows(records);
    while (std::getline(rows, piece))
    {
        if (piece.empty()) continue;
        std::vector<std::string> fields;
        std::istringstream line(piece);
        std::string field;
        while (std::getline(line, field, '|')) fields.push_back(field);
        if (!piece.empty() && piece.back() == '|') fields.push_back("");
        if (fields.size() != 6) continue;
        DefectRecord rec;
        rec.inspectionId = Utf8CStr(fields[0]);
        rec.inspectedAt = Utf8CStr(PercentDecode(fields[1]));
        rec.tireId = Utf8CStr(PercentDecode(fields[2]));
        rec.defectType = DefectTypeDisplay(Utf8CStr(PercentDecode(fields[3])));
        rec.detail = Utf8CStr(PercentDecode(fields[4]));
        rec.imagePath = Utf8CStr(PercentDecode(fields[5]));
        report.defectRecords.push_back(rec);
    }
    return true;
}

void CDailyReportPage::RefreshReport()
{
    const CString date = GetReportDate();
    if (date.IsEmpty())
        return;

    m_filteredHandovers.clear();
    m_total = 0;
    m_pass = 0;
    m_fail = 0;

    const CMainFrame* frame = dynamic_cast<const CMainFrame*>(GetParent());
    if (frame)
    {
        for (const auto& item : frame->GetHandoverItems())
        {
            if (item.workDate != date)
                continue;

            m_filteredHandovers.push_back(item);

            long long total = 0;
            long long passed = 0;
            long long failed = 0;

            if (ExtractCount(item.message, _T("총 검사:"), total))
                m_total += total;
            if (ExtractCount(item.message, _T("정상:"), passed))
                m_pass += passed;
            if (ExtractCount(item.message, _T("불량:"), failed))
                m_fail += failed;
        }
    }

    m_hasServerReport = LoadServerReport(date, m_serverReport);
    UpdateSummaryCards();

    if (m_previewList && ::IsWindow(m_previewList->GetSafeHwnd()))
    {
        CString preview = BuildReportText();
        preview.Replace(_T("\r\r\n"), _T("\r\n"));
        preview.Replace(_T("\r\n"), _T("\n"));
        preview.Replace(_T("\r"), _T("\n"));

        m_previewList->SetRedraw(FALSE);
        m_previewList->ResetContent();
        int widest = 0;
        CClientDC dc(m_previewList);
        CFont* oldFont = dc.SelectObject(&m_normalFont);
        int start = 0;
        while (start <= preview.GetLength())
        {
            const int end = preview.Find(_T('\n'), start);
            const CString line = (end < 0) ? preview.Mid(start) : preview.Mid(start, end - start);
            m_previewList->AddString(line.IsEmpty() ? _T(" ") : line);
            const CSize extent = dc.GetTextExtent(line);
            if (extent.cx > widest) widest = extent.cx;
            if (end < 0) break;
            start = end + 1;
        }
        dc.SelectObject(oldFont);
        m_previewList->SetHorizontalExtent(widest + 24);
        m_previewList->SetTopIndex(0);
        m_previewList->SetRedraw(TRUE);
        m_previewList->RedrawWindow(nullptr, nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW | RDW_ALLCHILDREN);
    }
}

void CDailyReportPage::UpdateSummaryCards()
{
    ReportViewData data = BuildReportViewData();

    CString text;
    text.Format(_T("총 검사\r\n%lld"), data.total);
    m_totalCard.SetText(text);

    text.Format(_T("정상\r\n%lld"), data.pass);
    m_passCard.SetText(text);

    text.Format(_T("NG\r\n%lld"), data.fail);
    m_failCard.SetText(text);

    text.Format(_T("불량률\r\n%.1f %%"), data.failRate);
    m_rateCard.SetText(text);
}

CDailyReportPage::ReportViewData CDailyReportPage::BuildReportViewData() const
{
    ReportViewData data;
    if (m_hasServerReport)
        data = m_serverReport;
    data.date = GetReportDate();
    data.author = g_userName.IsEmpty() ? _T("관리자") : g_userName;
    data.shift = _T("전체");
    data.generatedAt = CTime::GetCurrentTime().Format(_T("%Y-%m-%d %H:%M"));
    data.handovers = m_filteredHandovers;
    if (!m_hasServerReport)
    {
        // Never invent production quantities when the central database is unavailable.
        data.sample = true;
        data.total = 0; data.pass = 0; data.fail = 0;
        data.wheelDamage = 0; data.tireDamage = 0;
        data.bothDamage = 0; data.wrongTire = 0; data.unclassified = 0;
        data.hourlyTotal.assign(data.hourlyTotal.size(), 0);
        data.hourlyPass.assign(data.hourlyPass.size(), 0);
        data.hourlyFail.assign(data.hourlyFail.size(), 0);
        data.defectRecords.clear();
    }
    // 기존 NG 기록 중 세부 유형이 없는 건을 미분류로 표시합니다.
    const long long categorized = data.wheelDamage + data.tireDamage +
        data.bothDamage + data.wrongTire + data.unclassified;
    if (data.fail > categorized)
        data.unclassified += data.fail - categorized;
    data.failRate = data.total > 0 ? data.fail * 100.0 / data.total : 0.0;
    return data;
}

CString CDailyReportPage::BuildReportText() const
{
    ReportViewData d = BuildReportViewData();

    CString text;
    text.Format(
        _T("타이어 생산 품질 보고서\r\n")
        _T("생산일자 : %s\r\n")
        _T("작성자   : %s\r\n")
        _T("총 검사  : %lld\r\n")
        _T("정상     : %lld\r\n")
        _T("NG       : %lld\r\n")
        _T("불량률   : %.1f %%\r\n")
        _T("인수인계 : %d건\r\n\r\n")
        _T("※ PDF/인쇄 미리보기 버튼에서 실제 보고서 디자인을 확인할 수 있습니다.%s"),
        d.date.GetString(),
        d.author.GetString(),
        d.total,
        d.pass,
        d.fail,
        d.failRate,
        static_cast<int>(d.handovers.size()),
        d.sample ? _T("\r\n※ 서버 통계 미수신: 수량은 0으로 표시됩니다. 실제 생산실적으로 사용하지 마세요.") : _T(""));
    text += _T("\r\n[불량률 분석]\r\n");
    CString analysis;
    analysis.Format(_T("전체 불량률 : %.1f%% (NG %lld건 / 총 %lld건)\r\n"),
        d.failRate, d.fail, d.total);
    text += analysis;
    const LPCTSTR labels[] = {
        _T("휠 손상"), _T("타이어 고무 손상"),
        _T("휠·타이어 동시 손상"), _T("이종 타이어(B형)"), _T("미분류")
    };
    const long long counts[] = {
        d.wheelDamage, d.tireDamage, d.bothDamage, d.wrongTire, d.unclassified
    };
    for (int i = 0; i < 5; ++i)
    {
        CString row;
        row.Format(_T("  %s : %lld건 / NG 중 %.1f%% / 전체 중 %.1f%%\r\n"),
            labels[i], counts[i],
            d.fail > 0 ? counts[i] * 100.0 / d.fail : 0.0,
            d.total > 0 ? counts[i] * 100.0 / d.total : 0.0);
        text += row;
    }
    int peakIndex = -1;
    double peakRate = -1.0;
    for (size_t i = 0; i < d.hourlyLabel.size() &&
        i < d.hourlyTotal.size() && i < d.hourlyFail.size(); ++i)
    {
        if (d.hourlyTotal[i] <= 0) continue;
        const double rate = d.hourlyFail[i] * 100.0 / d.hourlyTotal[i];
        if (rate > peakRate) { peakRate = rate; peakIndex = static_cast<int>(i); }
    }
    if (peakIndex >= 0)
    {
        CString peak;
        peak.Format(_T("시간대별 최고 불량률 : %s시 %.1f%% (%d/%d건)\r\n"),
            d.hourlyLabel[peakIndex].GetString(), peakRate,
            d.hourlyFail[peakIndex], d.hourlyTotal[peakIndex]);
        text += peak;
    }
    text += d.total == 0 ? _T("검사 기록 없음\r\n") :
        (d.failRate > 7.0 ? _T("시연 기준 7.0% 초과: 주의\r\n")
            : _T("시연 기준 7.0% 이하: 정상\r\n"));
    text += _T("\r\n[설비 경고 및 비상정지 분석]\r\n");
    if (d.equipmentStatsAvailable)
    {
        CString eq;
        eq.Format(_T("비상정지: %lld회 / 완료된 세션 지속 시간 %lld초 (진행 중 %lld건)\r\n")
            _T("재확인: %lld회 / 완료된 세션 지속 시간 %lld초 (진행 중 %lld건)\r\n")
            _T("YOLO 경고: %lld회 / 완료된 세션 지속 시간 %lld초 (진행 중 %lld건)\r\n")
            _T("경고 해제: %lld회\r\n"), d.estopCount, d.estopSeconds, d.activeEstop,
            d.recheckCount, d.recheckSeconds, d.activeRecheck,
            d.yoloCount, d.yoloSeconds, d.activeYolo, d.clearCount);
        text += eq;
        CString avg;
        const long long estopClosed = d.estopValidClosed;
        const long long recheckClosed = d.recheckValidClosed;
        const long long yoloClosed = d.yoloValidClosed;
        avg.Format(_T("평균 지속 시간(종료 건): 비상정지 %.1f초 / 재확인 %.1f초 / YOLO %.1f초\r\n"),
            estopClosed > 0 ? static_cast<double>(d.estopSeconds) / estopClosed : 0.0,
            recheckClosed > 0 ? static_cast<double>(d.recheckSeconds) / recheckClosed : 0.0,
            yoloClosed > 0 ? static_cast<double>(d.yoloSeconds) / yoloClosed : 0.0);
        text += avg;
        if (d.estopReviewCount || d.recheckReviewCount || d.yoloReviewCount)
        {
            CString review;
            review.Format(_T("※ 지속 시간 검토 필요(10분 초과): 비상정지 %lld건 / 재확인 %lld건 / YOLO %lld건\r\n")
                _T("  해당 세션은 발생 횟수에 포함, 지속 시간 및 평균에서는 제외됩니다.\r\n"),
                d.estopReviewCount, d.recheckReviewCount, d.yoloReviewCount);
            text += review;
        }
    }
    else text += _T("설비 이벤트 집계 미수신 (중앙 서버 업데이트 필요)\r\n");
    if (!d.fromDatabase) text += _T("※ DB 검사 이력과 연동되지 않은 시연/인수인계 자료입니다.\r\n");
    return text;
}

void CDailyReportPage::OnSearch()
{
    CString date = GetReportDate();
    if (date.GetLength() != 10 || date[4] != _T('-') || date[7] != _T('-'))
    {
        AfxMessageBox(_T("생산일자를 YYYY-MM-DD 형식으로 입력하세요."));
        return;
    }

    RefreshReport();
}

void CDailyReportPage::OnPreview()
{
    RefreshReport();
    CReportPreviewDialog dialog(this, this);
    dialog.DoModal();
}

bool CDailyReportPage::PrintReportToDC(
    HDC hdc,
    const CString& documentName,
    const CString* outputPath) const
{
    if (!hdc)
        return false;

    DOCINFO info = {};
    info.cbSize = sizeof(info);
    info.lpszDocName = documentName;
    info.lpszOutput = outputPath ? outputPath->GetString() : nullptr;

    if (::StartDoc(hdc, &info) <= 0)
        return false;

    CDC dc;
    dc.Attach(hdc);
    const CRect pageRect(0, 0, dc.GetDeviceCaps(HORZRES), dc.GetDeviceCaps(VERTRES));
    const int pageCount = GetReportPageCount();
    for (int page = 0; page < pageCount; ++page)
    {
        if (::StartPage(hdc) <= 0)
        {
            dc.Detach();
            ::AbortDoc(hdc);
            return false;
        }
        DrawReportPage(dc, pageRect, page);
        if (::EndPage(hdc) <= 0)
        {
            dc.Detach();
            ::AbortDoc(hdc);
            return false;
        }
    }
    dc.Detach();

    return ::EndDoc(hdc) > 0;
}

void CDailyReportPage::DrawReport(CDC& dc, const CRect& pageRect) const
{
    ReportViewData d = BuildReportViewData();

    dc.SetBkMode(TRANSPARENT);
    FillRectColor(dc, pageRect, RGB(255, 255, 255));

    // 모든 좌표를 A4 기준 1000 x 1414 논리 좌표로 계산하고 실제 페이지 크기에 비례시킵니다.
    const int U = max(1, pageRect.Width() / 1000);
    auto X = [&](int x) { return pageRect.left + MulDiv(x, pageRect.Width(), 1000); };
    auto Y = [&](int y) { return pageRect.top + MulDiv(y, pageRect.Height(), 1414); };
    auto R = [&](int l, int t, int r, int b) { return CRect(X(l), Y(t), X(r), Y(b)); };

    // 실제 그려지는 A4 너비에 맞춰 글꼴을 축소/확대합니다.
    // 디스플레이 DPI만 사용하면 미리보기의 글꼴이 페이지보다 커져 겹칩니다.
    const int scaledDpi = max(40, MulDiv(96, pageRect.Width(), 1000));
    auto MakeFont = [&](CFont& font, int pt, int weight)
        {
            font.CreateFont(
                -max(1, MulDiv(pt, scaledDpi, 72)), 0, 0, 0, weight,
                FALSE, FALSE, 0, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, _T("맑은 고딕"));
        };

    CFont f7, f8, f9, f10, f11b, f13b, f18b, f23b;
    MakeFont(f7, 7, FW_NORMAL);
    MakeFont(f8, 8, FW_NORMAL);
    MakeFont(f9, 9, FW_NORMAL);
    MakeFont(f10, 10, FW_NORMAL);
    MakeFont(f11b, 11, FW_BOLD);
    MakeFont(f13b, 13, FW_BOLD);
    MakeFont(f18b, 18, FW_BOLD);
    MakeFont(f23b, 23, FW_BOLD);

    // 1) Header
    FillRectColor(dc, R(0, 0, 1000, 170), C_NAVY);
    DrawTextEx(dc, _T("TIRE PRODUCTION"), R(85, 40, 430, 70), f11b,
        RGB(255, 255, 255), DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    DrawTextEx(dc, _T("일일 생산 · 품질 보고서"), R(85, 75, 700, 130), f18b,
        RGB(255, 255, 255), DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    DrawTextEx(dc, _T("DAILY PRODUCTION QUALITY REPORT"), R(655, 44, 915, 72), f7,
        RGB(255, 255, 255), DT_RIGHT | DT_SINGLELINE | DT_VCENTER);

    CString equipmentLine;
    if (d.equipmentStatsAvailable)
        equipmentLine.Format(_T("ESTOP %lld회/%lld초  RECHECK %lld회/%lld초  YOLO %lld회/%lld초"),
            d.estopCount, d.estopSeconds, d.recheckCount, d.recheckSeconds, d.yoloCount, d.yoloSeconds);
    else equipmentLine = _T("설비 이벤트: 중앙 서버 집계 미수신");
    DrawTextEx(dc, equipmentLine, R(85, 133, 915, 158), f8,
        RGB(255, 255, 255), DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    CString reportNo;
    reportNo.Format(_T("REPORT NO. %s / REV.01"), d.date.GetString());
    DrawTextEx(dc, reportNo, R(710, 102, 915, 127), f7,
        RGB(220, 231, 243), DT_RIGHT | DT_SINGLELINE | DT_VCENTER);

    if (d.sample)
    {
        FillRoundRect(dc, R(805, 132, 915, 158), RGB(255, 245, 220), RGB(255, 245, 220), 8 * U);
        DrawTextEx(dc, _T("SAMPLE DATA"), R(810, 132, 910, 158), f7,
            C_ORANGE, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }

    // 2) Meta info
    FillRoundRect(dc, R(85, 220, 915, 280), C_PAPER, C_PAPER, 12 * U);
    struct Meta { CString key; CString value; int x1; int x2; };
    std::vector<Meta> metas = {
        { _T("생산일자"), d.date, 110, 280 },
        { _T("작성자"), d.author, 330, 470 },
        { _T("근무"), d.shift, 535, 650 },
        { _T("출력일시"), d.generatedAt, 700, 890 }
    };
    for (const auto& m : metas)
    {
        DrawTextEx(dc, m.key, R(m.x1, 230, m.x2, 247), f7, C_MUTED,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        DrawTextEx(dc, m.value, R(m.x1, 247, m.x2, 273), f10, C_INK,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }

    // 3) KPI cards
    struct Kpi { CString title; CString value; CString unit; COLORREF color; };
    CString sTotal, sPass, sFail, sRate;
    sTotal.Format(_T("%lld"), d.total);
    sPass.Format(_T("%lld"), d.pass);
    sFail.Format(_T("%lld"), d.fail);
    sRate.Format(_T("%.1f"), d.failRate);
    std::vector<Kpi> kpis = {
        { _T("총 검사"), sTotal, _T("EA"), C_BLUE },
        { _T("정상"), sPass, _T("EA"), C_GREEN },
        { _T("NG"), sFail, _T("EA"), C_RED },
        { _T("불량률"), sRate, _T("%"), C_ORANGE }
    };

    for (int i = 0; i < 4; ++i)
    {
        int left = 85 + i * 210;
        int right = left + 195;
        FillRoundRect(dc, R(left, 330, right, 445), RGB(255, 255, 255), C_LINE, 12 * U);
        FillRoundRect(dc, R(left, 330, right, 338), kpis[i].color, kpis[i].color, 6 * U);
        DrawTextEx(dc, kpis[i].title, R(left + 18, 355, right - 15, 380), f8, C_MUTED,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        DrawTextEx(dc, kpis[i].value, R(left + 18, 385, right - 55, 430), f18b, C_INK,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        DrawTextEx(dc, kpis[i].unit, R(right - 55, 395, right - 15, 428), f8, C_MUTED,
            DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    }

    // 4) 시간대별 생산 현황
    DrawSectionTitle(dc, 1, _T("시간대별 생산 현황"), X(85), X(915), Y(485), U, f11b, f13b);

    FillRoundRect(dc, R(85, 535, 620, 720), C_PAPER, C_PAPER, 12 * U);
    int maxValue = 1;
    for (int v : d.hourlyTotal)
        maxValue = max(maxValue, v);

    const int chartLeft = 120;
    const int chartBottom = 695;
    const int barW = 48;
    const int gap = 34;
    for (size_t i = 0; i < d.hourlyTotal.size() && i < 6; ++i)
    {
        int x = chartLeft + static_cast<int>(i) * (barW + gap);
        int h = MulDiv(d.hourlyTotal[i], 115, maxValue);
        FillRoundRect(dc, R(x, chartBottom - h, x + barW, chartBottom), C_BLUE, C_BLUE, 8 * U);

        CString value;
        value.Format(_T("%d"), d.hourlyTotal[i]);
        DrawTextEx(dc, value, R(x - 5, chartBottom - h - 28, x + barW + 5, chartBottom - h), f8,
            C_INK, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

        CString label = d.hourlyLabel[i] + _T("시");
        DrawTextEx(dc, label, R(x - 8, 696, x + barW + 8, 718), f7,
            C_MUTED, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }

    // 우측 품질 카드
    FillRoundRect(dc, R(650, 535, 915, 720), RGB(255, 255, 255), C_LINE, 12 * U);
    DrawTextEx(dc, _T("정상 / 불량 비율"), R(680, 560, 880, 585), f8, C_MUTED,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    FillRoundRect(dc, R(680, 600, 885, 620), RGB(233, 238, 244), RGB(233, 238, 244), 10 * U);
    const double passFraction = d.total > 0 ? static_cast<double>(d.pass) / d.total : 0.0;
    const int passWidth = static_cast<int>(205.0 * (std::max)(0.0, (std::min)(1.0, passFraction)));
    if (passWidth > 0)
        FillRoundRect(dc, R(680, 600, 680 + passWidth, 620), C_GREEN, C_GREEN, 10 * U);

    CString passPct, failPct;
    double passRate = d.total > 0 ? 100.0 - d.failRate : 0.0;
    passPct.Format(_T("정상 %.1f%%"), passRate);
    failPct.Format(_T("NG %.1f%%"), d.failRate);
    DrawTextEx(dc, passPct, R(680, 640, 800, 670), f11b, C_INK,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    DrawTextEx(dc, failPct, R(800, 640, 885, 670), f10, C_RED,
        DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    CString qualityState;
    if (d.total == 0) qualityState = _T("검사 기록 없음");
    else qualityState = d.failRate > 7.0 ? _T("기준 초과 · 주의") : _T("기준 충족 · 정상");
    CString qualitySummary(_T("시연 기준 7.0% · "));
    qualitySummary += qualityState;
    DrawTextEx(dc, qualitySummary, R(680, 680, 895, 704), f7,
        d.total > 0 && d.failRate > 7.0 ? C_RED : C_MUTED,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    // 5) 불량 유형별 분석: 집계값이 0이어도 항상 표를 표시합니다.
    DrawSectionTitle(dc, 2, _T("불량 유형별 분석"), X(85), X(915), Y(755), U, f11b, f13b);
    // 식별 문구는 표와 별개로 출력되어 이전 EXE 또는 캐시된 PDF를 구분합니다.
    DrawTextEx(dc, _T("휠 / 타이어 / 동시 손상 / 이종 타이어 / 미분류"),
        R(470, 759, 915, 780), f7, C_MUTED,
        DT_RIGHT | DT_SINGLELINE | DT_VCENTER);

    // 배경과 테이블을 단일 출력 흐름에서 그려 PDF 프린터에서도 누락되지 않도록 합니다.
    FillRectColor(dc, R(85, 795, 915, 1010), RGB(255, 255, 255));
    const int defectTop = 805;
    const int defectRowH = 29;
    const int defectCols[] = { 85, 338, 447, 573, 718, 915 };
    const CString defectHeads[] = {
        _T("불량 유형"), _T("건수"), _T("NG 내 비율"),
        _T("전체 대비"), _T("NG 구성 비중")
    };
    // 헤더와 행은 프린터 드라이버에서도 호환성이 높은 단순 사각형으로 그립니다.
    FillRectColor(dc, R(85, defectTop, 915, defectTop + defectRowH), C_NAVY);
    for (int column = 0; column < 5; ++column)
    {
        DrawTextEx(dc, defectHeads[column],
            R(defectCols[column] + 4, defectTop, defectCols[column + 1] - 4,
                defectTop + defectRowH),
            f8, RGB(255, 255, 255), DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }

    const CString defectNames[] = {
        _T("휠 손상"), _T("타이어 고무 손상"),
        _T("휠·타이어 동시 손상"), _T("이종 타이어(B형)"), _T("미분류")
    };
    const long long defectCounts[] = {
        d.wheelDamage, d.tireDamage, d.bothDamage, d.wrongTire, d.unclassified
    };
    const COLORREF defectColors[] = { C_BLUE, C_ORANGE, C_RED, RGB(145, 107, 203), C_MUTED };

    for (int index = 0; index < 5; ++index)
    {
        const int top = defectTop + defectRowH * (index + 1);
        const int bottom = top + defectRowH;
        FillRectColor(dc, R(85, top, 915, bottom),
            index % 2 == 0 ? RGB(255, 255, 255) : C_PAPER);

        const double ngRatio = d.fail > 0
            ? static_cast<double>(defectCounts[index]) * 100.0 / d.fail : 0.0;
        const double totalRatio = d.total > 0
            ? static_cast<double>(defectCounts[index]) * 100.0 / d.total : 0.0;
        CString countText, ngText, totalText;
        countText.Format(_T("%lld건"), defectCounts[index]);
        ngText.Format(_T("%.1f%%"), ngRatio);
        totalText.Format(_T("%.1f%%"), totalRatio);
        const CString values[] = { defectNames[index], countText, ngText, totalText };
        for (int column = 0; column < 4; ++column)
        {
            DrawTextEx(dc, values[column],
                R(defectCols[column] + 4, top,
                    defectCols[column + 1] - 4, bottom),
                f8, C_INK, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        }

        // 각 NG 유형의 비율을 같은 행에 표시합니다.
        FillRectColor(dc, R(740, top + 12, 898, top + 22), RGB(225, 234, 244));
        const double boundedRatio = (std::max)(0.0, (std::min)(100.0, ngRatio));
        const int barWidth = static_cast<int>(158.0 * boundedRatio / 100.0);
        if (barWidth > 0)
            FillRectColor(dc, R(740, top + 12, 740 + barWidth, top + 22),
                defectColors[index]);
    }

    // 외곽선은 표와 숫자가 그려진 뒤에 배치합니다.
    CPen defectPen(PS_SOLID, 1, C_LINE);
    CPen* oldPen = dc.SelectObject(&defectPen);
    CBrush* previousBrush = static_cast<CBrush*>(dc.SelectStockObject(NULL_BRUSH));
    dc.Rectangle(R(85, defectTop, 915, defectTop + defectRowH * 6));
    dc.SelectObject(previousBrush);
    dc.SelectObject(oldPen);

    CString defectSummary;
    defectSummary.Format(
        _T("NG 총 %lld건 · 전체 불량률 %.1f%% · 동시 손상은 1건으로 집계"),
        d.fail, d.failRate);
    DrawTextEx(dc, defectSummary, R(96, 982, 906, 1008), f8, C_MUTED,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    // 03: 설비 이벤트 분석 - 인수인계와 별도 영역
    DrawSectionTitle(dc, 3, _T("설비 이벤트 분석"), X(85), X(915), Y(1015), U, f11b, f13b);
    FillRoundRect(dc, R(85, 1060, 915, 1208), RGB(248, 251, 255), C_LINE, 9 * U);
    if (d.equipmentStatsAvailable)
    {
        CString lines[4];
        lines[0].Format(_T("비상정지 ESTOP  %lld회  |  유효 지속 %lld초  |  진행 중 %lld건"),
            d.estopCount, d.estopSeconds, d.activeEstop);
        lines[1].Format(_T("재확인 RECHECK  %lld회  |  유효 지속 %lld초  |  진행 중 %lld건"),
            d.recheckCount, d.recheckSeconds, d.activeRecheck);
        lines[2].Format(_T("YOLO 경고  %lld회  |  유효 지속 %lld초  |  진행 중 %lld건"),
            d.yoloCount, d.yoloSeconds, d.activeYolo);
        lines[3].Format(_T("경고 해제 %lld회  |  지속 시간 검토: ESTOP %lld / RECHECK %lld / YOLO %lld건"),
            d.clearCount, d.estopReviewCount, d.recheckReviewCount, d.yoloReviewCount);
        for (int i = 0; i < 4; ++i)
            DrawTextEx(dc, lines[i], R(104, 1072 + i * 31, 895, 1100 + i * 31),
                i == 3 ? f7 : f8, i == 3 ? C_MUTED : C_INK,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
    else
        DrawTextEx(dc, _T("설비 이벤트 통계 미수신 - 중앙 서버 연결을 확인하세요."),
            R(104, 1100, 895, 1140), f9, C_RED, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 04, 05: 가변 길이 항목은 다음 페이지에서 누락 없이 출력
    DrawSectionTitle(dc, 4, _T("인수인계 및 조치 사항"), X(85), X(915), Y(1225), U, f11b, f13b);
    CString handoverInfo;
    handoverInfo.Format(_T("등록 %d건 · 상세 내역은 다음 페이지에 표시됩니다."),
        static_cast<int>(d.handovers.size()));
    DrawTextEx(dc, handoverInfo, R(104, 1264, 895, 1300), f9, C_MUTED,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    DrawTextEx(dc, _T("05 최근 불량 상세도 인수인계 내역 다음에 이어집니다."),
        R(104, 1304, 895, 1336), f8, C_MUTED, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    // Footer
    CPen footerPen(PS_SOLID, 1, C_LINE);
    oldPen = dc.SelectObject(&footerPen);
    dc.MoveTo(X(85), Y(1360));
    dc.LineTo(X(915), Y(1360));
    dc.SelectObject(oldPen);

    // PDF/인쇄에 표시되는 버전 표식. 이전 실행 파일/이전 PDF와 구별할 때 사용합니다.
    DrawTextEx(dc, _T("Tire Production Monitoring System · REPORT PDF EVENT V8 20261022"),
        R(85, 1370, 700, 1395), f7, C_MUTED,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    CString pageNumber;
    pageNumber.Format(_T("Page 1 / %d"), GetReportPageCount());
    DrawTextEx(dc, pageNumber, R(760, 1370, 915, 1395), f7, C_MUTED,
        DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
}

int CDailyReportPage::GetReportPageCount() const
{
    const ReportViewData d = BuildReportViewData();
    // 한 페이지에 인수인계 6건. 나머지는 다음 페이지로 넘깁니다.
    const int handoverPages = max(1, (static_cast<int>(d.handovers.size()) + 5) / 6);
    // 최근 불량 상세는 인수인계 페이지와 별도 페이지로 출력합니다.
    return 1 + handoverPages + 1;
}

void CDailyReportPage::DrawReportPage(CDC& dc, const CRect& rect, int pageIndex) const
{
    if (pageIndex == 0) { DrawReport(dc, rect); return; }
    const ReportViewData d = BuildReportViewData();
    const int handoverPages = ((static_cast<int>(d.handovers.size()) + 5) / 6 > 1)
        ? (static_cast<int>(d.handovers.size()) + 5) / 6 : 1;
    const int totalPages = 1 + handoverPages + 1;
    auto X = [&](int x) { return rect.left + MulDiv(x, rect.Width(), 1000); };
    auto Y = [&](int y) { return rect.top + MulDiv(y, rect.Height(), 1414); };
    auto R = [&](int l, int t, int r, int b) { return CRect(X(l), Y(t), X(r), Y(b)); };
    // Use direct MFC font creation instead of a local generic lambda.
    // This avoids compiler/parser conflicts in some Visual Studio MFC configurations.
    const int dpiY = dc.GetDeviceCaps(LOGPIXELSY);
    const int fontScale = (dpiY > 0 ? dpiY : 96);
    CFont bodyFont;
    CFont smallFont;
    CFont reportTitleFont;
    bodyFont.CreateFont(-MulDiv(10, fontScale, 72), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"맑은 고딕");
    smallFont.CreateFont(-MulDiv(8, fontScale, 72), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"맑은 고딕");
    reportTitleFont.CreateFont(-MulDiv(18, fontScale, 72), 0, 0, 0, FW_BOLD,
        FALSE, FALSE, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"맑은 고딕");
    FillRectColor(dc, rect, RGB(255, 255, 255));
    FillRectColor(dc, R(0, 0, 1000, 166), C_NAVY);
    DrawTextEx(dc, _T("TIRE PRODUCTION | DAILY QUALITY REPORT"), R(85, 35, 915, 70),
        smallFont, RGB(255,255,255), DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    CString pageTitle = pageIndex <= handoverPages
        ? _T("04  인수인계 및 조치 사항") : _T("05  최근 불량 상세");
    DrawTextEx(dc, pageTitle, R(85, 76, 915, 135), reportTitleFont, RGB(255,255,255),
        DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    CString meta;
    meta.Format(_T("생산일자  %s   |   작성자  %s"), d.date.GetString(), d.author.GetString());
    DrawTextEx(dc, meta, R(85, 190, 915, 240), bodyFont, C_MUTED,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    if (pageIndex <= handoverPages)
    {
        const int first = (pageIndex - 1) * 6;
        const int last = (first + 6 < static_cast<int>(d.handovers.size()))
            ? first + 6 : static_cast<int>(d.handovers.size());
        if (d.handovers.empty())
            DrawTextEx(dc, _T("등록된 인수인계 내역이 없습니다."), R(105, 290, 900, 345),
                bodyFont, C_MUTED, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        for (int i = first; i < last; ++i)
        {
            const HandoverItem& item = d.handovers[i];
            const int top = 260 + (i - first) * 176;
            const COLORREF accent = item.priority == _T("URGENT") ? C_RED :
                (item.priority == _T("WARNING") ? C_ORANGE : C_BLUE);
            FillRoundRect(dc, R(85, top, 915, top + 158), RGB(248, 251, 255), C_LINE, 9);
            CString label;
            label.Format(_T("[%s] %s 근무  |  작성자 %s  |  등록 %s"),
                HandoverPriorityName(item.priority).GetString(),
                HandoverShiftName(item.shiftType).GetString(),
                item.userName.GetString(), item.createdAt.GetString());
            DrawTextEx(dc, label, R(108, top + 12, 895, top + 40), smallFont, accent,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            CString msg = item.message;
            msg.Replace(_T("\r\n"), _T("\n"));
            DrawTextEx(dc, msg, R(108, top + 43, 890, top + 113), bodyFont, C_INK,
                DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
            CString checked;
            if (item.checked)
                checked.Format(_T("확인 완료  |  확인자 %s  |  확인 시각 %s"),
                    item.checkedBy.GetString(), item.checkedAt.GetString());
            else checked = _T("확인 대기");
            DrawTextEx(dc, checked, R(108, top + 119, 895, top + 147), smallFont, C_MUTED,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }
    }
    else
    {
        if (d.defectRecords.empty())
            DrawTextEx(dc, _T("조회된 NG 상세 기록이 없습니다."), R(105, 280, 900, 330),
                bodyFont, C_MUTED, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        const int count = (static_cast<int>(d.defectRecords.size()) < 24)
            ? static_cast<int>(d.defectRecords.size()) : 24;
        for (int i = 0; i < count; ++i)
        {
            const auto& rec = d.defectRecords[i];
            const int y = 255 + i * 44;
            if (i % 2 == 0) FillRectColor(dc, R(85, y, 915, y + 40), C_PAPER);
            CString detail = rec.tireId + _T("  |  ") + rec.defectType + _T("  |  ") + OneLine(rec.detail);
            DrawTextEx(dc, detail, R(104, y + 4, 895, y + 37), smallFont, C_INK,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }
        if (static_cast<int>(d.defectRecords.size()) > count)
            DrawTextEx(dc, _T("전체 불량 상세 이력은 Excel 내보내기에서 확인할 수 있습니다."),
                R(104, 1330, 895, 1350), smallFont, C_MUTED, DT_LEFT | DT_SINGLELINE);
    }
    CString footer;
    footer.Format(_T("Tire Production Monitoring System  |  REPORT PDF EVENT V8  |  Page %d / %d"),
        pageIndex + 1, totalPages);
    DrawTextEx(dc, footer, R(85, 1360, 915, 1395), smallFont, C_MUTED,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER);
}

void CDailyReportPage::OnPrint()
{
    RefreshReport();

    CPrintDialog dialog(FALSE, PD_ALLPAGES | PD_USEDEVMODECOPIESANDCOLLATE, this);
    if (dialog.DoModal() != IDOK)
        return;

    HDC hdc = dialog.GetPrinterDC();
    if (!hdc)
    {
        AfxMessageBox(_T("프린터 DC를 가져오지 못했습니다."));
        return;
    }

    bool ok = PrintReportToDC(hdc, _T("타이어 생산 품질 보고서"));
    ::DeleteDC(hdc);

    if (!ok)
        AfxMessageBox(_T("인쇄 중 오류가 발생했습니다."));
}

void CDailyReportPage::OnPdf()
{
    RefreshReport();

    CString defaultName;
    defaultName.Format(_T("Tire_Report_%s.pdf"), GetReportDate().GetString());

    CFileDialog dialog(
        FALSE, _T("pdf"), defaultName,
        OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT,
        _T("PDF 파일 (*.pdf)|*.pdf||"), this);

    if (dialog.DoModal() != IDOK)
        return;

    CString path = dialog.GetPathName();

    HDC hdc = ::CreateDC(
        _T("WINSPOOL"),
        _T("Microsoft Print to PDF"),
        nullptr,
        nullptr);

    if (!hdc)
    {
        AfxMessageBox(_T("Microsoft Print to PDF 프린터를 찾을 수 없습니다."));
        return;
    }

    bool ok = PrintReportToDC(hdc, _T("Tire Production Quality Report"), &path);
    ::DeleteDC(hdc);

    AfxMessageBox(ok ? _T("PDF 저장이 완료되었습니다.") : _T("PDF 저장에 실패했습니다."));
}

bool CDailyReportPage::ExportExcelXlsx(const CString& path) const
{
    const ReportViewData d = BuildReportViewData();

    // 공유 문자열을 사용합니다. (HanCell 2022의 inlineStr 표시 호환성 문제 회피)
    std::map<std::wstring, unsigned int> sharedIndex;
    std::vector<CString> sharedStrings;
    size_t sharedCount = 0;
    auto textCell = [&](LPCTSTR ref, const CString& value, int style = 0) -> CString
        {
            const std::wstring key(value.GetString(), value.GetLength());
            auto it = sharedIndex.find(key);
            unsigned int index = 0;
            if (it == sharedIndex.end())
            {
                index = static_cast<unsigned int>(sharedStrings.size());
                sharedStrings.push_back(value);
                sharedIndex.emplace(key, index);
            }
            else index = it->second;
            ++sharedCount;
            CString c;
            c.Format(_T("<c r=\"%s\" s=\"%d\" t=\"s\"><v>%u</v></c>"), ref, style, index);
            return c;
        };
    auto numCell = [](LPCTSTR ref, long long number, int style = 0) -> CString
        {
            CString result;
            result.Format(_T("<c r=\"%s\" s=\"%d\"><v>%lld</v></c>"), ref, style, number);
            return result;
        };
    auto fmtPercent = [](long long numerator, long long denominator) -> CString
        {
            CString value;
            value.Format(_T("%.1f%%"), denominator > 0 ? numerator * 100.0 / denominator : 0.0);
            return value;
        };
    auto cellRef = [](wchar_t col, int row) -> CString
        {
            CString value;
            value.Format(_T("%c%d"), col, row);
            return value;
        };
    const CString open = _T("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>");
    const CString wsNs = _T("<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">");
    const CString pageMargins = _T("<pageMargins left=\"0.3\" right=\"0.3\" top=\"0.5\" bottom=\"0.5\" header=\"0.3\" footer=\"0.3\"/>");
    const CString note = d.fromDatabase
        ? _T("데이터 기준: 서버 SQLite 실제 검사 이력 / 날짜는 한국시간(UTC+9)")
        : _T("SAMPLE DATA | 시연/인수인계 수치이며 DB 검사 이력과 연동되지 않았습니다.");

    CString sheet1 = open + wsNs +
        _T("<dimension ref=\"A1:H21\"/><sheetViews><sheetView workbookViewId=\"0\"/></sheetViews>")
        _T("<sheetFormatPr defaultRowHeight=\"22\"/>")
        _T("<cols><col min=\"1\" max=\"1\" width=\"17\" customWidth=\"1\"/>")
        _T("<col min=\"2\" max=\"8\" width=\"16\" customWidth=\"1\"/></cols><sheetData>");
    sheet1 += _T("<row r=\"1\" ht=\"40\" customHeight=\"1\">") + textCell(_T("A1"), _T("타이어 생산 · 품질 보고서"), 1) + _T("</row>");
    sheet1 += _T("<row r=\"3\" ht=\"27\" customHeight=\"1\">") + textCell(_T("A3"), _T("TIRE PRODUCTION QUALITY REPORT"), 3) + _T("</row>");
    sheet1 += _T("<row r=\"4\">") + textCell(_T("A4"), note, 9) + _T("</row>");
    sheet1 += _T("<row r=\"6\" ht=\"27\" customHeight=\"1\">");
    sheet1 += textCell(_T("A6"), _T("총 검사"), 2) + textCell(_T("C6"), _T("정상"), 2)
        + textCell(_T("E6"), _T("NG"), 2) + textCell(_T("G6"), _T("불량률"), 2) + _T("</row>");
    sheet1 += _T("<row r=\"7\" ht=\"44\" customHeight=\"1\">");
    sheet1 += numCell(_T("A7"), d.total, 8) + numCell(_T("C7"), d.pass, 8)
        + numCell(_T("E7"), d.fail, 8) + textCell(_T("G7"), fmtPercent(d.fail, d.total), 8) + _T("</row>");
    sheet1 += _T("<row r=\"9\" ht=\"28\" customHeight=\"1\">") + textCell(_T("A9"), _T("보고서 기본 정보"), 3) + _T("</row>");
    sheet1 += _T("<row r=\"10\">") + textCell(_T("A10"), _T("생산일자"), 4) + textCell(_T("C10"), d.date)
        + textCell(_T("E10"), _T("작성자"), 4) + textCell(_T("G10"), d.author) + _T("</row>");
    sheet1 += _T("<row r=\"11\">") + textCell(_T("A11"), _T("근무 구분"), 4) + textCell(_T("C11"), d.shift)
        + textCell(_T("E11"), _T("생성 일시"), 4) + textCell(_T("G11"), d.generatedAt) + _T("</row>");
    sheet1 += _T("<row r=\"13\" ht=\"28\" customHeight=\"1\">") + textCell(_T("A13"), _T("불량 유형별 분석"), 3) + _T("</row>");
    sheet1 += _T("<row r=\"14\" ht=\"26\" customHeight=\"1\">") + textCell(_T("A14"), _T("불량 유형"), 2)
        + textCell(_T("E14"), _T("건수"), 2) + textCell(_T("G14"), _T("NG 내 비중"), 2) + _T("</row>");
    const CString typeNames[] = { _T("휠 손상"), _T("타이어 고무 손상"), _T("휠·타이어 동시 손상"), _T("이종 타이어(B형)"), _T("미분류") };
    const long long typeCounts[] = { d.wheelDamage, d.tireDamage, d.bothDamage, d.wrongTire, d.unclassified };
    for (int i = 0; i < 5; ++i)
    {
        const int row = 15 + i;
        CString rowXml;
        rowXml.Format(_T("<row r=\"%d\" ht=\"26\" customHeight=\"1\">"), row);
        rowXml += textCell(cellRef(L'A', row), typeNames[i], i % 2 ? 7 : 5);
        rowXml += numCell(cellRef(L'E', row), typeCounts[i], i % 2 ? 7 : 5);
        rowXml += textCell(cellRef(L'G', row), fmtPercent(typeCounts[i], d.fail), i % 2 ? 7 : 5);
        rowXml += _T("</row>");
        sheet1 += rowXml;
    }
    sheet1 += _T("<row r=\"20\">") + textCell(_T("A20"), _T("※ 동시 손상은 1건으로 집계하며 중복 계산하지 않습니다."), 9) + _T("</row>");
    sheet1 += _T("</sheetData><mergeCells count=\"21\">")
        _T("<mergeCell ref=\"A1:H2\"/><mergeCell ref=\"A3:H3\"/><mergeCell ref=\"A4:H4\"/>")
        _T("<mergeCell ref=\"A6:B6\"/><mergeCell ref=\"C6:D6\"/><mergeCell ref=\"E6:F6\"/><mergeCell ref=\"G6:H6\"/>")
        _T("<mergeCell ref=\"A7:B7\"/><mergeCell ref=\"C7:D7\"/><mergeCell ref=\"E7:F7\"/><mergeCell ref=\"G7:H7\"/>")
        _T("<mergeCell ref=\"A9:H9\"/><mergeCell ref=\"A13:H13\"/><mergeCell ref=\"A14:D14\"/>")
        _T("<mergeCell ref=\"E14:F14\"/><mergeCell ref=\"G14:H14\"/><mergeCell ref=\"A20:H20\"/>")
        _T("<mergeCell ref=\"A10:B10\"/><mergeCell ref=\"A11:B11\"/><mergeCell ref=\"E10:F10\"/><mergeCell ref=\"E11:F11\"/>")
        _T("</mergeCells>") + pageMargins + _T("</worksheet>");
    // 손상 유형명은 긴 한글 문구이므로 넓게 병합해 잘리지 않도록 합니다.
    CString extraMerges;
    for (int row = 15; row <= 19; ++row)
    {
        CString x;
        x.Format(_T("<mergeCell ref=\"A%d:D%d\"/>"), row, row);
        extraMerges += x;
        x.Format(_T("<mergeCell ref=\"E%d:F%d\"/>"), row, row);
        extraMerges += x;
        x.Format(_T("<mergeCell ref=\"G%d:H%d\"/>"), row, row);
        extraMerges += x;
    }
    sheet1.Replace(_T("</mergeCells>"), extraMerges + _T("</mergeCells>"));
    sheet1.Replace(_T("<mergeCells count=\"21\">"), _T("<mergeCells count=\"36\">"));

    // Sheet 2: 시간대별 데이터 (Excel 필터·고정 헤더)
    CString sheet2 = open + wsNs +
        _T("<dimension ref=\"A1:F20\"/><sheetViews><sheetView workbookViewId=\"0\">")
        _T("<pane ySplit=\"5\" topLeftCell=\"A6\" activePane=\"bottomLeft\" state=\"frozen\"/></sheetView></sheetViews>")
        _T("<sheetFormatPr defaultRowHeight=\"23\"/><cols><col min=\"1\" max=\"1\" width=\"23\" customWidth=\"1\"/>")
        _T("<col min=\"2\" max=\"6\" width=\"17\" customWidth=\"1\"/></cols><sheetData>");
    sheet2 += _T("<row r=\"1\" ht=\"40\" customHeight=\"1\">") + textCell(_T("A1"), _T("시간대별 생산 통계"), 1) + _T("</row>");
    sheet2 += _T("<row r=\"3\">") + textCell(_T("A3"), note, 9) + _T("</row>");
    const CString timeCols[] = { _T("시간대"), _T("총 검사"), _T("정상"), _T("NG"), _T("불량률"), _T("판정") };
    sheet2 += _T("<row r=\"5\" ht=\"30\" customHeight=\"1\">");
    for (int c = 0; c < 6; ++c) sheet2 += textCell(cellRef(L'A' + c, 5), timeCols[c], 2);
    sheet2 += _T("</row>");
    for (size_t i = 0; i < d.hourlyLabel.size() && i < d.hourlyTotal.size() && i < d.hourlyPass.size() && i < d.hourlyFail.size(); ++i)
    {
        int row = 6 + static_cast<int>(i);
        CString line; line.Format(_T("<row r=\"%d\" ht=\"26\" customHeight=\"1\">"), row);
        int hour = _ttoi(d.hourlyLabel[i]);
        CString period; period.Format(_T("%02d:00 ~ %02d:00"), hour, hour + 1);
        const int style = (i % 2 == 0) ? 5 : 7;
        line += textCell(cellRef(L'A', row), period, style);
        line += numCell(cellRef(L'B', row), d.hourlyTotal[i], style);
        line += numCell(cellRef(L'C', row), d.hourlyPass[i], style);
        line += numCell(cellRef(L'D', row), d.hourlyFail[i], style);
        line += textCell(cellRef(L'E', row), fmtPercent(d.hourlyFail[i], d.hourlyTotal[i]), style);
        line += textCell(cellRef(L'F', row), d.hourlyTotal[i] == 0 ? _T("-무검사-") :
            (d.hourlyFail[i] * 100.0 / d.hourlyTotal[i] > 7.0 ? _T("주의") : _T("정상")), style);
        sheet2 += line + _T("</row>");
    }
    sheet2 += _T("</sheetData><mergeCells count=\"2\"><mergeCell ref=\"A1:F2\"/><mergeCell ref=\"A3:F3\"/></mergeCells>")
        + pageMargins + _T("</worksheet>");

    // Sheet 3: 세 가지 유형과 미분류를 분리하여 NG 합계가 일치하도록 작성
    CString sheet3 = open + wsNs +
        _T("<dimension ref=\"A1:E17\"/><sheetViews><sheetView workbookViewId=\"0\"/></sheetViews>")
        _T("<sheetFormatPr defaultRowHeight=\"24\"/>")
        _T("<cols><col min=\"1\" max=\"1\" width=\"30\" customWidth=\"1\"/>")
        _T("<col min=\"2\" max=\"2\" width=\"18\" customWidth=\"1\"/>")
        _T("<col min=\"3\" max=\"3\" width=\"20\" customWidth=\"1\"/>")
        _T("<col min=\"4\" max=\"4\" width=\"24\" customWidth=\"1\"/>")
        _T("<col min=\"5\" max=\"5\" width=\"20\" customWidth=\"1\"/></cols><sheetData>");
    sheet3 += _T("<row r=\"1\" ht=\"40\" customHeight=\"1\">") + textCell(_T("A1"), _T("불량 유형별 분석"), 1) + _T("</row>");
    sheet3 += _T("<row r=\"3\">") + textCell(_T("A3"), note, 9) + _T("</row>");
    const CString defectCols[] = { _T("불량 유형"), _T("검출 건수"), _T("NG 내 비중"), _T("검사 총량 대비"), _T("분류 상태") };
    sheet3 += _T("<row r=\"5\" ht=\"30\" customHeight=\"1\">");
    for (int c = 0; c < 5; ++c) sheet3 += textCell(cellRef(L'A' + c, 5), defectCols[c], 2);
    sheet3 += _T("</row>");
    for (int i = 0; i < 4; ++i)
    {
        int row = 6 + i;
        CString line; line.Format(_T("<row r=\"%d\" ht=\"30\" customHeight=\"1\">"), row);
        const int style = i % 2 == 0 ? 5 : 7;
        line += textCell(cellRef(L'A', row), typeNames[i], style);
        line += numCell(cellRef(L'B', row), typeCounts[i], style);
        line += textCell(cellRef(L'C', row), fmtPercent(typeCounts[i], d.fail), style);
        line += textCell(cellRef(L'D', row), fmtPercent(typeCounts[i], d.total), style);
        line += textCell(cellRef(L'E', row), i == 3 ? _T("상세 분류 필요") : _T("분류 완료"), style);
        sheet3 += line + _T("</row>");
    }
    sheet3 += _T("<row r=\"10\" ht=\"28\" customHeight=\"1\">") + textCell(_T("A10"), _T("불량 합계"), 3)
        + numCell(_T("B10"), d.fail, 8) + textCell(_T("C10"), fmtPercent(d.fail, d.fail), 8)
        + textCell(_T("D10"), fmtPercent(d.fail, d.total), 8) + _T("</row>");
    // 목표는 프로젝트 시연용 판정 기준이며 DB에 저장된 운영 목표가 아닙니다.
    sheet3 += _T("<row r=\"12\" ht=\"28\" customHeight=\"1\">")
        + textCell(_T("A12"), _T("일일 전체 불량률"), 3)
        + textCell(_T("B12"), fmtPercent(d.fail, d.total), 8) + _T("</row>");
    sheet3 += _T("<row r=\"13\" ht=\"28\" customHeight=\"1\">")
        + textCell(_T("A13"), _T("시연 판정 기준"), 3)
        + textCell(_T("B13"), _T("7.0% 이하"), 8)
        + textCell(_T("C13"), d.total == 0 ? _T("검사 없음") :
            (d.failRate > 7.0 ? _T("주의 · 기준 초과") : _T("정상 · 기준 충족")), 8)
        + _T("</row>");
    int worstHourIndex = -1;
    double worstHourRate = -1.0;
    for (size_t i = 0; i < d.hourlyLabel.size() &&
        i < d.hourlyTotal.size() && i < d.hourlyFail.size(); ++i)
    {
        if (d.hourlyTotal[i] <= 0) continue;
        const double rate = d.hourlyFail[i] * 100.0 / d.hourlyTotal[i];
        if (rate > worstHourRate) { worstHourRate = rate; worstHourIndex = static_cast<int>(i); }
    }
    CString hourLabel = _T("검사 없음");
    CString hourValue = _T("-");
    CString hourCounts = _T("-");
    if (worstHourIndex >= 0)
    {
        hourLabel = d.hourlyLabel[worstHourIndex] + _T("시");
        hourValue.Format(_T("%.1f%%"), worstHourRate);
        hourCounts.Format(_T("NG %d / 총 %d건"),
            d.hourlyFail[worstHourIndex], d.hourlyTotal[worstHourIndex]);
    }
    sheet3 += _T("<row r=\"15\" ht=\"28\" customHeight=\"1\">")
        + textCell(_T("A15"), _T("최고 불량률 시간대"), 3)
        + textCell(_T("B15"), hourLabel, 8)
        + textCell(_T("C15"), hourValue, 8)
        + textCell(_T("D15"), hourCounts, 8) + _T("</row>");
    sheet3 += _T("<row r=\"17\" ht=\"28\" customHeight=\"1\">")
        + textCell(_T("A17"), _T("※ 분모 구분: NG 내 비중=유형 건수/전체 NG, 전체 대비=유형 건수/총 검사"), 9)
        + _T("</row>");
    sheet3 += _T("</sheetData><mergeCells count=\"3\"><mergeCell ref=\"A1:E2\"/><mergeCell ref=\"A3:E3\"/>")
        _T("<mergeCell ref=\"A17:E17\"/></mergeCells>")
        + pageMargins + _T("</worksheet>");

    // Sheet 4: NG 상세 이력 (없으면 DB 행을 임의 생성하지 않습니다.)
    CString sheet4 = open + wsNs +
        _T("<dimension ref=\"A1:F520\"/><sheetViews><sheetView workbookViewId=\"0\">")
        _T("<pane ySplit=\"5\" topLeftCell=\"A6\" activePane=\"bottomLeft\" state=\"frozen\"/></sheetView></sheetViews>")
        _T("<sheetFormatPr defaultRowHeight=\"25\"/>")
        _T("<cols><col min=\"1\" max=\"1\" width=\"16\" customWidth=\"1\"/>")
        _T("<col min=\"2\" max=\"2\" width=\"24\" customWidth=\"1\"/>")
        _T("<col min=\"3\" max=\"3\" width=\"22\" customWidth=\"1\"/>")
        _T("<col min=\"4\" max=\"4\" width=\"28\" customWidth=\"1\"/>")
        _T("<col min=\"5\" max=\"5\" width=\"48\" customWidth=\"1\"/>")
        _T("<col min=\"6\" max=\"6\" width=\"60\" customWidth=\"1\"/></cols><sheetData>");
    sheet4 += _T("<row r=\"1\" ht=\"40\" customHeight=\"1\">") + textCell(_T("A1"), _T("불량 상세 검사 이력"), 1) + _T("</row>");
    sheet4 += _T("<row r=\"3\">") + textCell(_T("A3"), note, 9) + _T("</row>");
    const CString detailCols[] = { _T("검사 ID"), _T("검사 일시"), _T("타이어 ID"), _T("불량 유형"), _T("상세 내용"), _T("NG 이미지 경로") };
    sheet4 += _T("<row r=\"5\" ht=\"30\" customHeight=\"1\">");
    for (int c = 0; c < 6; ++c) sheet4 += textCell(cellRef(L'A' + c, 5), detailCols[c], 2);
    sheet4 += _T("</row>");
    for (size_t i = 0; i < d.defectRecords.size(); ++i)
    {
        int row = 6 + static_cast<int>(i);
        const auto& defect = d.defectRecords[i];
        const CString values[] = { defect.inspectionId, defect.inspectedAt, defect.tireId,
            defect.defectType, defect.detail.IsEmpty() ? _T("상세 미입력") : defect.detail, defect.imagePath };
        CString line; line.Format(_T("<row r=\"%d\" ht=\"40\" customHeight=\"1\">"), row);
        for (int c = 0; c < 6; ++c)
            line += textCell(cellRef(L'A' + c, row), values[c], c >= 4 ? 6 : (i % 2 == 0 ? 5 : 7));
        sheet4 += line + _T("</row>");
    }
    if (d.defectRecords.empty()) sheet4 += _T("<row r=\"6\">") + textCell(_T("A6"), _T("조회된 NG 상세 이력이 없습니다."), 9) + _T("</row>");
    sheet4 += _T("</sheetData><mergeCells count=\"2\"><mergeCell ref=\"A1:F2\"/><mergeCell ref=\"A3:F3\"/></mergeCells>")
        + pageMargins + _T("</worksheet>");

    // Sheet 5: 인수인계
    CString sheet5 = open + wsNs +
        _T("<dimension ref=\"A1:H520\"/><sheetViews><sheetView workbookViewId=\"0\">")
        _T("<pane ySplit=\"5\" topLeftCell=\"A6\" activePane=\"bottomLeft\" state=\"frozen\"/></sheetView></sheetViews>")
        _T("<sheetFormatPr defaultRowHeight=\"25\"/>")
        _T("<cols><col min=\"1\" max=\"1\" width=\"17\" customWidth=\"1\"/>")
        _T("<col min=\"2\" max=\"3\" width=\"16\" customWidth=\"1\"/>")
        _T("<col min=\"4\" max=\"4\" width=\"20\" customWidth=\"1\"/>")
        _T("<col min=\"5\" max=\"5\" width=\"70\" customWidth=\"1\"/>")
        _T("<col min=\"6\" max=\"8\" width=\"19\" customWidth=\"1\"/></cols><sheetData>");
    sheet5 += _T("<row r=\"1\" ht=\"40\" customHeight=\"1\">") + textCell(_T("A1"), _T("인수인계 기록"), 1) + _T("</row>");
    sheet5 += _T("<row r=\"3\">") + textCell(_T("A3"), _T("당일 기록 / 상세 내용을 확인하세요."), 9) + _T("</row>");
    const CString handoverCols[] = { _T("근무일"), _T("근무"), _T("중요도"), _T("작성자"), _T("인수인계 내용"), _T("확인상태"), _T("확인자"), _T("확인시각") };
    sheet5 += _T("<row r=\"5\" ht=\"30\" customHeight=\"1\">");
    for (int c = 0; c < 8; ++c) sheet5 += textCell(cellRef(L'A' + c, 5), handoverCols[c], 2);
    sheet5 += _T("</row>");
    for (size_t i = 0; i < d.handovers.size(); ++i)
    {
        const auto& h = d.handovers[i];
        const int row = 6 + static_cast<int>(i);
        const CString values[] = { h.workDate, HandoverShiftName(h.shiftType), HandoverPriorityName(h.priority),
            h.userName, h.message, h.checked ? _T("확인완료") : _T("미확인"), h.checkedBy, h.checkedAt };
        CString line; line.Format(_T("<row r=\"%d\" ht=\"45\" customHeight=\"1\">"), row);
        for (int c = 0; c < 8; ++c)
            line += textCell(cellRef(L'A' + c, row), values[c], c == 4 ? 6 : (i % 2 == 0 ? 5 : 7));
        sheet5 += line + _T("</row>");
    }
    sheet5 += _T("</sheetData><mergeCells count=\"2\"><mergeCell ref=\"A1:H2\"/><mergeCell ref=\"A3:H3\"/></mergeCells>")
        + pageMargins + _T("</worksheet>");

    CString sheet6 = open + wsNs +
        _T("<dimension ref=\"A1:D13\"/><sheetViews><sheetView workbookViewId=\"0\"/></sheetViews>")
        _T("<sheetFormatPr defaultRowHeight=\"25\"/><cols><col min=\"1\" max=\"1\" width=\"26\" customWidth=\"1\"/>")
        _T("<col min=\"2\" max=\"4\" width=\"23\" customWidth=\"1\"/></cols><sheetData>");
    sheet6 += _T("<row r=\"1\">") + textCell(_T("A1"), _T("설비 이벤트 분석"), 1) + _T("</row>");
    sheet6 += _T("<row r=\"3\">") + textCell(_T("A3"), _T("생산일자"), 2) + textCell(_T("B3"), d.date) + _T("</row>");
    sheet6 += _T("<row r=\"4\">") + textCell(_T("A4"), _T("이벤트"), 2)
        + textCell(_T("B4"), _T("발생 횟수"), 2) + textCell(_T("C4"), _T("완료 세션 초"), 2)
        + textCell(_T("D4"), _T("진행 중 세션"), 2) + _T("</row>");
    const CString eventNames[] = { _T("비상정지 ESTOP"), _T("재확인 RECHECK"), _T("YOLO 경고"), _T("경고 해제 CLEAR") };
    const long long eventCounts[] = { d.estopCount, d.recheckCount, d.yoloCount, d.clearCount };
    const long long eventSeconds[] = { d.estopSeconds, d.recheckSeconds, d.yoloSeconds, 0 };
    const long long eventActive[] = { d.activeEstop, d.activeRecheck, d.activeYolo, 0 };
    for (int i = 0; i < 4; ++i)
    {
        int row = 5 + i; CString tag; tag.Format(_T("<row r=\"%d\">"), row);
        sheet6 += tag + textCell(cellRef(L'A', row), eventNames[i], 5);
        if (d.equipmentStatsAvailable)
            sheet6 += numCell(cellRef(L'B', row), eventCounts[i], 5)
                + numCell(cellRef(L'C', row), eventSeconds[i], 5)
                + numCell(cellRef(L'D', row), eventActive[i], 5);
        else sheet6 += textCell(cellRef(L'B', row), _T("미수신"), 5);
        sheet6 += _T("</row>");
    }
    sheet6 += _T("<row r=\"10\">") + textCell(_T("A10"),
        _T("발생 횟수=이벤트 세션 기준 / 지속 시간=종료된 세션 기준 / 검사 NG와 별도 집계"), 9) + _T("</row>");
    sheet6 += _T("</sheetData><mergeCells count=\"2\"><mergeCell ref=\"A1:D2\"/><mergeCell ref=\"A10:D10\"/></mergeCells>")
        + pageMargins + _T("</worksheet>");

    CString strings = open;
    CString start;
    start.Format(_T("<sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" count=\"%llu\" uniqueCount=\"%llu\">"),
        static_cast<unsigned long long>(sharedCount), static_cast<unsigned long long>(sharedStrings.size()));
    strings += start;
    for (const CString& item : sharedStrings)
        strings += _T("<si><t xml:space=\"preserve\">") + XmlEscape(item) + _T("</t></si>");
    strings += _T("</sst>");

    CString contentTypes = open + _T("<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">")
        _T("<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>")
        _T("<Default Extension=\"xml\" ContentType=\"application/xml\"/>")
        _T("<Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/>")
        _T("<Override PartName=\"/docProps/app.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.extended-properties+xml\"/>")
        _T("<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>")
        _T("<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>")
        _T("<Override PartName=\"/xl/sharedStrings.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml\"/>")
        _T("<Override PartName=\"/xl/worksheets/sheet1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>")
        _T("<Override PartName=\"/xl/worksheets/sheet2.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>")
        _T("<Override PartName=\"/xl/worksheets/sheet3.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>")
        _T("<Override PartName=\"/xl/worksheets/sheet4.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>")
        _T("<Override PartName=\"/xl/worksheets/sheet5.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>")
        _T("<Override PartName=\"/xl/worksheets/sheet6.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>")
        _T("</Types>");
    const CString rootRels = open +
        _T("<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">")
        _T("<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>")
        _T("<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" Target=\"docProps/core.xml\"/>")
        _T("<Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties\" Target=\"docProps/app.xml\"/>")
        _T("</Relationships>");
    const CString workbook = open +
        _T("<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">")
        _T("<bookViews><workbookView activeTab=\"0\"/></bookViews><sheets>")
        _T("<sheet name=\"생산요약\" sheetId=\"1\" r:id=\"rId1\"/>")
        _T("<sheet name=\"시간대별 통계\" sheetId=\"2\" r:id=\"rId2\"/>")
        _T("<sheet name=\"불량 유형 분석\" sheetId=\"3\" r:id=\"rId3\"/>")
        _T("<sheet name=\"불량 상세 이력\" sheetId=\"4\" r:id=\"rId4\"/>")
        _T("<sheet name=\"인수인계\" sheetId=\"5\" r:id=\"rId5\"/>")
        _T("<sheet name=\"설비 이벤트 분석\" sheetId=\"6\" r:id=\"rId6\"/>")
        _T("</sheets></workbook>");
    const CString workbookRels = open +
        _T("<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">")
        _T("<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/>")
        _T("<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet2.xml\"/>")
        _T("<Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet3.xml\"/>")
        _T("<Relationship Id=\"rId4\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet4.xml\"/>")
        _T("<Relationship Id=\"rId5\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet5.xml\"/>")
        _T("<Relationship Id=\"rId6\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet6.xml\"/>")
        _T("<Relationship Id=\"rId7\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>")
        _T("<Relationship Id=\"rId8\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings\" Target=\"sharedStrings.xml\"/>")
        _T("</Relationships>");
    const CString styles = open +
        _T("<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">")
        _T("<fonts count=\"5\">")
        _T("<font><sz val=\"10\"/><name val=\"Malgun Gothic\"/></font>")
        _T("<font><b/><sz val=\"17\"/><color rgb=\"FFFFFFFF\"/><name val=\"Malgun Gothic\"/></font>")
        _T("<font><b/><sz val=\"10\"/><color rgb=\"FFFFFFFF\"/><name val=\"Malgun Gothic\"/></font>")
        _T("<font><b/><sz val=\"12\"/><color rgb=\"FF1C436C\"/><name val=\"Malgun Gothic\"/></font>")
        _T("<font><sz val=\"9\"/><color rgb=\"FF5F7186\"/><name val=\"Malgun Gothic\"/></font>")
        _T("</fonts>")
        _T("<fills count=\"5\">")
        _T("<fill><patternFill patternType=\"none\"/></fill>")
        _T("<fill><patternFill patternType=\"gray125\"/></fill>")
        _T("<fill><patternFill patternType=\"solid\"><fgColor rgb=\"FF1C436C\"/><bgColor indexed=\"64\"/></patternFill></fill>")
        _T("<fill><patternFill patternType=\"solid\"><fgColor rgb=\"FFEAF4FD\"/><bgColor indexed=\"64\"/></patternFill></fill>")
        _T("<fill><patternFill patternType=\"solid\"><fgColor rgb=\"FFF7F9FC\"/><bgColor indexed=\"64\"/></patternFill></fill>")
        _T("</fills>")
        _T("<borders count=\"2\"><border><left/><right/><top/><bottom/><diagonal/></border>")
        _T("<border><left style=\"thin\"><color rgb=\"FFD9E2EC\"/></left>")
        _T("<right style=\"thin\"><color rgb=\"FFD9E2EC\"/></right>")
        _T("<top style=\"thin\"><color rgb=\"FFD9E2EC\"/></top>")
        _T("<bottom style=\"thin\"><color rgb=\"FFD9E2EC\"/></bottom><diagonal/></border></borders>")
        _T("<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>")
        _T("<cellXfs count=\"10\">")
        _T("<xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/>")
        _T("<xf numFmtId=\"0\" fontId=\"1\" fillId=\"2\" borderId=\"0\" xfId=\"0\" applyAlignment=\"1\"><alignment vertical=\"center\"/></xf>")
        _T("<xf numFmtId=\"0\" fontId=\"2\" fillId=\"2\" borderId=\"1\" xfId=\"0\" applyAlignment=\"1\"><alignment horizontal=\"center\" vertical=\"center\"/></xf>")
        _T("<xf numFmtId=\"0\" fontId=\"3\" fillId=\"3\" borderId=\"0\" xfId=\"0\" applyAlignment=\"1\"><alignment vertical=\"center\"/></xf>")
        _T("<xf numFmtId=\"0\" fontId=\"4\" fillId=\"4\" borderId=\"1\" xfId=\"0\" applyAlignment=\"1\"><alignment vertical=\"center\"/></xf>")
        _T("<xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"1\" xfId=\"0\" applyAlignment=\"1\"><alignment vertical=\"center\"/></xf>")
        _T("<xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"1\" xfId=\"0\" applyAlignment=\"1\"><alignment vertical=\"top\" wrapText=\"1\"/></xf>")
        _T("<xf numFmtId=\"0\" fontId=\"0\" fillId=\"4\" borderId=\"1\" xfId=\"0\" applyAlignment=\"1\"><alignment vertical=\"center\"/></xf>")
        _T("<xf numFmtId=\"0\" fontId=\"3\" fillId=\"3\" borderId=\"1\" xfId=\"0\" applyAlignment=\"1\"><alignment horizontal=\"center\" vertical=\"center\"/></xf>")
        _T("<xf numFmtId=\"0\" fontId=\"4\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyAlignment=\"1\"><alignment vertical=\"center\"/></xf>")
        _T("</cellXfs><cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles></styleSheet>");
    const CString coreProperties = open +
        _T("<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\"")
        _T(" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:dcterms=\"http://purl.org/dc/terms/\"")
        _T(" xmlns:dcmitype=\"http://purl.org/dc/dcmitype/\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">")
        _T("<dc:creator>Tire Production Monitoring</dc:creator></cp:coreProperties>");
    const CString appProperties = open +
        _T("<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/extended-properties\"")
        _T(" xmlns:vt=\"http://schemas.openxmlformats.org/officeDocument/2006/docPropsVTypes\">")
        _T("<Application>Tire Production Monitoring</Application></Properties>");

    const std::vector<XlsxPart> parts = {
        { "[Content_Types].xml", ToUtf8(contentTypes) },
        { "_rels/.rels", ToUtf8(rootRels) },
        { "docProps/core.xml", ToUtf8(coreProperties) },
        { "docProps/app.xml", ToUtf8(appProperties) },
        { "xl/workbook.xml", ToUtf8(workbook) },
        { "xl/_rels/workbook.xml.rels", ToUtf8(workbookRels) },
        { "xl/styles.xml", ToUtf8(styles) },
        { "xl/sharedStrings.xml", ToUtf8(strings) },
        { "xl/worksheets/sheet1.xml", ToUtf8(sheet1) },
        { "xl/worksheets/sheet2.xml", ToUtf8(sheet2) },
        { "xl/worksheets/sheet3.xml", ToUtf8(sheet3) },
        { "xl/worksheets/sheet4.xml", ToUtf8(sheet4) },
        { "xl/worksheets/sheet5.xml", ToUtf8(sheet5) },
        { "xl/worksheets/sheet6.xml", ToUtf8(sheet6) }
    };
    return SaveXlsxParts(path, parts);
}

void CDailyReportPage::OnExcel()
{
    RefreshReport();

    CString defaultName;
    defaultName.Format(_T("Tire_Report_%s.xlsx"), GetReportDate().GetString());

    CFileDialog dialog(
        FALSE, _T("xlsx"), defaultName,
        OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT,
        _T("Excel 통합 문서 (*.xlsx)|*.xlsx||"), this);

    if (dialog.DoModal() != IDOK)
        return;

    if (ExportExcelXlsx(dialog.GetPathName()))
        AfxMessageBox(_T("Excel 파일을 저장했습니다.\r\n저장한 파일을 한셀 또는 Excel에서 열어 확인해주세요."));
    else
        AfxMessageBox(_T("Excel 저장에 실패했습니다."));
}