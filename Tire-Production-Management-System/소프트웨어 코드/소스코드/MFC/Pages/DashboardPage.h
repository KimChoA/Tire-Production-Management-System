#pragma once

#include "../BasePage.h"
#include <vector>

struct DashboardInspectionRow
{
    CString time;
    CString tireId;
    CString verdict;
    CString defectType;
    CString detail;
};

class CDashboardPage : public CBasePage
{
public:
    BOOL CreatePage(CWnd* p, const CRect& r, UINT id);

    // 중앙 서버 SQLite 전체 통계로 화면을 동기화합니다 (UI 스레드에서 호출).
    void SetDatabaseStatistics(
        ULONGLONG total, ULONGLONG passed, ULONGLONG failed,
        ULONGLONG wheel, ULONGLONG tire, ULONGLONG both, ULONGLONG wrong, ULONGLONG unknown,
        const std::vector<DashboardInspectionRow>& recent);

    // 실제 검사 결과 연결 시 UI 스레드에서 호출한다.
    void AddInspection(
        const CString& productId,
        bool passed,
        const CString& detail);

private:
    CModernCard m_totalCard;
    CModernCard m_passCard;
    CModernCard m_failCard;
    CModernCard m_rateCard;
    CModernCard m_equipmentCard;

    CListCtrl m_recent;
    CModernButton m_sampleButton;

    ULONGLONG m_total = 0;
    ULONGLONG m_pass = 0;
    ULONGLONG m_fail = 0;
    ULONGLONG m_wheel = 0;
    ULONGLONG m_tire = 0;
    ULONGLONG m_both = 0;
    ULONGLONG m_wrong = 0;
    ULONGLONG m_unknown = 0;
    bool m_databaseLoaded = false;

    unsigned int m_sampleSequence = 0;

    void UpdateSummary();

    afx_msg void OnAddSample();
    afx_msg void OnPaint();
    afx_msg void OnRecentCustomDraw(
        NMHDR* header,
        LRESULT* result);

    DECLARE_MESSAGE_MAP()
};