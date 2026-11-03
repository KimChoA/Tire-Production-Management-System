#pragma once
#include "../BasePage.h"
#include <array>
#include <deque>
#include <atlimage.h>
#include <atomic>
#include <thread>

// ApplyStatus는 UI 스레드에서 호출한다.
class CEquipmentMonitorPage : public CBasePage
{
public:
    ~CEquipmentMonitorPage();
    BOOL CreatePage(CWnd* p, const CRect& r, UINT id);

    void ApplyStatus(
        int equipment,
        const CString& status,
        const CString& level,
        const CString& detail);

private:
    struct Event
    {
        CString time;
        CString equipment;
        CString level;
        CString detail;
    };

    std::array<CModernCard, 8> m_cards;
    std::array<CString, 8> m_states;

    CListCtrl m_log;
    CComboBox m_equipmentFilter;
    CComboBox m_levelFilter;
    CComboBox m_scenario;

    CModernButton m_apply;
    CStatic m_count;
    CStatic m_lastReceived;

    std::deque<Event> m_events;
    CImage m_topImage;
    CImage m_sideImage;
    std::atomic<bool> m_cameraStop{false};
    std::thread m_cameraWorker;
    CString m_cameraBaseUrl;

    CString m_yoloResult = _T("--");
    CString m_opencvResult = _T("--");
    CString m_finalResult = _T("--");
    CString m_inspectedTireId = _T("--");

    afx_msg LRESULT OnInspectionState(WPARAM, LPARAM);
    void StartCameraWorker();
    afx_msg LRESULT OnCameraFrame(WPARAM cameraIndex, LPARAM pathPointer);

    int m_scrollY = 0;
    static constexpr int ContentHeight = 1530;

    void RefreshLog();
    void UpdateScrollBar();
    void ScrollTo(int position);

    void DrawCameraPanel(
        CDC& dc,
        const CRect& rect,
        const CString& title,
        const CString& method,
        const CString& cameraName);

    afx_msg void OnPaint();
    afx_msg void OnSize(UINT nType, int cx, int cy);
    afx_msg void OnVScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar);
    afx_msg BOOL OnMouseWheel(UINT nFlags, short zDelta, CPoint pt);

    afx_msg void OnApplyScenario();
    afx_msg void OnFilterChanged();
    afx_msg void OnLogCustomDraw(NMHDR* header, LRESULT* result);
    afx_msg void OnLogDoubleClick(NMHDR* header, LRESULT* result);

    DECLARE_MESSAGE_MAP()
};
