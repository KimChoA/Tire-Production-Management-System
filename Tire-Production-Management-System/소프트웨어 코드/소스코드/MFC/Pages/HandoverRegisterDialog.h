#pragma once
#include "HandoverTypes.h"
#include <afxwin.h>
#include <vector>

// 버튼만 CButton 자체의 owner-draw를 사용합니다. CDialog의 DrawItem은 사용하지 않습니다.
class CHandoverActionButton : public CButton
{
public:
    void SetPalette(COLORREF background, COLORREF foreground, COLORREF border)
    {
        m_background = background;
        m_foreground = foreground;
        m_border = border;
    }
    virtual void DrawItem(LPDRAWITEMSTRUCT info) override;
private:
    COLORREF m_background = RGB(49, 111, 237);
    COLORREF m_foreground = RGB(255, 255, 255);
    COLORREF m_border = RGB(49, 111, 237);
};

class CHandoverRegisterDialog : public CDialog
{
public:
    explicit CHandoverRegisterDialog(const CString& defaultUser, CWnd* parent);
    const HandoverFormData& GetData() const { return m_data; }
protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
    afx_msg void OnPaint();
    afx_msg HBRUSH OnCtlColor(CDC* dc, CWnd* wnd, UINT colorType);
    DECLARE_MESSAGE_MAP()
private:
    void BuildTemplate();
    void CreateLabel(CStatic& label, const CString& text, const CRect& rect, CFont* font = nullptr);
    void CreateNumberEdit(CEdit& edit, const CRect& rect, UINT id);
    void CreateMultilineEdit(CEdit& edit, const CRect& rect, UINT id);
    bool ReadCount(CEdit& edit, LPCTSTR fieldName, long long& value);
    void DrawCard(CDC& dc, const CRect& rect);
    static void AppendWord(std::vector<BYTE>& target, WORD value);
    static void AppendWideString(std::vector<BYTE>& target, const wchar_t* value);

    std::vector<BYTE> m_template;
    CString m_defaultUser;
    HandoverFormData m_data;
    CFont m_normalFont, m_boldFont, m_titleFont, m_smallFont, m_sectionFont;
    CBrush m_whiteBrush, m_headerBrush;
    CStatic m_headerTitle, m_headerSubtitle;
    CStatic m_userLabel, m_shiftLabel, m_priorityLabel, m_productionLabel;
    CStatic m_totalLabel, m_passLabel, m_failLabel, m_countUnitLabel;
    CStatic m_equipmentLabel, m_equipmentHint, m_actionLabel, m_pendingLabel;
    CEdit m_userEdit, m_totalEdit, m_passEdit, m_failEdit, m_actionEdit, m_pendingEdit;
    CComboBox m_shiftCombo, m_priorityCombo;
    CButton m_equipmentChecks[4];
    CHandoverActionButton m_okButton, m_cancelButton;
};
