#include "pch.h"
#include "HandoverRegisterDialog.h"

BEGIN_MESSAGE_MAP(CHandoverRegisterDialog, CDialog)
    ON_WM_PAINT()
    ON_WM_CTLCOLOR()
END_MESSAGE_MAP()

void CHandoverActionButton::DrawItem(LPDRAWITEMSTRUCT info)
{
    if (!info || !info->hDC) return;
    CDC dc;
    dc.Attach(info->hDC);
    CRect rc(info->rcItem);
    const bool disabled = (info->itemState & ODS_DISABLED) != 0;
    const bool pressed = (info->itemState & ODS_SELECTED) != 0;
    COLORREF fill = disabled ? RGB(213, 220, 230) : m_background;
    if (pressed && !disabled) fill = RGB(32, 84, 181);
    dc.FillSolidRect(&rc, fill);
    dc.Draw3dRect(&rc, m_border, m_border);
    CString caption;
    GetWindowText(caption);
    dc.SetBkMode(TRANSPARENT);
    dc.SetTextColor(disabled ? RGB(120, 130, 145) : m_foreground);
    CFont* font = GetFont();
    CFont* oldFont = font ? dc.SelectObject(font) : nullptr;
    dc.DrawText(caption, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (oldFont) dc.SelectObject(oldFont);
    dc.Detach();
}

CHandoverRegisterDialog::CHandoverRegisterDialog(
    const CString& defaultUser, CWnd* parent)
    : CDialog(), m_defaultUser(defaultUser)
{
    BuildTemplate();
    InitModalIndirect(reinterpret_cast<LPCDLGTEMPLATE>(m_template.data()), parent);
}

BOOL CHandoverRegisterDialog::OnInitDialog()
{
    CDialog::OnInitDialog();
    SetWindowText(_T("인수인계 등록"));
    SetWindowPos(nullptr, 0, 0, 838, 807,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    CenterWindow(GetParent());

    m_normalFont.CreatePointFont(100, _T("맑은 고딕"));
    m_boldFont.CreatePointFont(105, _T("맑은 고딕"));
    m_titleFont.CreatePointFont(185, _T("맑은 고딕"));
    m_sectionFont.CreatePointFont(115, _T("맑은 고딕"));
    m_smallFont.CreatePointFont(90, _T("맑은 고딕"));
    m_whiteBrush.CreateSolidBrush(RGB(255, 255, 255));
    m_headerBrush.CreateSolidBrush(RGB(29, 43, 64));

    CreateLabel(m_headerTitle, _T("인수인계 등록"), CRect(30, 17, 390, 54), &m_titleFont);
    CreateLabel(m_headerSubtitle, _T("HANDOVER  /  PRODUCTION MANAGEMENT"),
        CRect(31, 54, 520, 75), &m_smallFont);

    // 기본 정보 카드
    CreateLabel(m_userLabel, _T("작성자"), CRect(44, 119, 140, 143));
    m_userEdit.Create(WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
        CRect(44, 145, 268, 177), this, 30101);
    m_userEdit.SetFont(&m_normalFont);
    m_userEdit.SetWindowText(m_defaultUser);
    CreateLabel(m_shiftLabel, _T("근무"), CRect(294, 119, 380, 143));
    m_shiftCombo.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        CRect(294, 145, 492, 300), this, 30102);
    m_shiftCombo.SetFont(&m_normalFont);
    m_shiftCombo.AddString(_T("오전"));
    m_shiftCombo.AddString(_T("오후"));
    m_shiftCombo.SetCurSel(CTime::GetCurrentTime().GetHour() < 13 ? 0 : 1);
    CreateLabel(m_priorityLabel, _T("중요도"), CRect(518, 119, 610, 143));
    m_priorityCombo.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        CRect(518, 145, 786, 300), this, 30103);
    m_priorityCombo.SetFont(&m_normalFont);
    m_priorityCombo.AddString(_T("긴급"));
    m_priorityCombo.AddString(_T("주의"));
    m_priorityCombo.AddString(_T("일반"));
    m_priorityCombo.SetCurSel(2);

    // 생산 현황 카드
    CreateLabel(m_productionLabel, _T("생산 현황"), CRect(44, 215, 220, 244), &m_sectionFont);
    CreateLabel(m_totalLabel, _T("총 검사"), CRect(44, 258, 120, 283));
    CreateNumberEdit(m_totalEdit, CRect(125, 253, 220, 285), 30104);
    CreateLabel(m_passLabel, _T("정상"), CRect(275, 258, 328, 283));
    CreateNumberEdit(m_passEdit, CRect(330, 253, 425, 285), 30105);
    CreateLabel(m_failLabel, _T("불량"), CRect(483, 258, 540, 283));
    CreateNumberEdit(m_failEdit, CRect(542, 253, 637, 285), 30106);
    CreateLabel(m_countUnitLabel, _T("단위  개"), CRect(694, 258, 775, 283), &m_smallFont);

    // 설비 상태 카드
    CreateLabel(m_equipmentLabel, _T("설비 상태"), CRect(44, 319, 230, 348), &m_sectionFont);
    m_equipmentHint.Create(_T("이상이 있는 설비만 선택하세요. 미선택 항목은 정상으로 기록됩니다."),
        WS_CHILD | WS_VISIBLE, CRect(44, 352, 770, 374), this, 30130);
    m_equipmentHint.SetFont(&m_smallFont);
    const LPCTSTR names[4] = {
        _T("카메라 이상"), _T("컨베이어 / 모터 이상"),
        _T("서보 이상"), _T("LED / 부저 이상")
    };
    for (int i = 0; i < 4; ++i)
    {
        const int x = 44 + (i % 2) * 375;
        const int y = 382 + (i / 2) * 29;
        m_equipmentChecks[i].Create(names[i],
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            CRect(x, y, x + 330, y + 25), this, 30110 + i);
        m_equipmentChecks[i].SetFont(&m_normalFont);
    }

    // 상세 입력 카드
    CreateLabel(m_actionLabel, _T("조치 내용"), CRect(44, 456, 250, 484), &m_sectionFont);
    CreateMultilineEdit(m_actionEdit, CRect(44, 487, 786, 547), 30120);
    m_actionEdit.SetWindowText(_T("없음"));
    CreateLabel(m_pendingLabel, _T("미완료 사항 및 다음 작업"),
        CRect(44, 572, 460, 601), &m_sectionFont);
    CreateMultilineEdit(m_pendingEdit, CRect(44, 603, 786, 663), 30121);
    m_pendingEdit.SetWindowText(_T("없음"));

    m_cancelButton.Create(_T("취소"),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        CRect(543, 699, 657, 742), this, IDCANCEL);
    m_cancelButton.SetFont(&m_boldFont);
    m_cancelButton.SetPalette(RGB(238, 242, 248), RGB(57, 72, 92), RGB(218, 226, 236));
    m_okButton.Create(_T("등록하기"),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        CRect(670, 699, 786, 742), this, IDOK);
    m_okButton.SetFont(&m_boldFont);
    m_okButton.SetPalette(RGB(49, 111, 237), RGB(255, 255, 255), RGB(49, 111, 237));
    m_totalEdit.SetFocus();
    return FALSE;
}

