#pragma once

#include "HandoverTypes.h"
#include <string>

namespace HandoverMerge
{
    std::string BuildRegisterPacket(const HandoverFormData& data);
    std::string BuildListRequestPacket();
    std::string BuildConfirmPacket(long long id, const CString& checkedBy);

    bool GetPacketType(const std::string& json, std::string& type);
    bool ParseItem(const std::string& json, HandoverItem& item);

    CString BuildListText(const HandoverItem& item);
    CString BuildDetailText(const HandoverItem& item);
}
