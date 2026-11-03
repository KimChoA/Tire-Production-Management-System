
#include "pch.h"
#include "DashboardPage.h"
#include "../MainFrame.h"

namespace
{
    constexpr UINT AddSampleButton = 3201;
    constexpr UINT RecentList = 3202;

    constexpr int MaxRecentRows = 50;

    const COLORREF COLOR_BG = RGB(245, 248, 252);
    const COLORREF COLOR_CARD = RGB(255, 255, 255);
    const COLORREF COLOR_BORDER = RGB(224, 232, 241);
    const COLORREF COLOR_NAVY = RGB(30, 55, 82);
    const COLORREF COLOR_MUTED = RGB(112, 130, 150);
    const COLORREF COLOR_BLUE = RGB(61, 146, 218);
    const COLORREF COLOR_GREEN = RGB(35, 169, 120);
    const COLORREF COLOR_RED = RGB(226, 79, 89);
    const COLORREF COLOR_TRACK = RGB(235, 241, 247);

    void DrawRoundPanel(CDC& dc, const CRect& rect)
    {
        CBrush brush(COLOR_CARD);
        CPen pen(PS_SOLID, 1, COLOR_BORDER);

        CBrush* oldBrush = dc.SelectObject(&brush);
        CPen* oldPen = dc.SelectObject(&pen);

        dc.RoundRect(
            rect.left,
            rect.top,
            rect.right,
            rect.bottom,
            16,
            16);

        dc.SelectObject(oldBrush);
        dc.SelectObject(oldPen);
    }
}

BEGIN_MESSAGE_MAP(CDashboardPage, CBasePage)
    ON_BN_CLICKED(
        AddSampleButton,
        &CDashboardPage::OnAddSample)

    ON_WM_PAINT()

    ON_NOTIFY(
        NM_CUSTOMDRAW,
        RecentList,
        &CDashboardPage::OnRecentCustomDraw)
END_MESSAGE_MAP()

BOOL CDashboardPage::CreatePage(
    CWnd* p,
    const CRect& r,
    UINT id)
{
    if (!CBasePage::CreatePage(p, r, id))
        return FALSE;

    AddTitle(_T("생산 대시보드"));

    // ===== 생산 요약 카드 =====

    m_totalCard.CreateCard(
        _T("TOTAL INSPECTIONS\r\n0"),
        this,
        CRect(28, 80, 248, 174));

    m_passCard.CreateCard(
        _T("PASS  |  정상\r\n0"),
        this,
        CRect(262, 80, 482, 174));

    m_failCard.CreateCard(
        _T("NG  |  불량\r\n0"),
        this,
        CRect(496, 80, 716, 174));

    m_rateCard.CreateCard(
        _T("DEFECT RATE\r\n0.0 %"),
        this,
        CRect(730, 80, 950, 174));

    // ===== 장비 상태 카드 =====
    // 실제 설비 상태 연동은 기존 로직을 유지합니다.

    m_equipmentCard.CreateCard(
        _T("EQUIPMENT STATUS\r\n")
        _T("Conveyor    미확인\r\n")
        _T("Camera       미확인\r\n")
        _T("Sensor        미확인\r\n")
        _T("Servo          미확인\r\n")
        _T("TCP/IP         미연결"),
        this,
        CRect(650, 196, 950, 470));

    // ===== 최근 검사 목록 =====

    AddLabel(
        _T("최근 검사 결과"),
        28, 496, 350, 26);

    // 실검사 수량은 Raspberry Pi -> 중앙 서버 -> SQLite 수신 전용.
    // 수동 시연 데이터 생성 버튼은 운영 화면에서 제거합니다.
    m_sampleButton.Create(
        _T("DB 새로고침"),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        CRect(778, 490, 950, 526),
        this, AddSampleButton);
    m_sampleButton.SetFont(&m_labelFont);

    m_recent.Create(
        WS_CHILD | WS_VISIBLE | WS_BORDER |
        LVS_REPORT | LVS_SINGLESEL,
        CRect(28, 536, 950, 662),
        this,
        RecentList);

    m_recent.SetExtendedStyle(
        LVS_EX_FULLROWSELECT |
        LVS_EX_GRIDLINES |
        LVS_EX_DOUBLEBUFFER);

    m_recent.SetFont(&m_normalFont);
    m_recent.SetBkColor(COLOR_CARD);
    m_recent.SetTextBkColor(COLOR_CARD);
    m_recent.SetTextColor(COLOR_NAVY);

    m_recent.InsertColumn(
        0, _T("검사 일시"),
        LVCFMT_LEFT, 185);

    m_recent.InsertColumn(
        1, _T("제품 ID"),
        LVCFMT_LEFT, 180);

    m_recent.InsertColumn(
        2, _T("판정"),
        LVCFMT_CENTER, 110);

    m_recent.InsertColumn(
        3, _T("검사 상세 내용"),
        LVCFMT_LEFT, 420);

    UpdateSummary();

    return TRUE;
}

