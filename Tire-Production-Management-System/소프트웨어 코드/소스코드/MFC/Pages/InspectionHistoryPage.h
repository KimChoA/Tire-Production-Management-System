
#pragma once

#include "../BasePage.h"
#include "DashboardPage.h"

#include <atlimage.h>
#include <vector>

class CNgImagePreview : public CWnd
{
public:
    BOOL CreatePreview(
        CWnd* parent,
        const CRect& rect,
        UINT id);

    void ShowImage(
        const CString& path,
        const CString& caption);

    void ShowMessage(const CString& message);

protected:
    CImage m_image;
    CString m_caption;
    bool m_loaded = false;

    afx_msg void OnPaint();
    DECLARE_MESSAGE_MAP()
};

class CInspectionHistoryPage : public CBasePage
{
public:
    BOOL CreatePage(CWnd* p, const CRect& r, UINT id);

    void SetInspectionRows(
        const std::vector<DashboardInspectionRow>& rows);

protected:
    enum
    {
        IDC_HISTORY_SEARCH = 33501,
        IDC_HISTORY_LIST = 33502,
        IDC_YOLO_PREVIEW = 33503,
        IDC_OPENCV_PREVIEW = 33504
    };

    CEdit* m_dateFilter = nullptr;
    CEdit* m_resultFilter = nullptr;
    CListCtrl* m_list = nullptr;

    CModernCard* m_imageCard = nullptr;
    CModernCard* m_detailCard = nullptr;

    CNgImagePreview m_yoloPreview;
    CNgImagePreview m_opencvPreview;

    std::vector<DashboardInspectionRow> m_allRows;
    std::vector<size_t> m_visibleIndices;

    void PopulateList();

    void LoadNgImages(
        const DashboardInspectionRow& rec);

    static bool DownloadPiImage(
        const CString& tireId,
        const CString& camera,
        CString& localPath,
        CString& error);

    afx_msg void OnSearch();
    afx_msg void OnRowSelected(
        NMHDR* header,
        LRESULT* result);

    DECLARE_MESSAGE_MAP()
};
