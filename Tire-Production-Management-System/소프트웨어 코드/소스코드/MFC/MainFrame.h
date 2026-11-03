#pragma once
#include "framework.h"
#include "Theme/ThemeControls.h"
#include "Pages/DashboardPage.h"
#include "Pages/InspectionHistoryPage.h"
#include "Pages/EquipmentMonitorPage.h"
#include "Pages/StatisticsPage.h"
#include "Pages/ChatHandoverPage.h"
#include "Pages/DailyReportPage.h"
#include <vector>

#define ID_NAV_DASHBOARD   2001
#define ID_NAV_HISTORY     2002
#define ID_NAV_EQUIPMENT   2003
#define ID_NAV_STATISTICS  2004
#define ID_NAV_CHAT        2005
#define ID_NAV_REPORT      2006
#define ID_NAV_HOME        2007

class CMainFrame : public CFrameWnd
{
public:
    BOOL CreateMainFrame();

    // 하트비트 수신 알림 함수
    void OnHeartbeatReceived();

    void ApplyInspectionEvent(
        const CString& tireId,
        const CString& result,
        const CString& defectType);

    void ApplyDashboardStats(
        ULONGLONG total, ULONGLONG passed, ULONGLONG failed,
        ULONGLONG wheel, ULONGLONG tire, ULONGLONG both, ULONGLONG wrong, ULONGLONG unknown,
        const std::vector<DashboardInspectionRow>& recent);
    void RequestDashboardRefresh();
    bool SubmitDemoInspection(const CString& verdict, const CString& defectType);

    void ApplyEquipmentEvent(
        const CString& equipment,
        const CString& status,
        const CString& message);

    const std::vector<HandoverItem>& GetHandoverItems() const
    {
        return m_chatPage.GetHandovers();
    }

protected:
    CStatic m_headerTitle;
    CStatic m_headerSubtitle;
    CStatic m_systemStatus;
    CStatic m_sideCaption;
    CStatic m_footerStatus;

    CFont m_headerFont;
    CFont m_subtitleFont;
    CFont m_menuFont;
    CFont m_smallFont;

    CBrush m_headerBrush;
    CBrush m_sidebarBrush;
    CBrush m_footerBrush;

    CModernButton m_btnDashboard, m_btnHistory, m_btnEquipment, m_btnStatistics, m_btnChat, m_btnReport, m_btnHome;
    CRect m_contentRect;

    CDashboardPage m_dashboardPage;
    CInspectionHistoryPage m_historyPage;
    CEquipmentMonitorPage m_equipmentPage;
    CStatisticsPage m_statisticsPage;
    CChatHandoverPage m_chatPage;
    CDailyReportPage m_reportPage;
    CBasePage* m_currentPage = nullptr;

    // 5초 감시용 변수
    CTime m_lastPiHeartbeat;
    BOOL  m_isPiAlarmActive;
    BOOL  m_bReceivedFirstHeartbeat;
    bool m_recheckAlertShown = false;
    bool m_estopAlertShown = false;

    void CreateNavigation();
    void CreatePages();
    void ShowPage(CBasePage* page);
    void ResizeLayout(int cx, int cy);
    void UpdateNavState();

    afx_msg int OnCreate(LPCREATESTRUCT lpCreateStruct);
    afx_msg void OnSize(UINT nType, int cx, int cy);
    afx_msg BOOL OnEraseBkgnd(CDC* pDC);
    afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);
    afx_msg void OnDashboard();
    afx_msg void OnHistory();
    afx_msg void OnEquipment();
    afx_msg void OnStatistics();
    afx_msg void OnChat();
    afx_msg void OnReport();
    afx_msg void OnHome();

    // 타이머 메시지 핸들러
    afx_msg void OnTimer(UINT_PTR nIDEvent);
    DECLARE_MESSAGE_MAP()
};
