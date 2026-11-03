#include "pch.h"
#include "MainFrame.h"
#include "TireProductionMFC.h"
#include <shellapi.h>

BEGIN_MESSAGE_MAP(CMainFrame, CFrameWnd)
    ON_WM_CREATE()
    ON_WM_SIZE()
    ON_WM_ERASEBKGND()
    ON_WM_CTLCOLOR()
    ON_WM_TIMER() // 타이머 메시지 연결
    ON_COMMAND(ID_NAV_DASHBOARD, &CMainFrame::OnDashboard)
    ON_COMMAND(ID_NAV_HISTORY, &CMainFrame::OnHistory)
    ON_COMMAND(ID_NAV_EQUIPMENT, &CMainFrame::OnEquipment)
    ON_COMMAND(ID_NAV_STATISTICS, &CMainFrame::OnStatistics)
    ON_COMMAND(ID_NAV_CHAT, &CMainFrame::OnChat)
    ON_COMMAND(ID_NAV_REPORT, &CMainFrame::OnReport)
    ON_COMMAND(ID_NAV_HOME, &CMainFrame::OnHome)
END_MESSAGE_MAP()

BOOL CMainFrame::CreateMainFrame()
{
    return Create(
        nullptr,
        _T("Smart Production Line Monitoring"),
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CRect(70, 45, 1450, 890)
    );
}

int CMainFrame::OnCreate(LPCREATESTRUCT lpCreateStruct)
{
    if (CFrameWnd::OnCreate(lpCreateStruct) == -1)
        return -1;

    m_headerFont.CreatePointFont(185, _T("Segoe UI Semibold"));
    m_subtitleFont.CreatePointFont(90, _T("Segoe UI"));
    m_menuFont.CreatePointFont(103, _T("Segoe UI Semibold"));
    m_smallFont.CreatePointFont(90, _T("Segoe UI"));

    m_headerBrush.CreateSolidBrush(UiTheme::Header());
    m_sidebarBrush.CreateSolidBrush(UiTheme::Sidebar());
    m_footerBrush.CreateSolidBrush(UiTheme::CardBg());

    m_headerTitle.Create(
        _T("TIRE PRODUCTION MONITORING"),
        WS_CHILD | WS_VISIBLE,
        CRect(26, 13, 620, 46),
        this
    );
    m_headerTitle.SetFont(&m_headerFont);

    m_headerSubtitle.Create(
        _T("Smart Factory · Production & Quality Management"),
        WS_CHILD | WS_VISIBLE,
        CRect(28, 46, 620, 65),
        this
    );
    m_headerSubtitle.SetFont(&m_subtitleFont);

    CString roleText;

    if (g_userRole == _T("ADMIN"))
        roleText = _T("관리자");
    else if (g_userRole == _T("OPERATOR"))
        roleText = _T("현장 작업자");
    else
        roleText = g_userRole;

    CString statusText;

    statusText.Format(
        _T("%s  |  %s    ●  SYSTEM READY"),
        g_userName.GetString(),
        roleText.GetString()
    );

    m_systemStatus.Create(
        statusText,
        WS_CHILD | WS_VISIBLE | SS_RIGHT,
        CRect(720, 25, 1325, 52),
        this
    );

    m_systemStatus.SetFont(&m_menuFont);

    m_sideCaption.Create(
        _T("MENU"),
        WS_CHILD | WS_VISIBLE,
        CRect(24, 90, 180, 112),
        this
    );
    m_sideCaption.SetFont(&m_smallFont);

    m_footerStatus.Create(
        _T("DB  --     TCP/IP  --     Raspberry Pi  --"),
        WS_CHILD | WS_VISIBLE,
        CRect(248, 760, 1050, 785),
        this
    );
    m_footerStatus.SetFont(&m_smallFont);

    CreateNavigation();
    CreatePages();
    ShowPage(&m_dashboardPage);

    // 하트비트 감시 타이머 시작 (1초 주기)
    m_lastPiHeartbeat = 0;
    m_isPiAlarmActive = FALSE;
    m_bReceivedFirstHeartbeat = FALSE;
    SetTimer(9001, 1000, NULL);

    return 0;
}

