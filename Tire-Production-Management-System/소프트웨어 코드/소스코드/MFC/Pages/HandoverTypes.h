#pragma once

#include <afxstr.h>

struct HandoverItem
{
    long long id = 0;
    CString workDate;
    CString shiftType;
    CString priority;
    CString userName;
    CString message;
    bool checked = false;
    CString checkedBy;
    CString checkedAt;
    CString createdAt;
};

struct HandoverFormData
{
    CString workDate;
    CString shiftType;
    CString priority;
    CString userName;
    CString message;
};

inline CString HandoverShiftName(const CString& value)
{
    return value == _T("PM") ? _T("오후") : _T("오전");
}

inline CString HandoverPriorityName(const CString& value)
{
    if (value == _T("URGENT")) return _T("긴급");
    if (value == _T("WARNING")) return _T("주의");
    return _T("일반");
}
