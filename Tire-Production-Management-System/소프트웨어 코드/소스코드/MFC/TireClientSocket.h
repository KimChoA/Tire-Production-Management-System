#pragma once
#include <afxwin.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

class CChatHandoverPage;

#define WM_CHAT_SOCKET_PACKET (WM_APP + 301)
#define WM_CHAT_SOCKET_STATE  (WM_APP + 302)

class CTireClientSocket
{
public:
    explicit CTireClientSocket(CChatHandoverPage* pPage);
    ~CTireClientSocket();

    BOOL ConnectToServer(const CString& serverIp, UINT port);
    BOOL SendChat(const CString& userName, const CString& role, const CString& message);
    BOOL SendRaw(const std::string& json);
    void Close();

    bool IsConnected() const { return m_connected.load(); }

private:
    CChatHandoverPage* m_pPage = nullptr;
    SOCKET m_socket = INVALID_SOCKET;
    std::thread m_receiveThread;
    std::mutex m_sendMutex;
    std::atomic<bool> m_connected{ false };
    std::atomic<bool> m_stopRequested{ false };

    void ConnectionWorker(CString serverIp, UINT port);
    void ReceiveLoop();
    void PostConnectionState(bool connected, const CString& text);
    void PostPacket(const std::string& packet);
    BOOL SendPacket(const std::string& packet);

    static std::string ToUtf8(const CString& text);
    static std::string EscapeJson(const std::string& text);
};
