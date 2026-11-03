#pragma once
#include "../framework.h"

namespace UiTheme
{
    inline COLORREF Header()      { return RGB(31, 41, 55); }
    inline COLORREF Sidebar()     { return RGB(248, 250, 252); }
    inline COLORREF PageBg()      { return RGB(243, 246, 250); }
    inline COLORREF CardBg()      { return RGB(255, 255, 255); }
    inline COLORREF Border()      { return RGB(226, 232, 240); }
    inline COLORREF Text()        { return RGB(38, 51, 69); }
    inline COLORREF Muted()       { return RGB(100, 116, 139); }
    inline COLORREF Accent()      { return RGB(59, 130, 246); }
    inline COLORREF AccentDark()  { return RGB(37, 99, 235); }
    inline COLORREF AccentSoft()  { return RGB(239, 246, 255); }
    inline COLORREF Success()     { return RGB(34, 197, 94); }
    inline COLORREF Danger()      { return RGB(239, 68, 68); }
}

class CModernButton : public CButton
{
public:
    void SetNavigationMode(bool value) { m_navigation = value; Invalidate(); }
    void SetActive(bool value) { m_active = value; Invalidate(); }
    bool IsActive() const { return m_active; }

protected:
    bool m_navigation = false;
    bool m_active = false;
    virtual void DrawItem(LPDRAWITEMSTRUCT lpDrawItemStruct) override;
};

class CModernCard : public CWnd
{
public:
    BOOL CreateCard(const CString& text, CWnd* parent, const CRect& rect, UINT id = 0);
    void SetText(const CString& text) { m_text = text; Invalidate(); }

protected:
    CString m_text;
    CFont m_titleFont;
    CFont m_valueFont;
    CFont m_bodyFont;

    afx_msg void OnPaint();
    afx_msg BOOL OnEraseBkgnd(CDC* pDC);
    DECLARE_MESSAGE_MAP()
};
