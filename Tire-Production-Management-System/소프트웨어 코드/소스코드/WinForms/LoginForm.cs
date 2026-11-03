using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace TireEquipmentHMI
{
    public partial class LoginForm : Form
    {
        private TextBox txtLoginId;
        private TextBox txtPassword;
        private Button btnLogin;
        private Label lblStatus;
        private RadioButton rbOperator;
        private RadioButton rbAdmin;

        public LoginForm()
        {
            InitializeComponent();
            InitializeLoginUI();
        }

        private void InitializeLoginUI()
        {
            Text = "직원 로그인";
            StartPosition = FormStartPosition.CenterScreen;
            ClientSize = new Size(520, 505);
            BackColor = Color.FromArgb(245, 247, 250);

            FormBorderStyle = FormBorderStyle.FixedSingle;
            MaximizeBox = false;
            MinimizeBox = false;

            Panel headerPanel = new Panel
            {
                Dock = DockStyle.Top,
                Height = 95,
                BackColor = Color.FromArgb(24, 49, 83)
            };

            Label lblTitle = new Label
            {
                Text = "TIRE PRODUCTION SYSTEM",
                ForeColor = Color.White,
                Font = new Font("Segoe UI", 16, FontStyle.Bold),
                AutoSize = true,
                Location = new Point(125, 20)
            };

            Label lblSubTitle = new Label
            {
                Text = "직원 인증",
                ForeColor = Color.FromArgb(210, 225, 245),
                Font = new Font("맑은 고딕", 10),
                AutoSize = true,
                Location = new Point(225, 58)
            };

            headerPanel.Controls.Add(lblTitle);
            headerPanel.Controls.Add(lblSubTitle);

            Panel loginPanel = new Panel
            {
                Size = new Size(390, 340),
                Location = new Point(65, 125),
                BackColor = Color.White,
                BorderStyle = BorderStyle.FixedSingle
            };

            Label lblLoginId = new Label
            {
                Text = "아이디",
                Font = new Font("맑은 고딕", 10, FontStyle.Bold),
                ForeColor = Color.FromArgb(45, 55, 72),
                AutoSize = true,
                Location = new Point(40, 27)
            };

            txtLoginId = new TextBox
            {
                Font = new Font("Segoe UI", 11),
                Size = new Size(310, 32),
                Location = new Point(40, 55),
                BorderStyle = BorderStyle.FixedSingle
            };

            Label lblPassword = new Label
            {
                Text = "비밀번호",
                Font = new Font("맑은 고딕", 10, FontStyle.Bold),
                ForeColor = Color.FromArgb(45, 55, 72),
                AutoSize = true,
                Location = new Point(40, 103)
            };

            txtPassword = new TextBox
            {
                Font = new Font("Segoe UI", 11),
                Size = new Size(310, 32),
                Location = new Point(40, 131),
                BorderStyle = BorderStyle.FixedSingle,
                UseSystemPasswordChar = true
            };

            btnLogin = new Button
            {
                Text = "로그인",
                Font = new Font("맑은 고딕", 11, FontStyle.Bold),
                Size = new Size(310, 45),
                Location = new Point(40, 245),
                FlatStyle = FlatStyle.Flat,
                BackColor = Color.FromArgb(37, 99, 235),
                ForeColor = Color.White,
                Cursor = Cursors.Hand
            };

            btnLogin.FlatAppearance.BorderSize = 0;
            btnLogin.Click += BtnLogin_Click;

            lblStatus = new Label
            {
                Text = "● 로그인 정보를 입력해주세요.",
                Font = new Font("맑은 고딕", 9),
                ForeColor = Color.Gray,
                AutoSize = true,
                Location = new Point(40, 302)
            };

            var roleLabel = new Label
            {
                Text = "접속 화면 선택",
                Font = new Font("맑은 고딕", 10, FontStyle.Bold),
                ForeColor = Color.FromArgb(45, 55, 72),
                AutoSize = true,
                Location = new Point(40, 177)
            };
            rbOperator = new RadioButton
            {
                Text = "현장 작업자",
                Checked = true,
                Font = new Font("맑은 고딕", 10),
                Location = new Point(40, 204),
                Size = new Size(140, 28),
                BackColor = Color.White
            };
            rbAdmin = new RadioButton
            {
                Text = "관리자",
                Font = new Font("맑은 고딕", 10),
                Location = new Point(200, 204),
                Size = new Size(120, 28),
                BackColor = Color.White
            };
            loginPanel.Controls.Add(roleLabel);
            loginPanel.Controls.Add(rbOperator);
            loginPanel.Controls.Add(rbAdmin);
            loginPanel.Controls.Add(lblLoginId);
            loginPanel.Controls.Add(txtLoginId);
            loginPanel.Controls.Add(lblPassword);
            loginPanel.Controls.Add(txtPassword);
            loginPanel.Controls.Add(btnLogin);
            loginPanel.Controls.Add(lblStatus);

            Controls.Add(headerPanel);
            Controls.Add(loginPanel);

            AcceptButton = btnLogin;
        }

        private async void BtnLogin_Click(object sender, EventArgs e)
        {
            string loginId = txtLoginId.Text.Trim();
            string password = txtPassword.Text;

            if (string.IsNullOrWhiteSpace(loginId))
            {
                MessageBox.Show(
                    "아이디를 입력해주세요.",
                    "로그인",
                    MessageBoxButtons.OK,
                    MessageBoxIcon.Warning
                );

                txtLoginId.Focus();
                return;
            }

            if (string.IsNullOrWhiteSpace(password))
            {
                MessageBox.Show(
                    "비밀번호를 입력해주세요.",
                    "로그인",
                    MessageBoxButtons.OK,
                    MessageBoxIcon.Warning
                );

                txtPassword.Focus();
                return;
            }

            btnLogin.Enabled = false;

            lblStatus.Text = "● 중앙 서버 연결 중...";
            lblStatus.ForeColor = Color.FromArgb(37, 99, 235);

            try
            {
                LoginResponse response =
                    await SendLoginRequest(loginId, password);

                if (response.status == "OK")
                {
                    lblStatus.Text =
                        $"● 로그인 성공 - {response.user_name}";

                    lblStatus.ForeColor =
                        Color.FromArgb(22, 163, 74);


                    string normalizedRole =
                        (response.role ?? string.Empty).ToUpperInvariant();

                    if (rbAdmin.Checked && normalizedRole != "ADMIN")
                    {
                        lblStatus.Text = "● 관리자 권한이 없습니다.";
                        lblStatus.ForeColor = Color.Red;
                        MessageBox.Show("관리자 화면은 관리자 계정으로만 접속할 수 있습니다.",
                            "권한 확인", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                        return;
                    }

                    if (rbAdmin.Checked)
                    {
                        StartMfcProgram(
                            response.user_name ?? "",
                            normalizedRole
                        );
                    }
                    else if (rbOperator.Checked && (normalizedRole == "OPERATOR" || normalizedRole == "ADMIN"))
                    {
                        MainForm mainForm = new MainForm(
                            response.user_name ?? "",
                            normalizedRole,
                            response.fingerprint_id
                        );

                        Hide();

                        mainForm.FormClosed +=
                            (s, args) =>
                            {
                                // 홈으로 돌아온 경우 로그인 창은 유지
                                if (!Visible) Close();
                            };

                        mainForm.Show();
                    }
                    else
                    {
                        MessageBox.Show(
                            "등록되지 않은 사용자 권한입니다.",
                            "로그인 오류",
                            MessageBoxButtons.OK,
                            MessageBoxIcon.Error
                        );
                    }
                }
                else
                {
                    lblStatus.Text = "● 로그인 실패";
                    lblStatus.ForeColor = Color.Red;

                    MessageBox.Show(
                        response.message ??
                        "아이디 또는 비밀번호가 일치하지 않습니다.",
                        "로그인 실패",
                        MessageBoxButtons.OK,
                        MessageBoxIcon.Error
                    );

                    txtPassword.Clear();
                    txtPassword.Focus();
                }
            }
            catch (SocketException)
            {
                lblStatus.Text =
                    "● 중앙 서버 연결 실패";

                lblStatus.ForeColor = Color.Red;

                MessageBox.Show(
                    "중앙 서버에 연결할 수 없습니다.\n" +
                    "서버 프로그램이 실행 중인지 확인해주세요.",
                    "서버 연결 실패",
                    MessageBoxButtons.OK,
                    MessageBoxIcon.Error
                );
            }
            catch (Exception ex)
            {
                lblStatus.Text = "● 오류 발생";
                lblStatus.ForeColor = Color.Red;

                MessageBox.Show(
                    ex.Message,
                    "오류",
                    MessageBoxButtons.OK,
                    MessageBoxIcon.Error
                );
            }
            finally
            {
                btnLogin.Enabled = true;
            }
        }

        public void PrepareForReturn()
        {
            txtPassword.Clear();
            lblStatus.Text = "● 다른 계정으로 로그인할 수 있습니다.";
            lblStatus.ForeColor = Color.Gray;
            btnLogin.Enabled = true;
        }

        private void StartMfcProgram(
            string userName,
            string role)
        {
            try
            {
                string? mfcPath = FindMfcExecutable();

                if (string.IsNullOrWhiteSpace(mfcPath) || !File.Exists(mfcPath))
                {
                    MessageBox.Show(
                        "관리자 프로그램을 찾을 수 없습니다.\n" +
                        "실행 파일 옆 Admin 폴더에 TireProductionMFC.exe가 있는지 확인해주세요.",
                        "MFC 실행 오류",
                        MessageBoxButtons.OK,
                        MessageBoxIcon.Error
                    );
                    return;
                }

                Process.Start(
                    new ProcessStartInfo
                    {
                        FileName = mfcPath,
                        Arguments = $"\"{userName}\" \"{role}\"",
                        WorkingDirectory = Path.GetDirectoryName(mfcPath) ?? string.Empty,
                        UseShellExecute = true
                    }
                );

                Close();
            }
            catch (Exception ex)
            {
                MessageBox.Show(
                    ex.Message,
                    "MFC 실행 오류",
                    MessageBoxButtons.OK,
                    MessageBoxIcon.Error
                );
            }
        }

        private static string? FindMfcExecutable()
        {
            string packagedPath = Path.Combine(
                AppContext.BaseDirectory,
                "Admin",
                "TireProductionMFC.exe");

            if (File.Exists(packagedPath))
                return packagedPath;

            DirectoryInfo? current = new DirectoryInfo(AppContext.BaseDirectory);

            while (current != null)
            {
                string solutionPath = Path.Combine(current.FullName, "TireProductionBase.sln");

                if (File.Exists(solutionPath))
                {
                    string debugPath = Path.Combine(
                        current.FullName,
                        "x64",
                        "Debug",
                        "TireProductionMFC.exe"
                    );

                    if (File.Exists(debugPath))
                        return debugPath;

                    string releasePath = Path.Combine(
                        current.FullName,
                        "x64",
                        "Release",
                        "TireProductionMFC.exe"
                    );

                    if (File.Exists(releasePath))
                        return releasePath;

                    string projectDebugPath = Path.Combine(
                        current.FullName,
                        "MFC",
                        "x64",
                        "Debug",
                        "TireProductionMFC.exe"
                    );

                    if (File.Exists(projectDebugPath))
                        return projectDebugPath;

                    string projectReleasePath = Path.Combine(
                        current.FullName,
                        "MFC",
                        "x64",
                        "Release",
                        "TireProductionMFC.exe"
                    );

                    if (File.Exists(projectReleasePath))
                        return projectReleasePath;

                    return debugPath;
                }

                current = current.Parent;
            }

            return null;
        }

        private async Task<LoginResponse> SendLoginRequest(
            string loginId,
            string password)
        {
            using TcpClient client =
                new TcpClient();

            await client.ConnectAsync(
                AppConfig.Current.ServerIp,
                AppConfig.Current.ServerPort
            );

            using NetworkStream stream =
                client.GetStream();

            using StreamReader reader =
                new StreamReader(
                    stream,
                    Encoding.UTF8,
                    false,
                    1024,
                    true
                );

            using StreamWriter writer =
                new StreamWriter(
                    stream,
                    new UTF8Encoding(false),
                    1024,
                    true
                );

            writer.AutoFlush = true;

            var request = new
            {
                type = "LOGIN",
                login_id = loginId,
                password = password
            };

            string json =
                JsonSerializer.Serialize(request);

            await writer.WriteLineAsync(json);

            string? responseJson =
                await reader.ReadLineAsync();

            if (string.IsNullOrWhiteSpace(responseJson))
            {
                throw new Exception(
                    "서버 응답이 없습니다."
                );
            }

            LoginResponse? response =
                JsonSerializer.Deserialize<LoginResponse>(
                    responseJson
                );

            if (response == null)
            {
                throw new Exception(
                    "로그인 응답을 읽을 수 없습니다."
                );
            }

            return response;
        }

        private string GetRoleName(
            string? role)
        {
            if (role == "ADMIN")
                return "관리자";

            if (role == "OPERATOR")
                return "현장 작업자";

            return role ?? "-";
        }

        private class LoginResponse
        {
            public string? status { get; set; }

            public string? user_name { get; set; }

            public string? role { get; set; }

            public int fingerprint_id { get; set; }

            public string? message { get; set; }
        }
    }
}
