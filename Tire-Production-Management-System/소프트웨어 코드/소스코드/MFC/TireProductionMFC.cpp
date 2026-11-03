#include "pch.h"
#include "TireProductionMFC.h"
#include "MainFrame.h"
#include <shellapi.h>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

CString g_userName = _T("");
CString g_userRole = _T("");

CTireProductionApp theApp;

BOOL CTireProductionApp::InitInstance()
{
    CWinApp::InitInstance();
    AfxEnableControlContainer();

    if (!AfxSocketInit())
    {
        AfxMessageBox(_T("Windows 소켓 초기화에 실패했습니다."));
        return FALSE;
    }

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    if (argv != nullptr && argc >= 3)
    {
        g_userName = argv[1];
        g_userRole = argv[2];
        LocalFree(argv);
    }
    else
    {
        if (argv != nullptr)
            LocalFree(argv);

        AfxMessageBox(_T("관리자 로그인 화면을 통해 실행해주세요."));
        return FALSE;
    }

    if (g_userName.IsEmpty() || g_userRole.CompareNoCase(_T("ADMIN")) != 0)
    {
        AfxMessageBox(_T("관리자 권한으로 로그인한 경우에만 실행할 수 있습니다."));
        return FALSE;
    }

    auto* frame = new CMainFrame();

    if (!frame->CreateMainFrame())
    {
        delete frame;
        return FALSE;
    }

    m_pMainWnd = frame;
    frame->ShowWindow(SW_SHOW);
    frame->UpdateWindow();

    return TRUE;
}