void CMainFrame::CreateNavigation()
{
    struct Nav
    {
        CModernButton* btn;
        LPCTSTR text;
        UINT id;
    };

    Nav items[] =
    {
        { &m_btnDashboard, _T("생산 대시보드"), ID_NAV_DASHBOARD },
        { &m_btnHistory, _T("검사 이력 · NG 이미지"), ID_NAV_HISTORY },
        { &m_btnEquipment, _T("설비 모니터링"), ID_NAV_EQUIPMENT },
        { &m_btnStatistics, _T("생산 통계 · 그래프"), ID_NAV_STATISTICS },
        { &m_btnChat, _T("채팅 · 인수인계"), ID_NAV_CHAT },
        { &m_btnReport, _T("일일 보고서"), ID_NAV_REPORT }
    };

    int y = 120;

    for (auto& item : items)
    {
        item.btn->Create(
            item.text,
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
            CRect(18, y, 218, y + 46),
            this,
            item.id
        );

        item.btn->SetNavigationMode(true);
        item.btn->SetFont(&m_menuFont);

        y += 54;
    }

    m_btnHome.Create(
        _T("홈 / 로그인 · 화면 전환"),
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
        CRect(18, y + 20, 218, y + 68),
        this,
        ID_NAV_HOME);
    m_btnHome.SetNavigationMode(true);
    m_btnHome.SetFont(&m_menuFont);
}

void CMainFrame::CreatePages()
{
    CRect r(246, 88, 1340, 785);

    m_contentRect = r;

    m_dashboardPage.CreatePage(this, r, 3001);
    m_historyPage.CreatePage(this, r, 3002);
    m_equipmentPage.CreatePage(this, r, 3003);
    m_statisticsPage.CreatePage(this, r, 3004);
    m_chatPage.CreatePage(this, r, 3005);
    m_reportPage.CreatePage(this, r, 3006);
}

void CMainFrame::ShowPage(CBasePage* page)
{
    CBasePage* pages[] =
    {
        &m_dashboardPage,
        &m_historyPage,
        &m_equipmentPage,
        &m_statisticsPage,
        &m_chatPage,
        &m_reportPage
    };

    for (auto* p : pages)
        p->ShowWindow(p == page ? SW_SHOW : SW_HIDE);

    m_currentPage = page;

    UpdateNavState();
}

void CMainFrame::UpdateNavState()
{
    m_btnDashboard.SetActive(m_currentPage == &m_dashboardPage);
    m_btnHistory.SetActive(m_currentPage == &m_historyPage);
    m_btnEquipment.SetActive(m_currentPage == &m_equipmentPage);
    m_btnStatistics.SetActive(m_currentPage == &m_statisticsPage);
    m_btnChat.SetActive(m_currentPage == &m_chatPage);
    m_btnReport.SetActive(m_currentPage == &m_reportPage);
}

void CMainFrame::ResizeLayout(int cx, int cy)
{
    if (cx <= 0 || cy <= 0)
        return;

    m_headerTitle.MoveWindow(
        26,
        12,
        max(470, cx - 520),
        36
    );

    m_headerSubtitle.MoveWindow(
        28,
        46,
        max(470, cx - 520),
        20
    );

    m_systemStatus.MoveWindow(
        max(600, cx - 700),
        25,
        660,
        28
    );

    m_sideCaption.MoveWindow(
        24,
        88,
        170,
        22
    );

    m_footerStatus.MoveWindow(
        248,
        max(710, cy - 38),
        max(520, cx - 280),
        24
    );

    m_contentRect = CRect(
        246,
        88,
        max(850, cx - 20),
        max(640, cy - 50)
    );

    CBasePage* pages[] =
    {
        &m_dashboardPage,
        &m_historyPage,
        &m_equipmentPage,
        &m_statisticsPage,
        &m_chatPage,
        &m_reportPage
    };

    for (auto* p : pages)
    {
        if (::IsWindow(p->GetSafeHwnd()))
            p->MoveWindow(m_contentRect);
    }

    Invalidate(FALSE);
}

