
#include "pch.h"
#include "InspectionHistoryPage.h"
#include "../MainFrame.h"

#include <winhttp.h>
#include <vector>
#include <string>

#pragma comment(lib, "winhttp.lib")

namespace
{
    constexpr wchar_t PI_HOST[] = L"192.168.0.209";
    constexpr INTERNET_PORT PI_PORT = 5001;

    bool IsSafeTireId(const CString& id)
    {
        if (id.GetLength() < 16 ||
            id.Left(5) != _T("TIRE-"))
            return false;

        for (int i = 0; i < id.GetLength(); ++i)
        {
            wchar_t c = id[i];

            if (!((c >= L'0' && c <= L'9') ||
                (c >= L'A' && c <= L'Z') ||
                (c >= L'a' && c <= L'z') ||
                c == L'-' || c == L'_'))
                return false;
        }

        return true;
    }

    CString GetPiToken()
    {
        wchar_t buffer[512] = {};

        DWORD length = GetEnvironmentVariableW(
            L"PI_CONTROL_TOKEN",
            buffer,
            _countof(buffer));

        if (length > 0 && length < _countof(buffer))
        {
            CString token(buffer);
            token.Trim();

            if (!token.IsEmpty())
                return token;
        }

        HKEY key = nullptr;

        if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            L"Environment",
            0,
            KEY_READ,
            &key) != ERROR_SUCCESS)
            return _T("");

        wchar_t value[512] = {};
        DWORD size = sizeof(value);
        DWORD type = 0;

        LONG result = RegQueryValueExW(
            key,
            L"PI_CONTROL_TOKEN",
            nullptr,
            &type,
            reinterpret_cast<LPBYTE>(value),
            &size);

        RegCloseKey(key);

        if (result != ERROR_SUCCESS ||
            (type != REG_SZ && type != REG_EXPAND_SZ))
            return _T("");

        value[_countof(value) - 1] = L'\0';

        CString token(value);
        token.Trim();
        return token;
    }
}

// ============================================================
// 이미지 미리보기 컨트롤
// ============================================================

BEGIN_MESSAGE_MAP(CNgImagePreview, CWnd)
    ON_WM_PAINT()
END_MESSAGE_MAP()

BOOL CNgImagePreview::CreatePreview(
    CWnd* parent,
    const CRect& rect,
    UINT id)
{
    CString className = AfxRegisterWndClass(
        CS_HREDRAW | CS_VREDRAW,
        ::LoadCursor(nullptr, IDC_ARROW),
        (HBRUSH)(COLOR_WINDOW + 1),
        nullptr);

    return Create(
        className,
        _T(""),
        WS_CHILD | WS_VISIBLE,
        rect,
        parent,
        id);
}

void CNgImagePreview::ShowImage(
    const CString& path,
    const CString& caption)
{
    m_image.Destroy();
    m_caption = caption;
    m_loaded = false;

    if (!path.IsEmpty())
    {
        HRESULT hr = m_image.Load(path);
        m_loaded = SUCCEEDED(hr);
    }

    if (!m_loaded)
        m_caption += _T(" - 이미지 없음");

    if (::IsWindow(GetSafeHwnd()))
        Invalidate(FALSE);
}

void CNgImagePreview::ShowMessage(
    const CString& message)
{
    m_image.Destroy();
    m_loaded = false;
    m_caption = message;

    if (::IsWindow(GetSafeHwnd()))
        Invalidate(FALSE);
}

void CNgImagePreview::OnPaint()
{
    CPaintDC dc(this);

    CRect rc;
    GetClientRect(&rc);

    dc.FillSolidRect(rc, RGB(250, 251, 253));
    dc.SetBkMode(TRANSPARENT);
    dc.SetTextColor(RGB(45, 65, 95));

    CRect titleRect = rc;
    titleRect.DeflateRect(10, 5);
    titleRect.bottom = titleRect.top + 24;

    dc.DrawText(
        m_caption,
        titleRect,
        DT_LEFT | DT_VCENTER |
        DT_SINGLELINE | DT_END_ELLIPSIS);

    CRect imageRect = rc;
    imageRect.DeflateRect(10, 8);
    imageRect.top += 28;

    if (m_loaded && !m_image.IsNull())
    {
        int w = m_image.GetWidth();
        int h = m_image.GetHeight();

        if (w > 0 && h > 0 &&
            imageRect.Width() > 0 &&
            imageRect.Height() > 0)
        {
            double sx =
                static_cast<double>(imageRect.Width()) / w;

            double sy =
                static_cast<double>(imageRect.Height()) / h;

            double scale = (sx < sy) ? sx : sy;

            int dw = static_cast<int>(w * scale);
            int dh = static_cast<int>(h * scale);

            int x = imageRect.left +
                (imageRect.Width() - dw) / 2;

            int y = imageRect.top +
                (imageRect.Height() - dh) / 2;

            m_image.Draw(
                dc.GetSafeHdc(),
                x, y, dw, dh);
        }
    }
    else
    {
        dc.SetTextColor(RGB(145, 150, 160));

        dc.DrawText(
            _T("이미지 없음"),
            imageRect,
            DT_CENTER | DT_VCENTER |
            DT_SINGLELINE);
    }
}

