#include "pch.h"
#include "ThemeControls.h"

void CModernButton::DrawItem(LPDRAWITEMSTRUCT dis)
{
    CDC dc;
    dc.Attach(dis->hDC);

    CRect rc(dis->rcItem);
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;

    COLORREF bg = UiTheme::Accent();
    COLORREF fg = RGB(255, 255, 255);
    COLORREF border = UiTheme::Accent();

    if (m_navigation)
    {
        if (m_active)
        {
            bg = UiTheme::AccentSoft();
            fg = UiTheme::AccentDark();
            border = UiTheme::AccentSoft();
        }
        else
        {
            bg = UiTheme::Sidebar();
            fg = UiTheme::Text();
            border = UiTheme::Sidebar();
        }
    }
    else if (pressed)
    {
        bg = UiTheme::AccentDark();
        border = UiTheme::AccentDark();
    }

    if (disabled)
    {
        bg = RGB(226, 232, 240);
        fg = RGB(148, 163, 184);
        border = bg;
    }

    dc.SetBkMode(TRANSPARENT);
    CBrush brush(bg);
    CPen pen(PS_SOLID, 1, border);
    auto* oldBrush = dc.SelectObject(&brush);
    auto* oldPen = dc.SelectObject(&pen);
    dc.RoundRect(rc.left, rc.top, rc.right, rc.bottom, 12, 12);

    if (m_navigation && m_active)
    {
        CBrush accent(UiTheme::Accent());
        CRect bar(rc.left + 1, rc.top + 8, rc.left + 5, rc.bottom - 8);
        dc.FillRect(bar, &accent);
    }

    CString text;
    GetWindowText(text);
    dc.SetTextColor(fg);
    CFont* oldFont = nullptr;
    if (GetFont()) oldFont = dc.SelectObject(GetFont());

    CRect textRc = rc;
    if (m_navigation)
    {
        textRc.left += 20;
        dc.DrawText(text, &textRc, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
    }
    else
    {
        dc.DrawText(text, &textRc, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS);
    }

    if (oldFont) dc.SelectObject(oldFont);
    dc.SelectObject(oldBrush);
    dc.SelectObject(oldPen);
    dc.Detach();
}

BEGIN_MESSAGE_MAP(CModernCard, CWnd)
    ON_WM_PAINT()
    ON_WM_ERASEBKGND()
END_MESSAGE_MAP()

BOOL CModernCard::CreateCard(const CString& text, CWnd* parent, const CRect& rect, UINT id)
{
    m_text = text;
    CString cls = AfxRegisterWndClass(CS_HREDRAW | CS_VREDRAW, ::LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr);
    if (!CreateEx(0, cls, _T("ModernCard"), WS_CHILD | WS_VISIBLE, rect, parent, id))
        return FALSE;

    m_titleFont.CreatePointFont(95, _T("Segoe UI"));
    m_valueFont.CreatePointFont(140, _T("Segoe UI Semibold"));
    m_bodyFont.CreatePointFont(100, _T("Segoe UI"));
    return TRUE;
}

BOOL CModernCard::OnEraseBkgnd(CDC* pDC)
{
    return TRUE;
}

void CModernCard::OnPaint()
{
    CPaintDC dc(this);
    CRect rc;
    GetClientRect(&rc);
    dc.SetBkMode(TRANSPARENT);

    CBrush bg(UiTheme::CardBg());
    CPen border(PS_SOLID, 1, UiTheme::Border());
    auto* oldBrush = dc.SelectObject(&bg);
    auto* oldPen = dc.SelectObject(&border);
    dc.RoundRect(rc.left, rc.top, rc.right - 1, rc.bottom - 1, 14, 14);

    CBrush accent(UiTheme::Accent());
    CRect accentRc(18, 14, 54, 18);
    dc.FillRect(accentRc, &accent);

    CString text = m_text;
    text.Replace(_T("\r\n"), _T("\n"));

    CStringArray lines;
    int pos = 0;
    CString token = text.Tokenize(_T("\n"), pos);
    while (!token.IsEmpty() || pos != -1)
    {
        lines.Add(token);
        if (pos == -1) break;
        token = text.Tokenize(_T("\n"), pos);
    }

    if (lines.GetCount() <= 1)
    {
        dc.SetTextColor(UiTheme::Muted());
        auto* oldFont = dc.SelectObject(&m_bodyFont);
        CRect tr = rc;
        tr.DeflateRect(22, 24);
        dc.DrawText(lines.GetCount() == 1 ? lines[0] : text, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        dc.SelectObject(oldFont);
    }
    else if (lines.GetCount() == 2)
    {
        dc.SetTextColor(UiTheme::Muted());
        auto* oldFont = dc.SelectObject(&m_titleFont);
        CRect titleRc(22, 24, rc.right - 22, 47);
        dc.DrawText(lines[0], &titleRc, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

        dc.SelectObject(&m_valueFont);
        dc.SetTextColor(UiTheme::Text());
        CRect valueRc(22, 51, rc.right - 22, rc.bottom - 10);
        dc.DrawText(lines[1], &valueRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        dc.SelectObject(oldFont);
    }
    else
    {
        dc.SetTextColor(UiTheme::Text());
        auto* oldFont = dc.SelectObject(&m_titleFont);
        CRect titleRc(22, 26, rc.right - 22, 52);
        dc.DrawText(lines[0], &titleRc, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

        dc.SelectObject(&m_bodyFont);
        dc.SetTextColor(UiTheme::Muted());
        int y = 60;
        for (INT_PTR i = 1; i < lines.GetCount(); ++i)
        {
            CRect lineRc(22, y, rc.right - 22, y + 24);
            dc.DrawText(lines[i], &lineRc, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            y += 25;
            if (y > rc.bottom - 18) break;
        }
        dc.SelectObject(oldFont);
    }

    dc.SelectObject(oldBrush);
    dc.SelectObject(oldPen);
}
