using TireEquipmentHMI.Theme;
using System.Text.Json;

namespace TireEquipmentHMI.Pages;

public sealed class EquipmentControlPage : UserControl
{
    public Func<string, Task<bool>>? HardwareCommandAsync;

    public Label lblConveyorStatus = new();
    public Label lblMotorRpm = new();
    public Label lblServoStatus = new();
    public Label lblSensorStatus = new();
    public Label lblLedStatus = new();
    public Label lblBuzzerStatus = new();
    public Label lblLastAction = new();

    public Button btnStart = new();
    public Button btnStop = new();
    public Button btnReset = new();
    public Button btnServoAction = new();
    public Button btnServoReturn = new();

    public RadioButton rbAuto = new();
    public RadioButton rbManual = new();

    public EquipmentControlPage()
    {
        BackColor = UiTheme.PageBg;
        BuildUi();
        WireEvents();
    }

    private void BuildUi()
    {
        var main = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 2,
            RowCount = 2,
            Padding = new Padding(0),
            BackColor = UiTheme.PageBg
        };

        main.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
        main.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
        main.RowStyles.Add(new RowStyle(SizeType.Percent, 60));
        main.RowStyles.Add(new RowStyle(SizeType.Percent, 40));

        main.Controls.Add(BuildConveyorCard(), 0, 0);
        main.Controls.Add(BuildManualControlCard(), 1, 0);

        var stateCard = BuildStateCard();
        main.Controls.Add(stateCard, 0, 1);
        main.SetColumnSpan(stateCard, 2);