// ============================================================
// NG 이미지 확대 팝업
// ============================================================

class CNgImagePopup : public CFrameWnd
{
public:
    BOOL Open(
        const DashboardInspectionRow& rec,
        const CString& yoloPath,
        const CString& opencvPath,
        const CString& yoloError,
        const CString& opencvError)
    {
        m_record = rec;
        m_yoloPath = yoloPath;
        m_opencvPath = opencvPath;
        m_yoloError = yoloError;
        m_opencvError = opencvError;

        CWnd* owner = AfxGetMainWnd();

        CRect parentRect;

        if (owner && ::IsWindow(owner->GetSafeHwnd()))
            owner->GetWindowRect(&parentRect);
        else
            parentRect.SetRect(
                0, 0,
                GetSystemMetrics(SM_CXSCREEN),
                GetSystemMetrics(SM_CYSCREEN));

        int screenW = GetSystemMetrics(SM_CXSCREEN);
        int screenH = GetSystemMetrics(SM_CYSCREEN);

        int width = 1100;
        int height = 650;

        if (width > screenW - 40)
            width = screenW - 40;

        if (height > screenH - 80)
            height = screenH - 80;

        CRect rect(
            parentRect.CenterPoint().x - width / 2,
            parentRect.CenterPoint().y - height / 2,
            parentRect.CenterPoint().x + width / 2,
            parentRect.CenterPoint().y + height / 2);

        if (!Create(
            nullptr,
            _T("NG 검사 이미지 상세"),
            WS_OVERLAPPEDWINDOW,
            rect,
            nullptr))
            return FALSE;

        ShowWindow(SW_SHOW);
        UpdateWindow();

        return TRUE;
    }

protected:
    DashboardInspectionRow m_record;

    CString m_yoloPath;
    CString m_opencvPath;
    CString m_yoloError;
    CString m_opencvError;

    CStatic m_info;
    CNgImagePreview m_yolo;
    CNgImagePreview m_opencv;

    afx_msg int OnCreate(LPCREATESTRUCT cs)
    {
        if (CFrameWnd::OnCreate(cs) == -1)
            return -1;

        CString info;

        info.Format(
            _T("ID: %s    |    결과: %s / %s    |    검사 시간: %s"),
            m_record.tireId.GetString(),
            m_record.verdict.GetString(),
            m_record.defectType.GetString(),
            m_record.time.GetString());

        if (!m_info.Create(
            info,
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            CRect(24, 19, 1030, 49),
            this))
            return -1;

        if (!m_yolo.CreatePreview(
            this,
            CRect(24, 65, 530, 570),
            52001))
            return -1;

        if (!m_opencv.CreatePreview(
            this,
            CRect(548, 65, 1054, 570),
            52002))
            return -1;

        if (!m_yoloPath.IsEmpty())
        {
            m_yolo.ShowImage(
                m_yoloPath,
                _T("YOLO | 상단 카메라"));
        }
        else
        {
            m_yolo.ShowMessage(
                _T("YOLO | ") + m_yoloError);
        }

        if (!m_opencvPath.IsEmpty())
        {
            m_opencv.ShowImage(
                m_opencvPath,
                _T("OpenCV | 측면 카메라"));
        }
        else
        {
            m_opencv.ShowMessage(
                _T("OpenCV | ") + m_opencvError);
        }

        return 0;
    }

    afx_msg void OnSize(
        UINT type,
        int cx,
        int cy)
    {
        CFrameWnd::OnSize(type, cx, cy);

        if (!::IsWindow(m_yolo.GetSafeHwnd()) ||
            !::IsWindow(m_opencv.GetSafeHwnd()))
            return;

        const int margin = 24;
        const int gap = 16;
        const int top = 64;

        int usableWidth = cx - margin * 2 - gap;
        int panelWidth = usableWidth / 2;
        int panelHeight = cy - top - margin;

        if (panelWidth <= 0 || panelHeight <= 0)
            return;

        m_yolo.MoveWindow(
            margin,
            top,
            panelWidth,
            panelHeight);

        m_opencv.MoveWindow(
            margin + panelWidth + gap,
            top,
            panelWidth,
            panelHeight);

        if (::IsWindow(m_info.GetSafeHwnd()))
        {
            m_info.MoveWindow(
                margin,
                18,
                cx - margin * 2,
                32);
        }
    }