void CMainFrame::OnSize(UINT nType, int cx, int cy)
{
    CFrameWnd::OnSize(nType, cx, cy);

    if (::IsWindow(m_headerTitle.GetSafeHwnd()))
        ResizeLayout(cx, cy);
}

BOOL CMainFrame::OnEraseBkgnd(CDC* pDC)
{
    CRect rc;
    GetClientRect(&rc);

    pDC->FillSolidRect(
        rc,
        UiTheme::PageBg()
    );

    pDC->FillSolidRect(
        CRect(0, 0, rc.right, 76),
        UiTheme::Header()
    );

    pDC->FillSolidRect(
        CRect(0, 76, 236, rc.bottom),
        UiTheme::Sidebar()
    );

    pDC->FillSolidRect(
        CRect(
            236,
            max(0, rc.bottom - 46),
            rc.right,
            rc.bottom
        ),
        UiTheme::CardBg()
    );

    pDC->FillSolidRect(
        CRect(
            235,
            76,
            236,
            rc.bottom
        ),
        UiTheme::Border()
    );

    pDC->FillSolidRect(
        CRect(
            236,
            max(0, rc.bottom - 47),
            rc.right,
            max(0, rc.bottom - 46)
        ),
        UiTheme::Border()
    );

    return TRUE;
}

HBRUSH CMainFrame::OnCtlColor(
    CDC* pDC,
    CWnd* pWnd,
    UINT nCtlColor
)
{
    if (nCtlColor == CTLCOLOR_STATIC)
    {
        if (pWnd->GetSafeHwnd() == m_headerTitle.GetSafeHwnd())
        {
            pDC->SetTextColor(RGB(255, 255, 255));
            pDC->SetBkColor(UiTheme::Header());

            return (HBRUSH)m_headerBrush.GetSafeHandle();
        }

        if (pWnd->GetSafeHwnd() == m_headerSubtitle.GetSafeHwnd())
        {
            pDC->SetTextColor(RGB(203, 213, 225));
            pDC->SetBkColor(UiTheme::Header());

            return (HBRUSH)m_headerBrush.GetSafeHandle();
        }

        if (pWnd->GetSafeHwnd() == m_systemStatus.GetSafeHwnd())
        {
            pDC->SetTextColor(RGB(134, 239, 172));
            pDC->SetBkColor(UiTheme::Header());

            return (HBRUSH)m_headerBrush.GetSafeHandle();
        }

        if (pWnd->GetSafeHwnd() == m_sideCaption.GetSafeHwnd())
        {
            pDC->SetTextColor(UiTheme::Muted());
            pDC->SetBkColor(UiTheme::Sidebar());

            return (HBRUSH)m_sidebarBrush.GetSafeHandle();
        }

        if (pWnd->GetSafeHwnd() == m_footerStatus.GetSafeHwnd())
        {
            pDC->SetTextColor(UiTheme::Muted());
            pDC->SetBkColor(UiTheme::CardBg());

            return (HBRUSH)m_footerBrush.GetSafeHandle();
        }
    }

    return CFrameWnd::OnCtlColor(
        pDC,
        pWnd,
        nCtlColor
    );
}


void CMainFrame::ApplyInspectionEvent(
    const CString& tireId,
    const CString& result,
    const CString& defectType)
{
    // 과거 호출 지점과의 호환을 유지하되, 실적은 DB 재조회로만 반영합니다.
    (void)tireId;
    (void)result;
    (void)defectType;
    RequestDashboardRefresh();
}


void CMainFrame::ApplyDashboardStats(
    ULONGLONG total, ULONGLONG passed,
    ULONGLONG failed, ULONGLONG wheel,
    ULONGLONG tire, ULONGLONG both,
    ULONGLONG wrong, ULONGLONG unknown,
    const std::vector<DashboardInspectionRow>& recent)
{
    m_dashboardPage.SetDatabaseStatistics(
        total, passed, failed,
        wheel, tire, both, wrong, unknown,
        recent);

    // 중앙 서버 검사 기록을 이력 페이지에도 전달
    m_historyPage.SetInspectionRows(recent);
}

void CMainFrame::RequestDashboardRefresh()
{
    m_chatPage.RequestDashboardStats();
}

