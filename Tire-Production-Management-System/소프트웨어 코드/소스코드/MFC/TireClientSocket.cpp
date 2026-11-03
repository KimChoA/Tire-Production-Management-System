#include "pch.h"
#include "TireClientSocket.h"
#include "Pages/ChatHandoverPage.h"
#include <ws2tcpip.h>

#pragma comment(lib, "Ws2_32.lib")

CTireClientSocket::CTireClientSocket(CChatHandoverPage* pPage)
    : m_pPage(pPage)
{
}

CTireClientSocket::~CTireClientSocket()
{
    Close();
}

BOOL CTireClientSocket::ConnectToServer(const CString& serverIp, UINT port)
{
    Close();

    m_stopRequested = false;
    m_connected = false;

    PostConnectionState(false, _T("● 중앙 서버 연결 중..."));

    try
    {
        m_receiveThread = std::thread(
            &CTireClientSocket::ConnectionWorker,
            this,
            serverIp,
            port);
    }
    catch (...)
    {
        PostConnectionState(false, _T("● 연결 스레드 생성 실패"));
        return FALSE;
    }

    return TRUE;
}

void CTireClientSocket::ConnectionWorker(CString serverIp, UINT port)
{
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET)
    {
        PostConnectionState(false, _T("● 소켓 생성 실패"));
        return;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(port));

    CStringA ipAnsi(serverIp);
    if (inet_pton(AF_INET, ipAnsi.GetString(), &address.sin_addr) != 1)
    {
        closesocket(sock);
        PostConnectionState(false, _T("● 서버 주소 오류"));
        return;
    }

    if (connect(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
    {
        const int error = WSAGetLastError();
        closesocket(sock);

        CString text;
        text.Format(_T("● 중앙 서버 연결 실패 (%d)"), error);
        PostConnectionState(false, text);
        return;
    }

    if (m_stopRequested.load())
    {
        shutdown(sock, SD_BOTH);
        closesocket(sock);
        return;
    }

    m_socket = sock;
    m_connected = true;

    CString connectedText;
    connectedText.Format(_T("● 서버 연결됨  %s:%u"), serverIp.GetString(), port);
    PostConnectionState(true, connectedText);

    ReceiveLoop();
}

void CTireClientSocket::ReceiveLoop()
{
    std::string recvBuffer;
    char buffer[4096];

    while (!m_stopRequested.load())
    {
        const int received = recv(m_socket, buffer, sizeof(buffer), 0);

        if (received == 0)
            break;

        if (received == SOCKET_ERROR)
        {
            const int error = WSAGetLastError();
            if (m_stopRequested.load())
                break;

            CString text;
            text.Format(_T("● 서버 수신 오류 (%d)"), error);
            PostConnectionState(false, text);
            break;
        }

        recvBuffer.append(buffer, received);

        size_t pos = 0;
        while ((pos = recvBuffer.find('\n')) != std::string::npos)
        {
            std::string line = recvBuffer.substr(0, pos);
            recvBuffer.erase(0, pos + 1);

            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            if (!line.empty())
                PostPacket(line);
        }
    }

    m_connected = false;

    SOCKET oldSocket = m_socket;
    m_socket = INVALID_SOCKET;

    if (oldSocket != INVALID_SOCKET)
        closesocket(oldSocket);

    if (!m_stopRequested.load())
        PostConnectionState(false, _T("● 중앙 서버 연결 끊김"));
}

BOOL CTireClientSocket::SendChat(
    const CString& userName,
    const CString& role,
    const CString& message)
{
    const std::string userUtf8 = EscapeJson(ToUtf8(userName));
    const std::string roleUtf8 = EscapeJson(ToUtf8(role));
    const std::string messageUtf8 = EscapeJson(ToUtf8(message));

    const std::string packet =
        "{\"type\":\"CHAT\",\"user_name\":\"" + userUtf8 +
        "\",\"role\":\"" + roleUtf8 +
        "\",\"message\":\"" + messageUtf8 + "\"}";

    return SendRaw(packet);
}

BOOL CTireClientSocket::SendRaw(const std::string& json)
{
    if (json.empty())
        return FALSE;

    std::string packet = json;

    if (packet.back() != '\n')
        packet.push_back('\n');

    return SendPacket(packet);
}

BOOL CTireClientSocket::SendPacket(const std::string& packet)
{
    if (!m_connected.load() ||
        m_socket == INVALID_SOCKET)
    {
        return FALSE;
    }

    std::lock_guard<std::mutex> lock(m_sendMutex);

    int totalSent = 0;

    while (totalSent <
        static_cast<int>(packet.size()))
    {
        const int sent =
            send(
                m_socket,
                packet.data() + totalSent,
                static_cast<int>(packet.size()) - totalSent,
                0);

        if (sent == SOCKET_ERROR ||
            sent == 0)
        {
            m_connected = false;

            PostConnectionState(
                false,
                _T("● 메시지 전송 실패"));

            return FALSE;
        }

        totalSent += sent;
    }

    return TRUE;
}

void CTireClientSocket::Close()
{
    m_stopRequested = true;
    m_connected = false;

    SOCKET sock = m_socket;
    m_socket = INVALID_SOCKET;

    if (sock != INVALID_SOCKET)
    {
        shutdown(sock, SD_BOTH);
        closesocket(sock);
    }

    if (m_receiveThread.joinable())
    {
        if (m_receiveThread.get_id() == std::this_thread::get_id())
            m_receiveThread.detach();
        else
            m_receiveThread.join();
    }
}

void CTireClientSocket::PostConnectionState(bool connected, const CString& text)
{
    if (!m_pPage)
        return;

    HWND hwnd = m_pPage->GetSafeHwnd();
    if (!::IsWindow(hwnd))
        return;

    CString* payload = new CString(text);
    if (!::PostMessage(hwnd, WM_CHAT_SOCKET_STATE, connected ? 1 : 0,
        reinterpret_cast<LPARAM>(payload)))
    {
        delete payload;
    }
}

void CTireClientSocket::PostPacket(const std::string& packet)
{
    if (!m_pPage)
        return;

    HWND hwnd = m_pPage->GetSafeHwnd();
    if (!::IsWindow(hwnd))
        return;

    std::string* payload = new std::string(packet);
    if (!::PostMessage(hwnd, WM_CHAT_SOCKET_PACKET, 0,
        reinterpret_cast<LPARAM>(payload)))
    {
        delete payload;
    }
}

std::string CTireClientSocket::ToUtf8(const CString& text)
{
    if (text.IsEmpty())
        return {};

    const int size = WideCharToMultiByte(
        CP_UTF8,
        0,
        text.GetString(),
        text.GetLength(),
        nullptr,
        0,
        nullptr,
        nullptr);

    std::string result(size, '\0');

    WideCharToMultiByte(
        CP_UTF8,
        0,
        text.GetString(),
        text.GetLength(),
        result.data(),
        size,
        nullptr,
        nullptr);

    return result;
}

std::string CTireClientSocket::EscapeJson(const std::string& text)
{
    std::string result;
    result.reserve(text.size() + 16);

    for (unsigned char ch : text)
    {
        switch (ch)
        {
        case '\\': result += "\\\\"; break;
        case '"':  result += "\\\""; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (ch < 0x20)
            {
                char temp[7];
                sprintf_s(temp, "\\u%04x", ch);
                result += temp;
            }
            else
            {
                result.push_back(static_cast<char>(ch));
            }
            break;
        }
    }

    return result;
}