void CHandoverRegisterDialog::DrawCard(CDC& dc, const CRect& rect)
{
    dc.FillSolidRect(rect, RGB(255, 255, 255));
    dc.Draw3dRect(rect, RGB(226, 232, 241), RGB(226, 232, 241));
}

void CHandoverRegisterDialog::OnPaint()
{
    CPaintDC dc(this);
    CRect client;
    GetClientRect(&client);
    dc.FillSolidRect(client, RGB(245, 247, 251));
    dc.FillSolidRect(CRect(0, 0, client.right, 88), RGB(29, 43, 64));
    DrawCard(dc, CRect(22, 102, 808, 192));
    DrawCard(dc, CRect(22, 203, 808, 301));
    DrawCard(dc, CRect(22, 308, 808, 440));
    DrawCard(dc, CRect(22, 448, 808, 557));
    DrawCard(dc, CRect(22, 564, 808, 673));
    dc.FillSolidRect(CRect(22, 690, 808, 691), RGB(225, 232, 242));
}

HBRUSH CHandoverRegisterDialog::OnCtlColor(CDC* dc, CWnd* wnd, UINT type)
{
    if (type == CTLCOLOR_STATIC || type == CTLCOLOR_BTN)
    {
        const HWND h = wnd ? wnd->GetSafeHwnd() : nullptr;
        if (h == m_headerTitle.GetSafeHwnd() || h == m_headerSubtitle.GetSafeHwnd())
        {
            dc->SetTextColor(h == m_headerTitle.GetSafeHwnd() ? RGB(255, 255, 255) : RGB(175, 193, 219));
            dc->SetBkColor(RGB(29, 43, 64));
            return static_cast<HBRUSH>(m_headerBrush.GetSafeHandle());
        }
        dc->SetTextColor(h == m_equipmentHint.GetSafeHwnd() || h == m_countUnitLabel.GetSafeHwnd()
            ? RGB(108, 122, 145) : RGB(35, 51, 76));
        dc->SetBkColor(RGB(255, 255, 255));
        return static_cast<HBRUSH>(m_whiteBrush.GetSafeHandle());
    }
    if (type == CTLCOLOR_EDIT)
    {
        dc->SetTextColor(RGB(35, 51, 76));
        dc->SetBkColor(RGB(255, 255, 255));
        return static_cast<HBRUSH>(m_whiteBrush.GetSafeHandle());
    }
    return CDialog::OnCtlColor(dc, wnd, type);
}