bool CMainFrame::SubmitDemoInspection(const CString& verdict, const CString& defectType)
{
    return m_chatPage.SubmitDemoInspection(verdict, defectType);
}

void CMainFrame::ApplyEquipmentEvent(
    const CString& equipment,
    const CString& status,
    const CString& message)
{
    // 현장 재확인/비상정지를 관리자에게 한번만 경고합니다.
    if (equipment.CompareNoCase(_T("ALERT")) == 0)
    {
        if (status.CompareNoCase(_T("RECHECK")) == 0)
        {
            m_systemStatus.SetWindowText(_T("[재확인 필요] 검사 결과 확인 / 컨베이어 정지"));
            if (!m_recheckAlertShown)
            {
                m_recheckAlertShown = true;
                AfxMessageBox(CString(_T("[재확인 경고] ")) + message,
                    MB_OK | MB_ICONWARNING);
            }
            m_equipmentPage.ApplyStatus(1, _T("RECHECK"), _T("WARN"), message);
        }
        else if (status.CompareNoCase(_T("ESTOP")) == 0)
        {
            m_systemStatus.SetWindowText(_T("[비상정지] 컨베이어 정지"));
            if (!m_estopAlertShown)
            {
                m_estopAlertShown = true;
                AfxMessageBox(CString(_T("[비상정지 경고] ")) + message,
                    MB_OK | MB_ICONERROR);
            }
        }
        else if (status.CompareNoCase(_T("CLEAR")) == 0)
        {
            m_recheckAlertShown = false;
            m_estopAlertShown = false;
            m_systemStatus.SetWindowText(_T("● 장비 경고 해제 / 상태 확인"));
        }
        return;
    }

    int index = -1;

    if (equipment.CompareNoCase(_T("CONVEYOR")) == 0) index = 0;
    else if (equipment.CompareNoCase(_T("CAMERA")) == 0) index = 1;
    else if (equipment.CompareNoCase(_T("SENSOR")) == 0) index = 2;
    else if (equipment.CompareNoCase(_T("SERVO")) == 0) index = 3;
    else if (equipment.CompareNoCase(_T("RASPBERRY_PI")) == 0 ||
        equipment.CompareNoCase(_T("RASPBERRY PI")) == 0) index = 4;
    else if (equipment.CompareNoCase(_T("TCP/IP")) == 0 ||
        equipment.CompareNoCase(_T("TCPIP")) == 0) index = 5;
    else if (equipment.CompareNoCase(_T("LED")) == 0 ||
        equipment.CompareNoCase(_T("BUZZER")) == 0 ||
        equipment.CompareNoCase(_T("LED_BUZZER")) == 0) index = 6;
    else if (equipment.CompareNoCase(_T("MODE")) == 0 ||
        equipment.CompareNoCase(_T("OPERATION_MODE")) == 0) index = 7;

    if (index < 0)
        return;

    CString level = _T("INFO");
    if (status.CompareNoCase(_T("ERROR")) == 0 ||
        status.CompareNoCase(_T("DISCONNECTED")) == 0)
    {
        level = _T("ERROR");
    }
    else if (status.CompareNoCase(_T("WARN")) == 0 ||
        status.CompareNoCase(_T("RECHECK")) == 0)
    {
        level = _T("WARN");
    }

    m_equipmentPage.ApplyStatus(index, status, level, message);
}