void CDashboardPage::SetDatabaseStatistics(
    ULONGLONG total, ULONGLONG passed, ULONGLONG failed,
    ULONGLONG wheel, ULONGLONG tire, ULONGLONG both, ULONGLONG wrong, ULONGLONG unknown,
    const std::vector<DashboardInspectionRow>& recent)
{
    m_total = total;
    m_pass = passed;
    m_fail = failed;
    m_wheel = wheel;
    m_tire = tire;
    m_both = both;
    m_wrong = wrong;
    m_unknown = unknown;
    m_databaseLoaded = true;

    m_recent.SetRedraw(FALSE);
    m_recent.DeleteAllItems();
    for (const auto& record : recent)
    {
        const int row = m_recent.InsertItem(m_recent.GetItemCount(), record.time);
        m_recent.SetItemText(row, 1, record.tireId);
        CString result = record.verdict;
        result.MakeUpper();
        m_recent.SetItemText(row, 2,
            result == _T("OK") || result == _T("NORMAL") ? _T("PASS") :
            result == _T("FAIL") || result == _T("DEFECT") ? _T("NG") : result);
        CString defect;
        CString category = record.defectType;
        category.MakeUpper();
        if (category == _T("WHEEL_DAMAGE") || category == _T("WHEEL")) defect = _T("휠 손상");
        else if (category == _T("TIRE_DAMAGE") || category == _T("TIRE") || category == _T("RUBBER_DAMAGE")) defect = _T("타이어 고무 손상");
        else if (category == _T("BOTH_DAMAGE") || category == _T("BOTH") || category == _T("WHEEL_TIRE_DAMAGE")) defect = _T("동시 손상");
        else if (category == _T("WRONG_TIRE") || category == _T("NG_WRONG_TIRE")) defect = _T("이종 타이어(B형)");
        else if (result == _T("NG")) defect = _T("미분류");
        else defect = _T("정상");
        if (!record.detail.IsEmpty()) defect += _T(" | ") + record.detail;
        m_recent.SetItemText(row, 3, defect);
    }
    m_recent.SetRedraw(TRUE);
    m_recent.Invalidate(FALSE);
    UpdateSummary();
}

void CDashboardPage::AddInspection(
    const CString& productId,
    bool passed,
    const CString& detail)
{
    if (productId.IsEmpty())
        return;

    ++m_total;

    if (passed)
        ++m_pass;
    else
        ++m_fail;

    const CString time =
        CTime::GetCurrentTime().Format(
            _T("%Y-%m-%d %H:%M:%S"));

    const int row = m_recent.InsertItem(0, time);

    m_recent.SetItemText(
        row, 1, productId);

    m_recent.SetItemText(
        row, 2,
        passed ? _T("PASS") : _T("NG"));

    m_recent.SetItemText(
        row, 3, detail);

    if (m_recent.GetItemCount() > MaxRecentRows)
        m_recent.DeleteItem(MaxRecentRows);

    UpdateSummary();
}

void CDashboardPage::UpdateSummary()
{
    CString text;

    text.Format(
        _T("TOTAL INSPECTIONS\r\n%llu"),
        m_total);
    m_totalCard.SetText(text);

    text.Format(
        _T("PASS  |  정상\r\n%llu"),
        m_pass);
    m_passCard.SetText(text);

    text.Format(
        _T("NG  |  불량\r\n%llu"),
        m_fail);
    m_failCard.SetText(text);

    const double failRate =
        m_total == 0
        ? 0.0
        : static_cast<double>(m_fail) /
        static_cast<double>(m_total) * 100.0;

    text.Format(
        _T("DEFECT RATE\r\n%.1f %%"),
        failRate);

    m_rateCard.SetText(text);

    const CRect graphRect(
        28, 196, 628, 470);

    InvalidateRect(&graphRect, FALSE);
}

