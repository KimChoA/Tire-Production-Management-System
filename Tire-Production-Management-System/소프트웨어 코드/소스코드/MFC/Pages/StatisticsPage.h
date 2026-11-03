#pragma once

#include "../BasePage.h"
#include <array>

class CStatisticsPage : public CBasePage
{
public:
    BOOL CreatePage(CWnd* p, const CRect& r, UINT id);
    void RefreshFromServer(bool showError = false);

private:
    CEdit* m_startDateEdit = nullptr;
    CEdit* m_endDateEdit = nullptr;
    CModernButton* m_searchButton = nullptr;

    CModernCard m_totalCard;
    CModernCard m_passCard;
    CModernCard m_failCard;
    CModernCard m_rateCard;

    // Verified hardware inspection data from central server.
    std::array<int, 8> m_hourlyTotal{};
    std::array<int, 8> m_hourlyNg{};
    std::array<int, 7> m_dailyTotal{};
    std::array<int, 7> m_dailyNg{};

    CString m_rangeText;
    int m_total = 0;
    int m_pass = 0;
    int m_fail = 0;

    void ClearRealData();
    void UpdateSummary();
    void DrawPanel(CDC& dc, const CRect& rect, const CString& title);
    void DrawHourlyGraph(CDC& dc, const CRect& rect);
    void DrawPassNgGraph(CDC& dc, const CRect& rect);
    void DrawDailyGraph(CDC& dc, const CRect& rect);

    afx_msg void OnSearch();
    afx_msg void OnPaint();

    DECLARE_MESSAGE_MAP()
};