void CMainFrame::OnHome()
{
    // 관리자 권한을 작업자 화면으로 직접 넘기지 않고 로그인 화면으로 이동한다.
    // 중앙 서버와 Raspberry Pi 프로세스는 종료하지 않는다.
    WCHAR modulePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    CString dir(modulePath);
    int slash = dir.ReverseFind(L'\\');
    if (slash >= 0) dir = dir.Left(slash);

    CString found;
    CString base = dir;
    for (int depth = 0; depth < 7 && found.IsEmpty(); ++depth)
    {
        CString candidates[] = {
            base + _T("\\TireEquipmentHMI.exe"),
            base + _T("\\Operator\\TireEquipmentHMI.exe"),
            base + _T("\\WinForms\\bin\\Debug\\net8.0-windows\\TireEquipmentHMI.exe"),
            base + _T("\\WinForms\\bin\\Release\\net8.0-windows\\TireEquipmentHMI.exe"),
            base + _T("\\WinForms\\bin\\x64\\Debug\\net8.0-windows\\TireEquipmentHMI.exe"),
            base + _T("\\WinForms\\bin\\x64\\Release\\net8.0-windows\\TireEquipmentHMI.exe")
        };
        for (const auto& candidate : candidates)
        {
            if (GetFileAttributesW(candidate) != INVALID_FILE_ATTRIBUTES)
            {
                found = candidate;
                break;
            }
        }
        int pos = base.ReverseFind(L'\\');
        if (pos < 0) break;
        base = base.Left(pos);
    }

    if (found.IsEmpty())
    {
        AfxMessageBox(_T("현장 프로그램 실행 파일을 찾을 수 없습니다.\nWinForms 프로젝트를 먼저 빌드하거나, MFC 실행 파일 옆 Operator 폴더에 TireEquipmentHMI.exe를 넣어주세요."));
        return;
    }

    HINSTANCE result = ShellExecuteW(
        m_hWnd, L"open", found, nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32)
    {
        AfxMessageBox(_T("로그인 화면 실행에 실패했습니다."));
        return;
    }
    // 새 로그인 화면을 연 뒤 관리자 UI만 닫는다.
    PostMessage(WM_CLOSE);
}

void CMainFrame::OnDashboard()
{
    ShowPage(&m_dashboardPage);
    RequestDashboardRefresh();
}


void CMainFrame::OnHistory()
{
    ShowPage(&m_historyPage);

    // 검사 이력 페이지 진입 시 실제 DB 정보 요청
    RequestDashboardRefresh();
}


void CMainFrame::OnEquipment()
{
    ShowPage(&m_equipmentPage);
}

void CMainFrame::OnStatistics()
{
    ShowPage(&m_statisticsPage);
    m_statisticsPage.RefreshFromServer();
}

void CMainFrame::OnChat()
{
    ShowPage(&m_chatPage);
}

void CMainFrame::OnReport()
{
    ShowPage(&m_reportPage);
}

// ChatHandoverPage에서 신호가 올 때마다 호출됨
void CMainFrame::OnHeartbeatReceived() {
    m_bReceivedFirstHeartbeat = TRUE;
    m_lastPiHeartbeat = CTime::GetCurrentTime();

    if (m_isPiAlarmActive) {
        m_isPiAlarmActive = FALSE;
        m_systemStatus.SetWindowText(_T("● 시스템 정상 (라즈베리파이 연결됨)"));
    }
}

// 1초마다 호출되어 5초 초과 여부를 감시
void CMainFrame::OnTimer(UINT_PTR nIDEvent) {
    if (nIDEvent == 9001) {
        // Live database refresh; no fabricated inspection rows.
        static int statsRefreshSeconds = 0;
        if (++statsRefreshSeconds >= 15)
        {
            statsRefreshSeconds = 0;
            RequestDashboardRefresh();
            if (m_currentPage == &m_statisticsPage)
                m_statisticsPage.RefreshFromServer();
        }
        // 신호를 한 번이라도 받았던 적이 있는 경우에만 검사
        if (m_bReceivedFirstHeartbeat && m_lastPiHeartbeat.GetTime() > 0) {
            CTime now = CTime::GetCurrentTime();
            CTimeSpan span = now - m_lastPiHeartbeat;

            // 5초 이상 신호가 없으면 비상 경보 발생
            if (span.GetTotalSeconds() >= 5 && !m_isPiAlarmActive) {
                m_isPiAlarmActive = TRUE;

                // 1. 상단 상태 라벨 빨간색 경보로 변경
                m_systemStatus.SetWindowText(_T("[경보] 장비 연결 끊김"));

                // 2. 관리실 경보 팝업 알림
                AfxMessageBox(_T("[비상 경보] 통신 신호가 5초 이상 두절되었습니다!\n현장 설비 및 랜선 상태를 확인하세요."), MB_ICONERROR);
            }
        }
    }

    CFrameWnd::OnTimer(nIDEvent);
}