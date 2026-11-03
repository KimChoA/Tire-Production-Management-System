#pragma once
#include "framework.h"
#include "Theme/ThemeControls.h"

class CBasePage : public CWnd
{
public:
    BOOL CreatePage(CWnd* parent, const CRect& rect, UINT id);
    void AddTitle(const CString& title);

protected:
    CStatic m_title;
    CFont m_titleFont;
    CFont m_normalFont;
    CFont m_labelFont;
    CBrush m_pageBrush;
    CBrush m_whiteBrush;

    CStatic* AddLabel(const CString& text, int x, int y, int w = 180, int h = 28);
    CModernButton* AddButton(const CString& text, int x, int y, int w = 110, int h = 36, UINT id = 0);
    CEdit* AddEdit(int x, int y, int w, int h, DWORD style = ES_AUTOHSCROLL);
    CListCtrl* AddList(int x, int y, int w, int h);
    CModernCard* AddBox(const CString& text, int x, int y, int w, int h);

    afx_msg int OnCreate(LPCREATESTRUCT lpCreateStruct);
    afx_msg BOOL OnEraseBkgnd(CDC* pDC);
    afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);
    DECLARE_MESSAGE_MAP()
};
