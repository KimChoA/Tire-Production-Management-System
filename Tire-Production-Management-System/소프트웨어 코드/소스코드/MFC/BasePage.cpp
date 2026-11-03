#include "pch.h"
#include "BasePage.h"

BEGIN_MESSAGE_MAP(CBasePage, CWnd)
    ON_WM_CREATE()
    ON_WM_ERASEBKGND()
    ON_WM_CTLCOLOR()
END_MESSAGE_MAP()

int CBasePage::OnCreate(LPCREATESTRUCT lpCreateStruct)
{
    if (CWnd::OnCreate(lpCreateStruct) == -1) return -1;
    m_titleFont.CreatePointFont(180, _T("Segoe UI Semibold"));
    m_normalFont.CreatePointFont(100, _T("Segoe UI"));
    m_labelFont.CreatePointFont(100, _T("Segoe UI Semibold"));
    m_pageBrush.CreateSolidBrush(UiTheme::PageBg());
    m_whiteBrush.CreateSolidBrush(UiTheme::CardBg());
    return 0;
}

BOOL CBasePage::CreatePage(CWnd* parent, const CRect& rect, UINT id)
{
    CString cls = AfxRegisterWndClass(CS_HREDRAW | CS_VREDRAW, ::LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr);
    return CreateEx(0, cls, _T("Page"), WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, rect, parent, id);
}

void CBasePage::AddTitle(const CString& title)
{
    m_title.Create(title, WS_CHILD | WS_VISIBLE, CRect(28, 20, 760, 58), this);
    m_title.SetFont(&m_titleFont);
}

CStatic* CBasePage::AddLabel(const CString& text, int x, int y, int w, int h)
{
    auto* c = new CStatic();
    c->Create(text, WS_CHILD | WS_VISIBLE, CRect(x, y, x + w, y + h), this);
    c->SetFont(&m_labelFont);
    return c;
}

CModernButton* CBasePage::AddButton(const CString& text, int x, int y, int w, int h, UINT id)
{
    auto* c = new CModernButton();
    c->Create(text, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP, CRect(x, y, x + w, y + h), this, id);
    c->SetNavigationMode(false);
    c->SetFont(&m_labelFont);
    return c;
}

CEdit* CBasePage::AddEdit(int x, int y, int w, int h, DWORD style)
{
    auto* c = new CEdit();
    c->Create(WS_CHILD | WS_VISIBLE | WS_BORDER | style, CRect(x, y, x + w, y + h), this, 0);
    c->SetFont(&m_normalFont);
    return c;
}

CListCtrl* CBasePage::AddList(int x, int y, int w, int h)
{
    auto* c = new CListCtrl();
    c->Create(WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL, CRect(x, y, x + w, y + h), this, 0);
    c->SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    c->SetFont(&m_normalFont);
    c->SetBkColor(UiTheme::CardBg());
    c->SetTextBkColor(UiTheme::CardBg());
    c->SetTextColor(UiTheme::Text());
    return c;
}

CModernCard* CBasePage::AddBox(const CString& text, int x, int y, int w, int h)
{
    auto* c = new CModernCard();
    c->CreateCard(text, this, CRect(x, y, x + w, y + h));
    return c;
}

BOOL CBasePage::OnEraseBkgnd(CDC* pDC)
{
    CRect rc;
    GetClientRect(&rc);
    pDC->FillSolidRect(rc, UiTheme::PageBg());
    return TRUE;
}

HBRUSH CBasePage::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor)
{
    if (nCtlColor == CTLCOLOR_STATIC)
    {
        pDC->SetBkMode(TRANSPARENT);
        pDC->SetTextColor(pWnd->GetSafeHwnd() == m_title.GetSafeHwnd() ? UiTheme::Text() : UiTheme::Muted());
        return (HBRUSH)m_pageBrush.GetSafeHandle();
    }
    if (nCtlColor == CTLCOLOR_EDIT)
    {
        pDC->SetBkColor(UiTheme::CardBg());
        pDC->SetTextColor(UiTheme::Text());
        return (HBRUSH)m_whiteBrush.GetSafeHandle();
    }
    return CWnd::OnCtlColor(pDC, pWnd, nCtlColor);
}
