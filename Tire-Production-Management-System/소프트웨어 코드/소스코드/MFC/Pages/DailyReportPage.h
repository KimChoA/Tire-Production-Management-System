#pragma once

#include "../BasePage.h"
#include "HandoverTypes.h"
#include <vector>

class CDailyReportPage : public CBasePage
{
public:
    BOOL CreatePage(CWnd* p, const CRect& r, UINT id);

    // �μ�/�̸�����/PDF�� ��� ���� ���̾ƿ��� ����մϴ�.
    void DrawReport(CDC& dc, const CRect& pageRect) const;
    void DrawReportPage(CDC& dc, const CRect& pageRect, int pageIndex) const;
    int GetReportPageCount() const;

private:
    enum
    {
        IDC_REPORT_SEARCH = 3601,
        IDC_REPORT_PREVIEW = 3602,
        IDC_REPORT_PDF = 3603,
        IDC_REPORT_EXCEL = 3604,
        IDC_REPORT_PRINT = 3605
    };

    struct DefectRecord
    {
        CString inspectionId;
        CString inspectedAt;
        CString tireId;
        CString defectType;
        CString detail;
        CString imagePath;
    };

    struct ReportViewData
    {
        bool sample = false;
        CString date;
        CString author;
        CString shift;
        CString generatedAt;

        long long total = 0;
        long long pass = 0;
        long long fail = 0;
        double failRate = 0.0;
        bool fromDatabase = false;
        long long estopCount = 0, recheckCount = 0, clearCount = 0;
        long long yoloCount = 0, yoloSeconds = 0, activeYolo = 0;
        long long estopSeconds = 0, recheckSeconds = 0;
        long long activeEstop = 0, activeRecheck = 0;
        long long estopReviewCount = 0, recheckReviewCount = 0, yoloReviewCount = 0;
        long long estopValidClosed = 0, recheckValidClosed = 0, yoloValidClosed = 0;
        bool equipmentStatsAvailable = false;
        long long wheelDamage = 0;
        long long tireDamage = 0;
        long long bothDamage = 0;
        long long wrongTire = 0;
        long long unclassified = 0;
        std::vector<DefectRecord> defectRecords;

        std::vector<int> hourlyTotal;
        std::vector<int> hourlyPass;
        std::vector<int> hourlyFail;
        std::vector<CString> hourlyLabel;

        std::vector<HandoverItem> handovers;
    };

    CEdit* m_dateEdit = nullptr;
    CListBox* m_previewList = nullptr;
    CModernCard m_totalCard;
    CModernCard m_passCard;
    CModernCard m_failCard;
    CModernCard m_rateCard;

    std::vector<HandoverItem> m_filteredHandovers;
    long long m_total = 0;
    long long m_pass = 0;
    long long m_fail = 0;
    bool m_hasServerReport = false;
    ReportViewData m_serverReport;
    bool LoadServerReport(const CString& date, ReportViewData& report) const;

    CString GetReportDate() const;
    void RefreshReport();
    void UpdateSummaryCards();
    CString BuildReportText() const;
    ReportViewData BuildReportViewData() const;

    // ���� OOXML ��Ű��(.xlsx)�� �����Ͽ� �Ѽ�/Excel ȣȯ���� Ȯ���մϴ�.
    bool ExportExcelXlsx(const CString& path) const;
    bool PrintReportToDC(HDC hdc, const CString& documentName, const CString* outputPath = nullptr) const;

    static bool ExtractCount(const CString& message, LPCTSTR label, long long& value);

    afx_msg void OnSearch();
    afx_msg void OnPreview();
    afx_msg void OnPdf();
    afx_msg void OnExcel();
    afx_msg void OnPrint();
 
    DECLARE_MESSAGE_MAP()
};
