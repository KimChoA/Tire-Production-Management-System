#pragma once
#include "../BasePage.h"
#include "../TireClientSocket.h"
#include "HandoverRegisterDialog.h"
#include "HandoverMergeSupport.h"
#include <string>
#include <vector>

// WinForms 채팅 화면과 같은 좌/우 말풍선 UI (MFC 전용)
class CChatBubbleView : public CWnd
{
public:
    struct Message
    {
        CString time, sender, body;
        bool mine = false;
    };
    BOOL CreateView(CWnd* parent, const CRect& rect, UINT id);
    void AddMessage(const CString& time, const CString& sender, const CString& body, bool mine);
    void ClearMessages();
protected:
    std::vector<Message> m_messages;
    int m_scroll = 0;
    int m_contentHeight = 0;
    CFont m_senderFont;
    CFont m_bodyFont;
    void UpdateScroll();
    afx_msg void OnPaint();
    afx_msg void OnSize(UINT type, int cx, int cy);
    afx_msg void OnVScroll(UINT code, UINT pos, CScrollBar* bar);
    afx_msg BOOL OnMouseWheel(UINT flags, short delta, CPoint point);
    DECLARE_MESSAGE_MAP()
};

class CChatHandoverPage : public CBasePage
{
public:
    CChatHandoverPage();
    virtual ~CChatHandoverPage();

    BOOL CreatePage(CWnd* p, const CRect& r, UINT id);
    void ProcessReceivedPacket(const std::string& jsonString);
    void SetConnectionState(bool connected, const CString& text);
    void RequestDashboardStats();
    bool SubmitDemoInspection(const CString& verdict, const CString& defectType);
    virtual BOOL PreTranslateMessage(MSG* pMsg) override;

    const std::vector<HandoverItem>& GetHandovers() const { return m_handovers; }

protected:
    enum
    {
        IDC_BTN_CHAT_SEND = 10101,
        IDC_BTN_CHAT_RECONNECT = 10102,
        IDC_BTN_HANDOVER_REGISTER = 10103,
        IDC_BTN_HANDOVER_CONFIRM = 10104,
        IDC_LIST_HANDOVER = 10105
    };

    CChatBubbleView* m_pChatView = nullptr;
    CEdit* m_pChatEdit = nullptr;
    CListCtrl* m_pHandoverList = nullptr;
    CModernCard* m_pHandoverDetail = nullptr;
    CStatic* m_pConnectionLabel = nullptr;
    CTireClientSocket* m_pSocket = nullptr;
    int m_loadedHistoryCount = 0;
    std::vector<HandoverItem> m_handovers;
    long long m_selectedHandoverId = 0;

    void SendCurrentMessage();
    void ReloadHandovers();
    void UpsertHandover(const HandoverItem& item);
    const HandoverItem* FindHandover(long long id) const;

    void AddChatMessage(
        const CString& time,
        const CString& role,
        const CString& user,
        const CString& message);

    afx_msg void OnBnClickedSend();
    afx_msg void OnBnClickedReconnect();
    afx_msg void OnBnClickedHandoverRegister();
    afx_msg void OnBnClickedHandoverConfirm();
    afx_msg void OnHandoverItemChanged(NMHDR* header, LRESULT* result);
    afx_msg LRESULT OnSocketPacket(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnSocketState(WPARAM wParam, LPARAM lParam);

    DECLARE_MESSAGE_MAP()
};