void CHandoverRegisterDialog::OnOK()
{
    m_userEdit.GetWindowText(m_data.userName);
    m_data.userName.Trim();
    if (m_data.userName.IsEmpty())
    {
        AfxMessageBox(_T("작성자 이름을 입력해주세요."));
        m_userEdit.SetFocus();
        return;
    }

    long long total = 0;
    long long passed = 0;
    long long failed = 0;
    if (!ReadCount(m_totalEdit, _T("총 검사"), total) ||
        !ReadCount(m_passEdit, _T("정상"), passed) ||
        !ReadCount(m_failEdit, _T("불량"), failed))
        return;

    if (total != passed + failed)
    {
        AfxMessageBox(
            _T("총 검사 수는 '정상 + 불량'과 같아야 합니다."),
            MB_OK | MB_ICONWARNING);
        m_totalEdit.SetFocus();
        return;
    }

    CString action;
    CString pending;
    m_actionEdit.GetWindowText(action);
    m_pendingEdit.GetWindowText(pending);
    action.Trim();
    pending.Trim();
    if (action.IsEmpty()) action = _T("없음");
    if (pending.IsEmpty()) pending = _T("없음");

    const LPCTSTR equipmentNames[] =
    {
        _T("카메라"),
        _T("컨베이어 / 모터"),
        _T("서보"),
        _T("LED / 부저")
    };

    CString equipmentText;
    for (int index = 0; index < 4; ++index)
    {
        CString line;
        line.Format(
            _T("%s 이상: %s\r\n"),
            equipmentNames[index],
            m_equipmentChecks[index].GetCheck() == BST_CHECKED ? _T("유") : _T("무"));
        equipmentText += line;
    }

    m_data.message.Format(
        _T("[생산 현황]\r\n")
        _T("총 검사: %lld개 / 정상: %lld개 / 불량: %lld개\r\n\r\n")
        _T("[설비 상태]\r\n%s\r\n")
        _T("[조치 내용]\r\n%s\r\n\r\n")
        _T("[미완료 사항 및 다음 작업]\r\n%s"),
        total, passed, failed,
        equipmentText.GetString(), action.GetString(), pending.GetString());

    m_data.workDate = CTime::GetCurrentTime().Format(_T("%Y-%m-%d"));
    m_data.shiftType = m_shiftCombo.GetCurSel() == 1 ? _T("PM") : _T("AM");
    m_data.priority = _T("NORMAL");
    if (m_priorityCombo.GetCurSel() == 0) m_data.priority = _T("URGENT");
    else if (m_priorityCombo.GetCurSel() == 1) m_data.priority = _T("WARNING");

    CDialog::OnOK();
}

void CHandoverRegisterDialog::BuildTemplate()
{
    DLGTEMPLATE dialog = {};
    dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT;
    dialog.cdit = 0;
    dialog.cx = 330;
    dialog.cy = 225;
    const BYTE* header = reinterpret_cast<const BYTE*>(&dialog);
    m_template.assign(header, header + sizeof(dialog));
    AppendWord(m_template, 0);
    AppendWord(m_template, 0);
    AppendWideString(m_template, L"인수인계 등록");
    AppendWord(m_template, 10);
    AppendWideString(m_template, L"Segoe UI");
}

void CHandoverRegisterDialog::CreateLabel(CStatic& label, const CString& text, const CRect& rect, CFont* font)
{
    label.Create(text, WS_CHILD | WS_VISIBLE, rect, this);
    label.SetFont(font ? font : &m_boldFont);
}

void CHandoverRegisterDialog::CreateNumberEdit(CEdit& edit, const CRect& rect, UINT id)
{
    edit.Create(
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_NUMBER | ES_RIGHT,
        rect, this, id);
    edit.SetFont(&m_normalFont);
    edit.SetLimitText(9);
    edit.SetWindowText(_T("0"));
}

void CHandoverRegisterDialog::CreateMultilineEdit(CEdit& edit, const CRect& rect, UINT id)
{
    edit.Create(
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP |
        ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL,
        rect, this, id);
    edit.SetFont(&m_normalFont);
    edit.SetLimitText(2000);
}

bool CHandoverRegisterDialog::ReadCount(CEdit& edit, LPCTSTR fieldName, long long& value)
{
    CString text;
    edit.GetWindowText(text);
    text.Trim();
    if (text.IsEmpty())
    {
        CString warning;
        warning.Format(_T("%s 수량을 입력해주세요."), fieldName);
        AfxMessageBox(warning, MB_OK | MB_ICONWARNING);
        edit.SetFocus();
        return false;
    }

    for (int index = 0; index < text.GetLength(); ++index)
    {
        if (!_istdigit(text[index]))
        {
            CString warning;
            warning.Format(_T("%s 수량은 숫자로 입력해주세요."), fieldName);
            AfxMessageBox(warning, MB_OK | MB_ICONWARNING);
            edit.SetFocus();
            return false;
        }
    }

    value = _ttoi64(text);
    return true;
}

void CHandoverRegisterDialog::AppendWord(std::vector<BYTE>& target, WORD value)
{
    const BYTE* source = reinterpret_cast<const BYTE*>(&value);
    target.insert(target.end(), source, source + sizeof(value));
}

void CHandoverRegisterDialog::AppendWideString(
    std::vector<BYTE>& target,
    const wchar_t* value)
{
    const size_t bytes = (wcslen(value) + 1) * sizeof(wchar_t);
    const BYTE* source = reinterpret_cast<const BYTE*>(value);
    target.insert(target.end(), source, source + bytes);
}

