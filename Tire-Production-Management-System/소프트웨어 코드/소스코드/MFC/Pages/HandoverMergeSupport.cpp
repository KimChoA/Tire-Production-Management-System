#include "pch.h"
#include "HandoverMergeSupport.h"
#include <cstdlib>

namespace
{
    std::string ToUtf8(const CString& text)
    {
        if (text.IsEmpty()) return {};
        const int size = WideCharToMultiByte(
            CP_UTF8, 0, text, text.GetLength(), nullptr, 0, nullptr, nullptr);
        std::string result(size, '\0');
        WideCharToMultiByte(
            CP_UTF8, 0, text, text.GetLength(), result.data(), size, nullptr, nullptr);
        return result;
    }

    CString FromUtf8(const std::string& text)
    {
        if (text.empty()) return _T("");
        const int size = MultiByteToWideChar(
            CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        CString result;
        wchar_t* buffer = result.GetBuffer(size);
        MultiByteToWideChar(
            CP_UTF8, 0, text.data(), static_cast<int>(text.size()), buffer, size);
        result.ReleaseBuffer(size);
        return result;
    }

    std::string Escape(const std::string& text)
    {
        std::string result;
        for (unsigned char ch : text)
        {
            switch (ch)
            {
            case '\\': result += "\\\\"; break;
            case '"': result += "\\\""; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result.push_back(static_cast<char>(ch)); break;
            }
        }
        return result;
    }

    int HexValue(char ch)
    {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    }

    void AppendUtf8(std::string& output, unsigned int codePoint)
    {
        if (codePoint <= 0x7F) output.push_back(static_cast<char>(codePoint));
        else if (codePoint <= 0x7FF)
        {
            output.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
        else
        {
            output.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
    }

    bool ExtractString(const std::string& json, const std::string& key, std::string& value)
    {
        const std::string token = "\"" + key + "\"";
        size_t pos = json.find(token);
        if (pos == std::string::npos) return false;
        pos = json.find(':', pos + token.size());
        if (pos == std::string::npos) return false;
        pos = json.find('"', pos + 1);
        if (pos == std::string::npos) return false;
        ++pos;
        std::string output;
        while (pos < json.size())
        {
            const char ch = json[pos++];
            if (ch == '"') { value = output; return true; }
            if (ch != '\\') { output.push_back(ch); continue; }
            if (pos >= json.size()) return false;
            const char escaped = json[pos++];
            if (escaped == 'n') output.push_back('\n');
            else if (escaped == 'r') output.push_back('\r');
            else if (escaped == 't') output.push_back('\t');
            else if (escaped == 'u' && pos + 4 <= json.size())
            {
                unsigned int code = 0;
                for (int index = 0; index < 4; ++index)
                {
                    const int hex = HexValue(json[pos + index]);
                    if (hex < 0) return false;
                    code = (code << 4) | static_cast<unsigned int>(hex);
                }
                pos += 4;
                AppendUtf8(output, code);
            }
            else output.push_back(escaped);
        }
        return false;
    }

    bool ExtractInt64(const std::string& json, const std::string& key, long long& value)
    {
        const std::string token = "\"" + key + "\"";
        size_t pos = json.find(token);
        if (pos == std::string::npos) return false;
        pos = json.find(':', pos + token.size());
        if (pos == std::string::npos) return false;
        ++pos;
        while (pos < json.size() && isspace(static_cast<unsigned char>(json[pos]))) ++pos;
        char* end = nullptr;
        value = std::strtoll(json.c_str() + pos, &end, 10);
        return end != json.c_str() + pos;
    }

    bool ExtractBool(const std::string& json, const std::string& key, bool& value)
    {
        const std::string token = "\"" + key + "\"";
        size_t pos = json.find(token);
        if (pos == std::string::npos) return false;
        pos = json.find(':', pos + token.size());
        if (pos == std::string::npos) return false;
        ++pos;
        while (pos < json.size() && isspace(static_cast<unsigned char>(json[pos]))) ++pos;
        if (json.compare(pos, 4, "true") == 0 || json.compare(pos, 1, "1") == 0)
        { value = true; return true; }
        if (json.compare(pos, 5, "false") == 0 || json.compare(pos, 1, "0") == 0)
        { value = false; return true; }
        return false;
    }
}

std::string HandoverMerge::BuildRegisterPacket(const HandoverFormData& data)
{
    return "{\"type\":\"HANDOVER\",\"work_date\":\"" + Escape(ToUtf8(data.workDate)) +
        "\",\"shift_type\":\"" + Escape(ToUtf8(data.shiftType)) +
        "\",\"priority\":\"" + Escape(ToUtf8(data.priority)) +
        "\",\"user_name\":\"" + Escape(ToUtf8(data.userName)) +
        "\",\"message\":\"" + Escape(ToUtf8(data.message)) + "\"}";
}

std::string HandoverMerge::BuildListRequestPacket()
{
    return "{\"type\":\"HANDOVER_LIST\"}";
}

std::string HandoverMerge::BuildConfirmPacket(long long id, const CString& checkedBy)
{
    return "{\"type\":\"HANDOVER_CHECK\",\"id\":" + std::to_string(id) +
        ",\"checked_by\":\"" + Escape(ToUtf8(checkedBy)) + "\"}";
}

bool HandoverMerge::GetPacketType(const std::string& json, std::string& type)
{
    return ExtractString(json, "type", type);
}

bool HandoverMerge::ParseItem(const std::string& json, HandoverItem& item)
{
    std::string type;
    if (!GetPacketType(json, type) || type != "HANDOVER_ITEM") return false;
    std::string value;
    if (!ExtractInt64(json, "id", item.id) || item.id <= 0) return false;
    if (ExtractString(json, "work_date", value)) item.workDate = FromUtf8(value);
    if (ExtractString(json, "shift_type", value)) item.shiftType = FromUtf8(value);
    if (ExtractString(json, "priority", value)) item.priority = FromUtf8(value);
    if (ExtractString(json, "user_name", value)) item.userName = FromUtf8(value);
    if (ExtractString(json, "message", value)) item.message = FromUtf8(value);
    if (ExtractString(json, "checked_by", value)) item.checkedBy = FromUtf8(value);
    if (ExtractString(json, "checked_at", value)) item.checkedAt = FromUtf8(value);
    if (ExtractString(json, "created_at", value)) item.createdAt = FromUtf8(value);
    ExtractBool(json, "checked", item.checked);
    return true;
}

CString HandoverMerge::BuildListText(const HandoverItem& item)
{
    CString date = item.workDate;
    if (date.GetLength() >= 10) date = date.Mid(5, 5);
    CString result;
    result.Format(
        _T("[%s] %s %s 근무 인수인계"),
        HandoverPriorityName(item.priority).GetString(),
        date.GetString(),
        HandoverShiftName(item.shiftType).GetString());
    return result;
}

CString HandoverMerge::BuildDetailText(const HandoverItem& item)
{
    CString result;
    result.Format(
        _T("%s %s 근무 인수인계\r\n중요도: %s\r\n작성자: %s\r\n작성 시각: %s\r\n상태: %s\r\n\r\n%s"),
        item.workDate.GetString(),
        HandoverShiftName(item.shiftType).GetString(),
        HandoverPriorityName(item.priority).GetString(),
        item.userName.GetString(),
        item.createdAt.GetString(),
        item.checked ? _T("확인완료") : _T("미확인"),
        item.message.GetString());
    if (item.checked)
    {
        result.AppendFormat(
            _T("\r\n\r\n확인자: %s\r\n확인 시각: %s"),
            item.checkedBy.GetString(), item.checkedAt.GetString());
    }
    return result;
}
