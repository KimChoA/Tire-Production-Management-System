#pragma once
#include "framework.h"
extern CString g_userName;
extern CString g_userRole;

class CTireProductionApp : public CWinApp
{
public:
    virtual BOOL InitInstance() override;
};
