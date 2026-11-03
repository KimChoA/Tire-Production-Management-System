using System.Net.Http.Json;
using System.Text.Json;
using TireEquipmentHMI.Pages;
using TireEquipmentHMI.Theme;

namespace TireEquipmentHMI;

public sealed class MainForm : Form
{
    private readonly Panel contentPanel = new();
    private readonly Label pageTitleLabel = new();
    private readonly Label systemStatusLabel = new();
    private readonly Label userInfoLabel = new();
    private readonly Label footerStatusLabel = new();
    private readonly List<Button> menuButtons = new();

    private readonly EquipmentControlPage equipmentPage = new();
    private readonly InspectionMonitoringPage inspectionPage = new();
    private readonly ChatPage chatPage;
    private readonly HttpClient piClient = new() { Timeout = TimeSpan.FromSeconds(3) };
    private readonly System.Windows.Forms.Timer piStateTimer = new() { Interval = 1200 };
    private string piUrl = "http://127.0.0.1:5001";
    private string piToken = "";
    private string lastAlertState = "";
    private bool piStateBusy;
    private int statsPollTicks;


    // 하트비트 감시용 변수 및 타이머
    private DateTime _lastPiHeartbeat = DateTime.MinValue;
    private readonly System.Windows.Forms.Timer _heartbeatTimer = new();
    private bool _isAlarmActive = false;
    private bool _hasReceivedFirstHeartbeat = false;

    private readonly string _userName;
    private readonly string _role;
    private readonly int _fingerprintId;

    public MainForm(
        string userName,
        string role,
        int fingerprintId)
    {
        _userName = userName;
        _role = role;
        _fingerprintId = fingerprintId;
        chatPage = new ChatPage(_userName, _role);
        chatPage.HeartbeatReceived += OnHeartbeatReceived;
        chatPage.RealInspectionHistoryReceived += rows =>
        {
            if (IsDisposed || !IsHandleCreated) return;
            BeginInvoke((Action)(() =>
            {
                if (!IsDisposed) inspectionPage.DisplayRealInspectionHistory(rows);
            }));
        };
        _heartbeatTimer.Interval = 1000;
        _heartbeatTimer.Tick += CheckHeartbeatTimeout;
        _heartbeatTimer.Start();

        Text = $"Tire Inspection Equipment HMI - {_userName}";
        StartPosition = FormStartPosition.CenterScreen;
        MinimumSize = new Size(1180, 760);
        Size = new Size(1280, 820);
        Font = new Font("Segoe UI", 10F);
        BackColor = UiTheme.PageBg;

        LoadPiSettings();
        equipmentPage.HardwareCommandAsync = SendHardwareCommandAsync;
        BuildLayout();
        piStateTimer.Tick += async (_, _) =>
        {
            await RefreshPiStateAsync();
            if (++statsPollTicks % 5 == 0)
                await chatPage.RequestRealInspectionHistoryAsync();
        };
        piStateTimer.Start();
        FormClosed += (_, _) => { piStateTimer.Stop(); piClient.Dispose(); };


        ShowPage(equipmentPage, "장비 제어", 0);
        Shown += (_, _) => chatPage.StartConnection();
    }

    private void LoadPiSettings()
    {
        try
        {
            var path = Path.Combine(AppContext.BaseDirectory, "camera_config.json");
            using var doc = JsonDocument.Parse(File.ReadAllText(path));
            if (doc.RootElement.TryGetProperty("CameraBaseUrl", out var url))
                piUrl = (url.GetString() ?? piUrl).TrimEnd('/');
            if (doc.RootElement.TryGetProperty("PiControlToken", out var token))
                piToken = token.GetString() ?? "";
        }
        catch { /* missing settings will be surfaced during control attempts */ }
    }