        Controls.Add(main);
    }

    private Control BuildConveyorCard()
    {
        var card = CreateCard("컨베이어 / 모터 상태");

        lblConveyorStatus = UiFactory.ValueLabel("Conveyor : 정지", 24, 66, 12.5F);
        lblMotorRpm = UiFactory.ValueLabel("Motor RPM : -- (실측 미연결)", 24, 112, 12.5F);

        btnStart = UiFactory.ActionButton("START", 24, 184);
        btnStop = UiFactory.ActionButton("STOP", 150, 184);
        btnReset = UiFactory.ActionButton("재확인 해제", 276, 184);

        var simulationInfo = new Label
        {
            Text = "제어 명령은 Raspberry Pi에 전송됩니다",
            AutoSize = true,
            ForeColor = UiTheme.Muted,
            Font = new Font("맑은 고딕", 9F),
            Location = new Point(24, 242)
        };

        card.Controls.AddRange([
            lblConveyorStatus,
            lblMotorRpm,
            btnStart,
            btnStop,
            btnReset,
            simulationInfo
        ]);

        return card;
    }

    private Control BuildManualControlCard()
    {
        var card = CreateCard("수동 제어 / 운전 모드");

        lblServoStatus = UiFactory.ValueLabel("Servo : 원점", 24, 66, 12.5F);

        btnServoAction = UiFactory.ActionButton("SERVO 동작", 24, 128, 150);
        btnServoReturn = UiFactory.ActionButton("SERVO 복귀", 190, 128, 150);

        var modeLabel = new Label
        {
            Text = "운전 모드",
            AutoSize = true,
            ForeColor = UiTheme.Muted,
            Font = new Font("맑은 고딕", 9.5F),
            Location = new Point(24, 208)
        };

        rbAuto = new RadioButton
        {
            Text = "AUTO",
            Location = new Point(24, 238),
            AutoSize = true,
            Checked = true,
            ForeColor = UiTheme.Text,
            Font = new Font("Segoe UI", 10F, FontStyle.Bold)
        };

        rbManual = new RadioButton
        {
            Text = "MANUAL",
            Location = new Point(130, 238),
            AutoSize = true,
            ForeColor = UiTheme.Text,
            Font = new Font("Segoe UI", 10F, FontStyle.Bold)
        };

        card.Controls.AddRange([
            lblServoStatus,
            btnServoAction,
            btnServoReturn,
            modeLabel,
            rbAuto,
            rbManual
        ]);

        return card;
    }

    private Control BuildStateCard()
    {
        var card = CreateCard("실제 하드웨어 상태");

        lblSensorStatus = UiFactory.ValueLabel("검사 : 대기", 24, 64, 12F);
        lblLedStatus = UiFactory.ValueLabel("LED : OFF", 286, 64, 12F);
        lblBuzzerStatus = UiFactory.ValueLabel("Buzzer : OFF", 500, 64, 12F);

        lblLastAction = new Label
        {
            Text = $"최근 동작: {DateTime.Now:HH:mm:ss}  장비 상태 수신 대기",
            AutoSize = true,
            ForeColor = UiTheme.Muted,
            Font = new Font("맑은 고딕", 9F),
            Location = new Point(24, 105)
        };

        card.Controls.AddRange([
            lblSensorStatus,
            lblLedStatus,
            lblBuzzerStatus,
            lblLastAction
        ]);

        return card;
    }

    private void WireEvents()
    {
        btnStart.Click += async (_, _) => await IssueCommand("START");
        btnStop.Click += async (_, _) => await IssueCommand("STOP");
        btnReset.Click += async (_, _) => await IssueCommand("ACK_RECHECK");
        btnServoAction.Click += async (_, _) => await IssueCommand("SERVO_TEST");
        btnServoReturn.Click += async (_, _) => await IssueCommand("SERVO_HOME");


        rbAuto.CheckedChanged += (_, _) =>
        {
            if (rbAuto.Checked)
                _ = IssueCommand("AUTO");
        };

        rbManual.CheckedChanged += (_, _) =>
        {
            if (rbManual.Checked)
                _ = IssueCommand("MANUAL");
        };
    }

    private async Task IssueCommand(string action)
    {
        if (HardwareCommandAsync is null)
        {
            SetLastAction("Pi 제어 연결 없음"); return;
        }
        bool sent = await HardwareCommandAsync(action);
        SetLastAction(sent ? $"{action} 요청 완료" : $"{action} 요청 실패");
    }

    public void ApplyHardwareState(JsonElement s)
    {
        static string Prop(JsonElement data, string key, string fallback = "-") =>
            data.TryGetProperty(key, out var v) ? v.ToString() : fallback;
        var state = Prop(s, "state");
        lblConveyorStatus.Text = "Conveyor : " + state;
        lblMotorRpm.Text = "검사 모드 : " + Prop(s, "mode");
        lblServoStatus.Text = "Servo : " + Prop(s, "servo", "HOME");
        lblSensorStatus.Text = "검사 결과 : " + Prop(s, "last_result");
        lblLedStatus.Text = "RGB1 : " + Prop(s, "rgb1") + " / RGB2 : " + Prop(s, "rgb2");
        lblBuzzerStatus.Text = "Buzzer : " + Prop(s, "buzzer");
        lblLastAction.Text = "장비 상태: " + Prop(s, "message");
        lblLastAction.ForeColor = state == "RECHECK" || state == "ESTOP" ? UiTheme.Danger : UiTheme.Muted;
        btnStart.Enabled = state != "ESTOP" && state != "RECHECK";
        btnServoAction.Enabled = state != "RUNNING" && state != "ESTOP";
        btnReset.Enabled = state == "RECHECK";
    }


    public void SetHardwareDisconnected()
    {
        lblConveyorStatus.Text = "Conveyor : 연결 끊김";
        lblServoStatus.Text = "Servo : 상태 확인 불가";
        lblSensorStatus.Text = "검사 결과 : 연결 끊김";
        lblLedStatus.Text = "LED : 상태 확인 불가";
        lblBuzzerStatus.Text = "Buzzer : 상태 확인 불가";

        lblLastAction.Text = "Raspberry Pi 상태 수신 실패";
        lblLastAction.ForeColor = UiTheme.Danger;

        btnStart.Enabled = false;
        btnServoAction.Enabled = false;
        btnServoReturn.Enabled = false;
        btnReset.Enabled = false;
        rbAuto.Enabled = false;
        rbManual.Enabled = false;
    }


    private void SetLastAction(string text)
    {
        lblLastAction.Text = $"최근 동작: {DateTime.Now:HH:mm:ss}  {text}";
    }

    private static ModernCardPanel CreateCard(string title)
    {
        var card = new ModernCardPanel { Dock = DockStyle.Fill };
        card.Controls.Add(UiFactory.CardTitle(title));
        return card;
    }
}