    virtual void PostNcDestroy() override;

    afx_msg void OnClose()
    {
        DestroyWindow();
    }

    DECLARE_MESSAGE_MAP()
};

namespace
{
    CNgImagePopup* g_imagePopup = nullptr;
}

void CNgImagePopup::PostNcDestroy()
{
    if (g_imagePopup == this)
        g_imagePopup = nullptr;

    CFrameWnd::PostNcDestroy();
    delete this;
}

BEGIN_MESSAGE_MAP(CNgImagePopup, CFrameWnd)
    ON_WM_CREATE()
    ON_WM_SIZE()
    ON_WM_CLOSE()
END_MESSAGE_MAP()
// ============================================================
// Raspberry Pi 이미지 직접 다운로드
// ============================================================

bool CInspectionHistoryPage::DownloadPiImage(
    const CString& tireId,
    const CString& camera,
    CString& localPath,
    CString& error)
{
    localPath.Empty();
    error.Empty();

    if (!IsSafeTireId(tireId) ||
        (camera != _T("yolo") &&
            camera != _T("opencv")))
    {
        error = _T("잘못된 이미지 요청");
        return false;
    }

    CString token = GetPiToken();

    if (token.IsEmpty())
    {
        error = _T("PI_CONTROL_TOKEN 미설정");
        return false;
    }

    CString endpoint;

    endpoint.Format(
        _T("/api/ng-image/%s/%s"),
        tireId.GetString(),
        camera.GetString());

    HINTERNET session = nullptr;
    HINTERNET connection = nullptr;
    HINTERNET request = nullptr;

    std::vector<unsigned char> bytes;
    DWORD status = 0;
    bool success = false;

    do
    {
        session = WinHttpOpen(
            L"TireProductionMFC/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);

        if (!session)
        {
            error = _T("HTTP 초기화 실패");
            break;
        }

        WinHttpSetTimeouts(
            session, 2000, 3000, 3000, 4000);

        connection = WinHttpConnect(
            session,
            PI_HOST,
            PI_PORT,
            0);

        if (!connection)
        {
            error = _T("Raspberry Pi 연결 실패");
            break;
        }

        request = WinHttpOpenRequest(
            connection,
            L"GET",
            endpoint.GetString(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            0);

        if (!request)
        {
            error = _T("이미지 요청 생성 실패");
            break;
        }

        CString header;

        header.Format(
            _T("X-Line-Token: %s\r\n"),
            token.GetString());

        if (!WinHttpAddRequestHeaders(
            request,
            header.GetString(),
            (DWORD)-1,
            WINHTTP_ADDREQ_FLAG_ADD))
        {
            error = _T("인증 헤더 설정 실패");
            break;
        }

        if (!WinHttpSendRequest(
            request,
            WINHTTP_NO_ADDITIONAL_HEADERS,
            0,
            WINHTTP_NO_REQUEST_DATA,
            0, 0, 0))
        {
            error = _T("요청 전송 실패");
            break;
        }

        if (!WinHttpReceiveResponse(
            request,
            nullptr))
        {
            error = _T("Pi 서버 응답 실패");
            break;
        }

        DWORD length = sizeof(status);

        if (!WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_STATUS_CODE |
            WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status,
            &length,
            WINHTTP_NO_HEADER_INDEX))
        {
            error = _T("HTTP 상태 확인 실패");
            break;
        }

        if (status == 404)
        {
            error = _T("저장된 사진 없음");
            break;
        }

        if (status == 403)
        {
            error = _T("이미지 인증 실패");
            break;
        }

        if (status != 200)
        {
            error.Format(
                _T("HTTP 오류: %lu"),
                status);
            break;
        }

        constexpr size_t MAX_IMAGE =
            12 * 1024 * 1024;

        while (true)
        {
            unsigned char buffer[8192];
            DWORD received = 0;

            if (!WinHttpReadData(
                request,
                buffer,
                sizeof(buffer),
                &received))
            {
                error = _T("이미지 수신 실패");
                break;
            }

            if (received == 0)
            {
                success = true;
                break;
            }

            if (bytes.size() + received > MAX_IMAGE)
            {
                error = _T("이미지 크기 제한 초과");
                break;
            }

            bytes.insert(
                bytes.end(),
                buffer,
                buffer + received);
        }

    } while (false);

    if (request)
        WinHttpCloseHandle(request);

    if (connection)
        WinHttpCloseHandle(connection);

    if (session)
        WinHttpCloseHandle(session);

    if (!success)
        return false;

    if (bytes.size() < 4 ||
        bytes[0] != 0xFF ||
        bytes[1] != 0xD8)
    {
        error = _T("유효하지 않은 JPEG 이미지");
        return false;
    }

    wchar_t tempDir[MAX_PATH] = {};

    if (!GetTempPathW(MAX_PATH, tempDir))
    {
        error = _T("임시 폴더 접근 실패");
        return false;
    }

    localPath.Format(
        _T("%s%s_%s.jpg"),
        tempDir,
        tireId.GetString(),
        camera.GetString());

    try
    {
        CFile file(
            localPath,
            CFile::modeCreate |
            CFile::modeWrite |
            CFile::typeBinary);

        file.Write(
            bytes.data(),
            static_cast<UINT>(bytes.size()));

        file.Close();
    }
    catch (CFileException* ex)
    {
        ex->Delete();

        localPath.Empty();
        error = _T("이미지 임시 저장 실패");
        return false;
    }

    return true;
}

// ============================================================
// 검사 이력 화면
// ============================================================

BEGIN_MESSAGE_MAP(CInspectionHistoryPage, CBasePage)

    ON_BN_CLICKED(
        IDC_HISTORY_SEARCH,
        &CInspectionHistoryPage::OnSearch)

    ON_NOTIFY(
        LVN_ITEMCHANGED,
        IDC_HISTORY_LIST,
        &CInspectionHistoryPage::OnRowSelected)

END_MESSAGE_MAP()

BOOL CInspectionHistoryPage::CreatePage(
    CWnd* p,
    const CRect& r,
    UINT id)
{
    if (!CBasePage::CreatePage(p, r, id))
        return FALSE;

    AddTitle(_T("검사 이력 / NG 이미지"));

    AddLabel(_T("날짜"), 28, 83, 48);
    m_dateFilter = AddEdit(82, 78, 180, 32);

    AddLabel(_T("결과"), 286, 83, 48);
    m_resultFilter = AddEdit(340, 78, 130, 32);

    AddButton(
        _T("조회"),
        492, 76, 92, 36,
        IDC_HISTORY_SEARCH);

    AddLabel(
        _T("검사 이력"),
        28, 132, 120, 24);

    m_list = AddList(
        28, 162, 600, 450);

    m_list->SetDlgCtrlID(IDC_HISTORY_LIST);

    m_list->InsertColumn(
        0, _T("시간"), LVCFMT_LEFT, 145);

    m_list->InsertColumn(
        1, _T("ID"), LVCFMT_LEFT, 180);

    m_list->InsertColumn(
        2, _T("결과"), LVCFMT_LEFT, 75);

    m_list->InsertColumn(
        3, _T("불량 유형"), LVCFMT_LEFT, 190);

    m_imageCard = AddBox(
        _T("NG IMAGE\r\n")
        _T("NG 기록 선택 시 확대 창 표시"),
        650, 162, 300, 278);

    m_detailCard = AddBox(
        _T("선택 검사 상세 정보\r\n")
        _T("ID  --\r\n")
        _T("검사 결과  --\r\n")
        _T("검사 시간  --"),
        650, 460, 300, 152);

    if (!m_yoloPreview.CreatePreview(
        this,
        CRect(662, 190, 938, 309),
        IDC_YOLO_PREVIEW))
        return FALSE;

    if (!m_opencvPreview.CreatePreview(
        this,
        CRect(662, 312, 938, 431),
        IDC_OPENCV_PREVIEW))
        return FALSE;

    m_yoloPreview.ShowMessage(
        _T("YOLO - 검사 선택 대기"));

    m_opencvPreview.ShowMessage(
        _T("OpenCV - 검사 선택 대기"));

    return TRUE;
}

void CInspectionHistoryPage::SetInspectionRows(
    const std::vector<DashboardInspectionRow>& rows)
{
    m_allRows = rows;

    if (m_list &&
        ::IsWindow(m_list->GetSafeHwnd()))
    {
        PopulateList();
    }
}

void CInspectionHistoryPage::PopulateList()
{
    if (!m_list)
        return;

    CString date, result;

    if (m_dateFilter)
        m_dateFilter->GetWindowText(date);

    if (m_resultFilter)
        m_resultFilter->GetWindowText(result);

    date.Trim();
    result.Trim();
    result.MakeUpper();

    m_list->SetRedraw(FALSE);
    m_list->DeleteAllItems();
    m_visibleIndices.clear();

    for (size_t i = 0; i < m_allRows.size(); ++i)
    {
        const auto& rec = m_allRows[i];

        CString verdict = rec.verdict;
        verdict.MakeUpper();

        if (verdict == _T("PASS") ||
            verdict == _T("NORMAL"))
            verdict = _T("OK");

        if (verdict == _T("FAIL") ||
            verdict == _T("DEFECT"))
            verdict = _T("NG");

        if (!date.IsEmpty() &&
            rec.time.Find(date) < 0)
            continue;

        if (!result.IsEmpty() &&
            verdict.Find(result) < 0)
            continue;

        int row = m_list->InsertItem(
            m_list->GetItemCount(),
            rec.time);

        m_list->SetItemText(
            row, 1, rec.tireId);

        m_list->SetItemText(
            row, 2, verdict);

        m_list->SetItemText(
            row, 3, rec.defectType);

        m_visibleIndices.push_back(i);
    }

    m_list->SetRedraw(TRUE);
    m_list->Invalidate(FALSE);

    if (m_detailCard)
    {
        m_detailCard->SetText(
            _T("선택 검사 상세 정보\r\n")
            _T("목록에서 검사를 선택하세요."));
    }

    m_yoloPreview.ShowMessage(
        _T("YOLO - 검사 선택 대기"));

    m_opencvPreview.ShowMessage(
        _T("OpenCV - 검사 선택 대기"));
}

void CInspectionHistoryPage::LoadNgImages(
    const DashboardInspectionRow& rec)
{
    CString verdict = rec.verdict;
    verdict.MakeUpper();

    if (verdict != _T("NG") &&
        verdict != _T("FAIL") &&
        verdict != _T("DEFECT"))
    {
        m_yoloPreview.ShowMessage(
            _T("YOLO - 정상 검사"));

        m_opencvPreview.ShowMessage(
            _T("OpenCV - 정상 검사"));

        return;
    }

    CString yoloPath;
    CString opencvPath;
    CString yoloError;
    CString opencvError;

    bool yoloOk = DownloadPiImage(
        rec.tireId,
        _T("yolo"),
        yoloPath,
        yoloError);

    bool opencvOk = DownloadPiImage(
        rec.tireId,
        _T("opencv"),
        opencvPath,
        opencvError);

    // YOLO 이미지 표시
    if (yoloOk)
    {
        m_yoloPreview.ShowImage(
            yoloPath,
            _T("YOLO - 상단 카메라"));
    }
    else
    {
        m_yoloPreview.ShowMessage(
            _T("YOLO - ") + yoloError);
    }

    // OpenCV 이미지 표시
    if (opencvOk)
    {
        m_opencvPreview.ShowImage(
            opencvPath,
            _T("OpenCV - 측면 카메라"));
    }
    else
    {
        m_opencvPreview.ShowMessage(
            _T("OpenCV - ") + opencvError);
    }
}

void CInspectionHistoryPage::OnSearch()
{
    PopulateList();

    auto* frame =
        dynamic_cast<CMainFrame*>(GetParent());

    if (frame)
        frame->RequestDashboardRefresh();
}

void CInspectionHistoryPage::OnRowSelected(
    NMHDR* header,
    LRESULT* result)
{
    if (result)
        *result = 0;

    auto* change =
        reinterpret_cast<NMLISTVIEW*>(header);

    if (!change ||
        !(change->uChanged & LVIF_STATE) ||
        !(change->uNewState & LVIS_SELECTED))
        return;

    int selected = change->iItem;

    if (selected < 0 ||
        static_cast<size_t>(selected) >=
        m_visibleIndices.size())
        return;

    const auto& rec =
        m_allRows[m_visibleIndices[selected]];

    CString detail;

    detail.Format(
        _T("선택 검사 상세 정보\r\n")
        _T("ID  %s\r\n")
        _T("검사 결과  %s / %s\r\n")
        _T("검사 시간  %s"),
        rec.tireId.GetString(),
        rec.verdict.GetString(),
        rec.defectType.GetString(),
        rec.time.GetString());

    if (!rec.detail.IsEmpty())
        detail += _T("\r\n") + rec.detail;

    if (m_detailCard)
        m_detailCard->SetText(detail);

    LoadNgImages(rec);
}