    private async Task<bool> SendHardwareCommandAsync(string action)
    {
        if (string.IsNullOrWhiteSpace(piToken))
        {
            MessageBox.Show("camera_config.json에 PiControlToken을 설정해주세요.", "제어 설정 누락");
            return false;
        }
        try
        {
            using var request = new HttpRequestMessage(HttpMethod.Post, $"{piUrl}/api/control")
            {
                Content = JsonContent.Create(new { action })
            };
            request.Headers.Add("X-Line-Token", piToken);
            using var response = await piClient.SendAsync(request);
            if (!response.IsSuccessStatusCode)
            {
                string error = await response.Content.ReadAsStringAsync();
                MessageBox.Show($"장비 명령 거부: {error}", "하드웨어 제어", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return false;
            }
            await RefreshPiStateAsync();
            return true;
        }
        catch (Exception ex)
        {
            MessageBox.Show($"Raspberry Pi 제어 실패: {ex.Message}", "연결 실패");
            return false;
        }
    }

    private async Task RefreshPiStateAsync()
    {
        if (piStateBusy || IsDisposed) return;
        piStateBusy = true;
        try
        {
            using var response = await piClient.GetAsync($"{piUrl}/api/state");
            response.EnsureSuccessStatusCode();
            using var doc = JsonDocument.Parse(await response.Content.ReadAsStringAsync());
            var state = doc.RootElement;
            equipmentPage.ApplyHardwareState(state);
            string status = state.GetProperty("state").GetString() ?? "";
            string lastResult = state.TryGetProperty("last_result", out var lr)
                ? lr.GetString() ?? "NONE" : "NONE";

            footerStatusLabel.Text =
                $"Raspberry Pi 연결됨  |  설비: {status}  |  최근 검사: {lastResult}";

            if (status is "RECHECK" or "ESTOP")
            {
                if (lastAlertState != status)
                {
                    lastAlertState = status;
                    string detail = state.GetProperty("message").GetString() ?? "";
                    MessageBox.Show(detail, status=="RECHECK" ? "검사 재확인 필요" : "비상정지 경고",
                        MessageBoxButtons.OK, MessageBoxIcon.Warning);
                }
            }
            else lastAlertState = "";
        }
        catch
        {
            equipmentPage.SetHardwareDisconnected();
        }
        finally { piStateBusy = false; }
    }

    private void BuildLayout()
    {
        var header = new Panel
        {
            Dock = DockStyle.Top,
            Height = 76,
            BackColor = UiTheme.Header
        };

        var appTitle = new Label
        {
            Text = "TIRE INSPECTION EQUIPMENT",
            ForeColor = Color.White,
            Font = new Font("Segoe UI", 18F, FontStyle.Bold),
            AutoSize = true,
            Location = new Point(26, 12)
        };

        var subtitle = new Label
        {
            Text = "Smart Factory · Equipment Control HMI",
            ForeColor = Color.FromArgb(203, 213, 225),
            Font = new Font("Segoe UI", 9F),
            AutoSize = true,
            Location = new Point(28, 47)
        };

        var rightHeader = new Panel
        {
            Dock = DockStyle.Right,
            Width = 360,
            BackColor = UiTheme.Header
        };

        userInfoLabel.Text =
            $"{_userName}  |  {GetRoleName(_role)}";

        userInfoLabel.ForeColor = Color.White;
        userInfoLabel.Font =
            new Font("맑은 고딕", 10F, FontStyle.Bold);

        userInfoLabel.TextAlign =
            ContentAlignment.MiddleRight;

        userInfoLabel.Dock = DockStyle.Top;
        userInfoLabel.Height = 38;
        userInfoLabel.Padding =
            new Padding(0, 8, 28, 0);

        systemStatusLabel.Text = "●  SYSTEM READY";
        systemStatusLabel.ForeColor =
            Color.FromArgb(134, 239, 172);

        systemStatusLabel.Font =
            new Font("Segoe UI", 9F, FontStyle.Bold);

        systemStatusLabel.TextAlign =
            ContentAlignment.MiddleRight;

        systemStatusLabel.Dock = DockStyle.Bottom;
        systemStatusLabel.Height = 38;
        systemStatusLabel.Padding =
            new Padding(0, 0, 28, 8);

        rightHeader.Controls.Add(userInfoLabel);
        rightHeader.Controls.Add(systemStatusLabel);

        header.Controls.Add(appTitle);
        header.Controls.Add(subtitle);
        header.Controls.Add(rightHeader);

        var body = new Panel
        {
            Dock = DockStyle.Fill,
            BackColor = UiTheme.PageBg
        };

        var side = new Panel
        {
            Dock = DockStyle.Left,
            Width = 236,
            Padding = new Padding(18, 12, 18, 18),
            BackColor = UiTheme.Sidebar
        };

        var menuCaption = new Label
        {
            Text = "MENU",
            AutoSize = true,
            ForeColor = UiTheme.Muted,
            Font = new Font("Segoe UI", 9F),
            Location = new Point(22, 14)
        };

        side.Controls.Add(menuCaption);

        var btnEquipment =
            CreateMenuButton("장비 제어", 48);

        var btnInspection =
            CreateMenuButton("검사 모니터링", 102);

        var btnChat =
            CreateMenuButton("채팅", 156);

        btnEquipment.Click += (_, _) =>
            ShowPage(
                equipmentPage,
                "장비 제어",
                0
            );

        btnInspection.Click += (_, _) =>
            ShowPage(
                inspectionPage,
                "검사 모니터링",
                1
            );

        btnChat.Click += (_, _) =>
            ShowPage(
                chatPage,
                "채팅",
                2
            );

        menuButtons.Add(btnEquipment);
        menuButtons.Add(btnInspection);
        menuButtons.Add(btnChat);

        side.Controls.Add(btnEquipment);
        side.Controls.Add(btnInspection);
        side.Controls.Add(btnChat);

        var btnHome = CreateMenuButton("⌂  홈 / 로그인 · 화면 전환", 236);
        btnHome.BackColor = UiTheme.AccentSoft;
        btnHome.ForeColor = UiTheme.AccentDark;
        btnHome.Click += (_, _) => ReturnToLogin();
        side.Controls.Add(btnHome);

        var right = new Panel
        {
            Dock = DockStyle.Fill,
            BackColor = UiTheme.PageBg
        };

        var pageHeader = new Panel
        {
            Dock = DockStyle.Top,
            Height = 66,
            Padding = new Padding(26, 17, 0, 0),
            BackColor = UiTheme.Card
        };

        pageTitleLabel.AutoSize = true;
        pageTitleLabel.Font =
            new Font(
                "Segoe UI",
                16F,
                FontStyle.Bold
            );

        pageTitleLabel.ForeColor =
            UiTheme.Text;

        pageHeader.Controls.Add(
            pageTitleLabel
        );

        var footer = new Panel
        {
            Dock = DockStyle.Bottom,
            Height = 44,
            Padding = new Padding(24, 12, 0, 0),
            BackColor = UiTheme.Card
        };

        footer.Paint += (_, e) =>
            e.Graphics.DrawLine(
                new Pen(UiTheme.Border),
                0,
                0,
                footer.Width,
                0
            );

        footerStatusLabel.Text =
            "Raspberry Pi  --     TCP/IP  --     Equipment  --";

        footerStatusLabel.AutoSize = true;

        footerStatusLabel.Font =
            new Font("Segoe UI", 9F);

        footerStatusLabel.ForeColor =
            UiTheme.Muted;

        footer.Controls.Add(
            footerStatusLabel
        );

        contentPanel.Dock =
            DockStyle.Fill;

        contentPanel.Padding =
            new Padding(18);

        contentPanel.BackColor =
            UiTheme.PageBg;

        right.Controls.Add(
            contentPanel
        );

        right.Controls.Add(
            footer
        );

        right.Controls.Add(
            pageHeader
        );

        body.Controls.Add(
            right
        );

        body.Controls.Add(
            side
        );

        Controls.Add(
            body
        );

        Controls.Add(
            header
        );
    }

    private void ReturnToLogin()
    {
        // 기존 로그인 화면을 재사용하고 작업자 화면만 닫는다.
        // 다른 권한으로 들어갈 때는 중앙 서버 인증을 다시 거친다.
        var login = Application.OpenForms.OfType<LoginForm>().FirstOrDefault();
        if (login == null || login.IsDisposed)
        {
            MessageBox.Show("로그인 화면을 찾을 수 없습니다.", "화면 전환");
            return;
        }
        _heartbeatTimer.Stop();
        login.PrepareForReturn();
        login.Show();
        login.BringToFront();
        Close();
    }

    private static Button CreateMenuButton(
        string text,
        int top) => new()
        {
            Text = text,
            Width = 200,
            Height = 46,

            FlatStyle =
                FlatStyle.Flat,

            FlatAppearance =
            {
                BorderSize = 0
            },

            BackColor =
                UiTheme.Sidebar,

            ForeColor =
                UiTheme.Text,

            Font =
                new Font(
                    "Segoe UI",
                    10F,
                    FontStyle.Bold
                ),

            TextAlign =
                ContentAlignment.MiddleLeft,

            Padding =
                new Padding(
                    16,
                    0,
                    0,
                    0
                ),

            Location =
                new Point(
                    18,
                    top
                ),

            Cursor =
                Cursors.Hand,

            UseVisualStyleBackColor =
                false
        };

    private void ShowPage(
        UserControl page,
        string title,
        int activeIndex)
    {
        pageTitleLabel.Text =
            title;

        for (var i = 0;
             i < menuButtons.Count;
             i++)
        {
            var active =
                i == activeIndex;

            menuButtons[i].BackColor =
                active
                    ? UiTheme.AccentSoft
                    : UiTheme.Sidebar;

            menuButtons[i].ForeColor =
                active
                    ? UiTheme.AccentDark
                    : UiTheme.Text;
        }

        contentPanel.SuspendLayout();

        contentPanel.Controls.Clear();

        page.Dock =
            DockStyle.Fill;

        contentPanel.Controls.Add(
            page
        );

        contentPanel.ResumeLayout();
    }


    private static string GetRoleName(
        string role)
    {
        return role switch
        {
            "ADMIN" => "관리자",
            "OPERATOR" => "현장 작업자",
            _ => role
        };
    }

    // 하트비트 신호가 들어왔을 때 실행되는 함수
    private void OnHeartbeatReceived()
    {
        if (IsDisposed || Disposing) return;
        if (InvokeRequired)
        {
            BeginInvoke((Action)OnHeartbeatReceived);
            return;
        }
        _hasReceivedFirstHeartbeat = true;
        _lastPiHeartbeat = DateTime.Now;

        if (_isAlarmActive)
        {
            _isAlarmActive = false;
            systemStatusLabel.Text = "● 시스템 정상";
            systemStatusLabel.ForeColor = Color.LightGreen;
        }
    }

    // 1초마다 시계를 보며 5초 이상 검사하는 함수
    private void CheckHeartbeatTimeout(object? sender, EventArgs e)
    {
        // 아직 한 번도 신호를 받은 적이 없다면 대기
        if (!_hasReceivedFirstHeartbeat) return;

        // 마지막 신호로부터 5초 이상 지났는지 확인
        if ((DateTime.Now - _lastPiHeartbeat).TotalSeconds >= 5.0 && !_isAlarmActive)
        {
            _isAlarmActive = true;
            systemStatusLabel.Text = "[경보] 장비 연결 끊김";
            systemStatusLabel.ForeColor = Color.Red;

            MessageBox.Show(
                "통신 신호가 5초 이상 두절되었습니다!\n현장 설비 및 랜선 연결 상태를 확인하세요.",
                "장비 비상 경보",
                MessageBoxButtons.OK,
                MessageBoxIcon.Error
            );
        }
    }
}