void CDashboardPage::OnAddSample()
{
    // DB를 변경하지 않고 서버에 현재 누적 수치를 다시 요청합니다.
    auto* frame = dynamic_cast<CMainFrame*>(GetParent());
    if (frame)
        frame->RequestDashboardRefresh();
}

void CDashboardPage::OnPaint()
{
    CPaintDC dc(this);
    dc.SetBkMode(TRANSPARENT);
    const CRect panel(28, 196, 628, 470);
    DrawRoundPanel(dc, panel);

    dc.FillSolidRect(48, 218, 4, 23, COLOR_BLUE);
    CFont* oldFont = dc.SelectObject(&m_labelFont);
    dc.SetTextColor(COLOR_NAVY);
    CRect title(64, 213, 580, 245);
    dc.DrawText(_T("불량 유형별 분석"), &title, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    dc.SelectObject(&m_normalFont);
    dc.SetTextColor(COLOR_MUTED);
    CRect subtitle(49, 249, 600, 271);
    dc.DrawText(_T("전체 불량(NG) 내 유형별 비중 · DB 누적"),
        &subtitle, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    const CString labels[] = {
        _T("휠 손상"), _T("타이어 고무"), _T("동시 손상"), _T("이종 타이어(B형)"), _T("미분류")
    };
    const ULONGLONG values[] = { m_wheel, m_tire, m_both, m_wrong, m_unknown };
    const COLORREF colors[] = {
        COLOR_BLUE, RGB(102, 126, 219), COLOR_RED, RGB(234, 168, 71), RGB(160, 173, 188)
    };
    for (int i = 0; i < 5; ++i)
    {
        const int y = 278 + i * 36;
        dc.SetTextColor(COLOR_NAVY);
        CRect label(50, y - 3, 160, y + 25);
        dc.DrawText(labels[i], &label, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        const int barX = 170, barW = 248, barH = 14;
        const int barY = y + 4;
        dc.FillSolidRect(barX, barY, barW, barH, COLOR_TRACK);
        const double percent = m_fail == 0 ? 0.0 :
            100.0 * static_cast<double>(values[i]) / static_cast<double>(m_fail);
        const int width = (std::min)(barW,
            static_cast<int>((percent / 100.0) * barW + 0.5));
        if (width > 0)
            dc.FillSolidRect(barX, barY, width, barH, colors[i]);

        CString value;
        value.Format(_T("%llu건  %.1f%%"), values[i], percent);
        dc.SetTextColor(COLOR_NAVY);
        CRect valueRect(431, y - 3, 609, y + 25);
        dc.DrawText(value, &valueRect, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    }
    dc.SelectObject(oldFont);
}

void CDashboardPage::OnRecentCustomDraw(
    NMHDR* header,
    LRESULT* result)
{
    auto* draw =
        reinterpret_cast<NMLVCUSTOMDRAW*>(header);

    *result = CDRF_DODEFAULT;

    if (draw->nmcd.dwDrawStage == CDDS_PREPAINT)
    {
        *result = CDRF_NOTIFYITEMDRAW;
    }
    else if (
        draw->nmcd.dwDrawStage ==
        CDDS_ITEMPREPAINT)
    {
        const int row =
            static_cast<int>(
                draw->nmcd.dwItemSpec);

        const CString inspectionResult =
            m_recent.GetItemText(row, 2);

        if (inspectionResult == _T("NG"))
        {
            draw->clrText = COLOR_RED;
            draw->clrTextBk =
                RGB(255, 244, 245);
        }
        else if (inspectionResult == _T("PASS"))
        {
            draw->clrText = COLOR_GREEN;
            draw->clrTextBk =
                RGB(242, 251, 247);
        }
        else
        {
            draw->clrText = COLOR_NAVY;
            draw->clrTextBk = COLOR_CARD;
        }
    }
}
