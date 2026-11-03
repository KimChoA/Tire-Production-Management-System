using Microsoft.Data.Sqlite;
using System;
using System.Threading;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using System.Threading.Tasks;

namespace TireProductionServer
{
    internal class Program
    {
        private static readonly ServerConfig config = ServerConfig.Load();

        private static readonly string dbPath = config.ResolveDatabasePath();

        private static readonly string connectionString =
            $"Data Source={dbPath}";

        private static readonly int SERVER_PORT = config.Port;

        private static readonly ConcurrentDictionary<Guid, ConnectedClient> clients = new();

        private sealed class ConnectedClient
        {
            private readonly SemaphoreSlim sendLock = new(1, 1);

            public Guid Id { get; } = Guid.NewGuid();
            public TcpClient TcpClient { get; }
            public StreamWriter Writer { get; }

            public ConnectedClient(TcpClient tcpClient, StreamWriter writer)
            {
                TcpClient = tcpClient;
                Writer = writer;
            }

            public async Task SendLineAsync(string message)
            {
                await sendLock.WaitAsync();
                try
                {
                    await Writer.WriteLineAsync(message);
                }
                finally
                {
                    sendLock.Release();
                }
            }
        }

        static async Task Main(string[] args)
        {
            Console.OutputEncoding = Encoding.UTF8;
            Console.Title = "Tire Production Central Server";

            Console.WriteLine("==========================================");
            Console.WriteLine("     Tire Production Central Server");
            Console.WriteLine("==========================================");
            Console.WriteLine();

            InitializeDatabase();

            // DB 연결 확인
            if (!CheckDatabase())
            {
                Console.WriteLine();
                Console.WriteLine("[ERROR] DB 연결에 실패했습니다.");
                Console.WriteLine("Enter 키를 누르면 종료합니다.");
                Console.ReadLine();
                return;
            }

            Console.WriteLine("[OK] SQLite DB 연결 성공");
            Console.WriteLine($"[DB] {dbPath}");
            Console.WriteLine();

            ShowUsers();

            Console.WriteLine();

            // TCP 서버 시작
            TcpListener listener =
                new TcpListener(IPAddress.Any, SERVER_PORT);

            try
            {
                listener.Start();

                Console.WriteLine("==========================================");
                Console.WriteLine("[SERVER] TCP 서버 시작");
                Console.WriteLine($"[PORT] {SERVER_PORT}");
                Console.WriteLine("[WAIT] 클라이언트 접속 대기 중...");
                Console.WriteLine("==========================================");
                Console.WriteLine();

                while (true)
                {
                    TcpClient client =
                        await listener.AcceptTcpClientAsync();

                    string clientIp =
                        client.Client.RemoteEndPoint?.ToString()
                        ?? "UNKNOWN";

                    Console.WriteLine(
                        $"[CONNECT] 클라이언트 접속 : {clientIp}"
                    );

                    _ = HandleClient(client);
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine();
                Console.WriteLine("[SERVER ERROR]");
                Console.WriteLine(ex.Message);
            }
            finally
            {
                listener.Stop();
            }
        }

        private static void InitializeDatabase()
        {
            string? directory = Path.GetDirectoryName(dbPath);
            if (!string.IsNullOrWhiteSpace(directory))
                Directory.CreateDirectory(directory);

            using SqliteConnection conn = new(connectionString);
            conn.Open();

            const string sql = @"
                CREATE TABLE IF NOT EXISTS users
                (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    login_id TEXT NOT NULL UNIQUE,
                    password_hash TEXT NOT NULL,
                    user_name TEXT NOT NULL,
                    role TEXT NOT NULL,
                    fingerprint_id INTEGER NOT NULL DEFAULT 0,
                    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
                );

                CREATE TABLE IF NOT EXISTS inspection
                (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    tire_id TEXT NOT NULL,
                    result TEXT NOT NULL,
                    defect_type TEXT,
                    defect_detail TEXT,
                    image_path TEXT,
                    is_demo INTEGER NOT NULL DEFAULT 0,
                    data_source TEXT NOT NULL DEFAULT 'LEGACY_UNVERIFIED',
                    event_id TEXT,
                    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
                );

                CREATE TABLE IF NOT EXISTS equipment_log
                (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    equipment TEXT NOT NULL,
                    status TEXT NOT NULL,
                    message TEXT,
                    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
                );

                CREATE TABLE IF NOT EXISTS chat
                (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    user_name TEXT NOT NULL,
                    message TEXT NOT NULL,
                    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
                );

                CREATE TABLE IF NOT EXISTS handover
                (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    work_date TEXT NOT NULL,
                    shift_type TEXT NOT NULL,
                    priority TEXT NOT NULL DEFAULT 'NORMAL',
                    user_name TEXT NOT NULL,
                    message TEXT NOT NULL,
                    checked INTEGER NOT NULL DEFAULT 0,
                    checked_by TEXT,
                    checked_at TEXT,
                    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
                );

                INSERT OR IGNORE INTO users
                    (login_id, password_hash, user_name, role, fingerprint_id)
                VALUES
                    ('wonjong01', '1234', '노원종', 'OPERATOR', 1);
            ";

            using SqliteCommand cmd = new(sql, conn);
            cmd.ExecuteNonQuery();

            // CREATE TABLE IF NOT EXISTS는 기존 테이블에 컬럼을 추가하지 않습니다.
            // 이전 버전 DB에도 검사 시간 컬럼을 보완하되, 검사 기록은 삭제하지 않습니다.
            EnsureInspectionTimestampColumn(conn);
            EnsureInspectionDefectColumns(conn);
            EnsureInspectionDemoColumn(conn);
            EnsureInspectionSourceAndEventId(conn);
            EnsureInspectionVisionColumns(conn);
            EnsureEventSessions(conn);
            BackfillLegacyEquipmentSessions(conn);
        }

        private static void EnsureInspectionTimestampColumn(SqliteConnection conn)
        {
            List<string> columns = new();
            using (SqliteCommand pragma = conn.CreateCommand())
            {
                pragma.CommandText = "PRAGMA table_info(inspection);";
                using SqliteDataReader reader = pragma.ExecuteReader();
                while (reader.Read())
                    columns.Add(reader.GetString(1));
            }

            if (columns.Any(name => name.Equals("created_at", StringComparison.OrdinalIgnoreCase)))
                return;

            using (SqliteCommand alter = conn.CreateCommand())
            {
                // SQLite에서는 기존 행이 있는 테이블에 CURRENT_TIMESTAMP를
                // DEFAULT로 추가할 수 없으므로 NULL 허용 컬럼을 사용합니다.
                alter.CommandText = "ALTER TABLE inspection ADD COLUMN created_at TEXT;";
                alter.ExecuteNonQuery();
            }
            Console.WriteLine("[DB MIGRATION] inspection.created_at 컬럼 추가 (기존 검사 기록 유지)");

            // 과거 검사 일시 컬럼이 명확히 존재하는 경우만 해당 값을 보존합니다.
            // 알 수 없는 과거 검사 시각을 현재 시각으로 채우지 않습니다.
            string[] legacyNames =
            {
                "inspected_at", "inspection_time", "inspection_datetime",
                "recorded_at", "timestamp", "date_time", "inspection_date"
            };
            string? legacy = legacyNames.FirstOrDefault(candidate =>
                columns.Any(name => name.Equals(candidate, StringComparison.OrdinalIgnoreCase)));

            if (legacy != null)
            {
                using SqliteCommand backfill = conn.CreateCommand();
                backfill.CommandText =
                    $"UPDATE inspection SET created_at = \"{legacy}\" " +
                    $"WHERE typeof(\"{legacy}\") = 'text' " +
                    $"AND datetime(\"{legacy}\") IS NOT NULL AND created_at IS NULL;";
                int filled = backfill.ExecuteNonQuery();
                Console.WriteLine($"[DB MIGRATION] 기존 '{legacy}'에서 {filled}건의 검사 시각 복구");
                Console.WriteLine("[DB NOTICE] 복구한 시각의 UTC/한국시간 저장 기준을 확인하세요.");
            }
            else
            {
                Console.WriteLine("[DB NOTICE] 과거 검사 시간 컬럼이 없어 과거 기록은 근무별 시간 집계에서 제외됩니다.");
            }
        }

        // 기존 DB 데이터를 삭제하지 않고 불량 상세 컬럼을 보완합니다.
        private static void EnsureInspectionDefectColumns(SqliteConnection conn)
        {
            HashSet<string> columns = new(StringComparer.OrdinalIgnoreCase);
            using (SqliteCommand pragma = conn.CreateCommand())
            {
                pragma.CommandText = "PRAGMA table_info(inspection);";
                using SqliteDataReader reader = pragma.ExecuteReader();
                while (reader.Read()) columns.Add(reader.GetString(1));
            }
            foreach (string name in new[] { "defect_type", "defect_detail", "image_path" })
            {
                if (columns.Contains(name)) continue;
                using SqliteCommand alter = conn.CreateCommand();
                alter.CommandText = $"ALTER TABLE inspection ADD COLUMN {name} TEXT;";
                alter.ExecuteNonQuery();
                Console.WriteLine($"[DB MIGRATION] inspection.{name} 컬럼 추가");
            }
        }

        private static void EnsureInspectionDemoColumn(SqliteConnection conn)
        {
            using SqliteCommand pragma = conn.CreateCommand();
            pragma.CommandText = "PRAGMA table_info(inspection);";
            bool found = false;
            using (SqliteDataReader reader = pragma.ExecuteReader())
            {
                while (reader.Read())
                    if (string.Equals(reader.GetString(1), "is_demo", StringComparison.OrdinalIgnoreCase))
                        found = true;
            }
            if (found) return;
            using SqliteCommand alter = conn.CreateCommand();
            alter.CommandText = "ALTER TABLE inspection ADD COLUMN is_demo INTEGER NOT NULL DEFAULT 0;";
            alter.ExecuteNonQuery();
            Console.WriteLine("[DB MIGRATION] inspection.is_demo 컬럼 추가");
        }

        // Provenance: older entries have no verified hardware origin. Do not backfill as real.
        private static void EnsureInspectionSourceAndEventId(SqliteConnection conn)
        {
            HashSet<string> columns = new(StringComparer.OrdinalIgnoreCase);
            using (SqliteCommand pragma = conn.CreateCommand())
            {
                pragma.CommandText = "PRAGMA table_info(inspection);";
                using SqliteDataReader reader = pragma.ExecuteReader();
                while (reader.Read()) columns.Add(reader.GetString(1));
            }
            if (!columns.Contains("data_source"))
            {
                using SqliteCommand alter = conn.CreateCommand();
                alter.CommandText = "ALTER TABLE inspection ADD COLUMN data_source TEXT NOT NULL DEFAULT 'LEGACY_UNVERIFIED';";
                alter.ExecuteNonQuery();
            }
            if (!columns.Contains("event_id"))
            {
                using SqliteCommand alter = conn.CreateCommand();
                alter.CommandText = "ALTER TABLE inspection ADD COLUMN event_id TEXT;";
                alter.ExecuteNonQuery();
            }
            using SqliteCommand migration = conn.CreateCommand();
            migration.CommandText = @"
                UPDATE inspection
                SET is_demo = 1, data_source = 'DEMO'
                WHERE is_demo = 1 OR UPPER(COALESCE(tire_id,'')) LIKE 'DEMO-%'
                   OR UPPER(COALESCE(tire_id,'')) LIKE 'VIRTUAL-%'
                   OR UPPER(COALESCE(defect_type,'')) = 'VIRTUAL_NG';
                CREATE UNIQUE INDEX IF NOT EXISTS ux_inspection_event_id
                    ON inspection(event_id) WHERE event_id IS NOT NULL;
                CREATE UNIQUE INDEX IF NOT EXISTS ux_inspection_hw_tire
                    ON inspection(tire_id) WHERE data_source = 'HARDWARE';";
            migration.ExecuteNonQuery();
            Console.WriteLine("[DB] 기존 검사 출처 미확인 기록은 실검사 통계에서 제외됩니다.");
        }


                private static void EnsureInspectionVisionColumns(SqliteConnection conn)
        {
            HashSet<string> columns = new(StringComparer.OrdinalIgnoreCase);

            using (var pragma = conn.CreateCommand())
            {
                pragma.CommandText = "PRAGMA table_info(inspection);";
                using var reader = pragma.ExecuteReader();
                while (reader.Read())
                    columns.Add(reader.GetString(1));
            }

            foreach (string definition in new[]
            {
                "yolo_result TEXT",
                "opencv_result TEXT",
                "yolo_confidence REAL",
                "inspection_time TEXT"
            })
            {
                string name = definition.Split(' ')[0];
                if (columns.Contains(name)) continue;

                using var command = conn.CreateCommand();
                command.CommandText =
                    $"ALTER TABLE inspection ADD COLUMN {definition};";
                command.ExecuteNonQuery();
            }
        }
        private static string NormalizeDefectType(string raw)
        {
            string value = (raw ?? "").Trim().ToUpperInvariant();

            return value switch
            {
                "WHEEL_DAMAGE" or "WHEEL"
                    => "WHEEL_DAMAGE",

                "TIRE_DAMAGE" or "TIRE" or "RUBBER_DAMAGE"
                    => "TIRE_DAMAGE",

                "BOTH_DAMAGE" or "BOTH" or "WHEEL_TIRE_DAMAGE"
                    => "BOTH_DAMAGE",

                "WRONG_TIRE" or "NG_WRONG_TIRE"
                    => "WRONG_TIRE",

                _ => ""
            };
        }


        // Date range production statistics are based solely on verified hardware records.
        private static string GetStatisticsRange(JsonElement request)
        {
            string startText = GetString(request, "start_date");
            string endText = GetString(request, "end_date");
            var culture = System.Globalization.CultureInfo.InvariantCulture;
            if (!DateTime.TryParseExact(startText, "yyyy-MM-dd", culture,
                System.Globalization.DateTimeStyles.None, out DateTime start) ||
                !DateTime.TryParseExact(endText, "yyyy-MM-dd", culture,
                System.Globalization.DateTimeStyles.None, out DateTime end) ||
                start > end || (end - start).TotalDays > 366)
                throw new Exception("통계 조회 기간이 올바르지 않습니다 (최대 366일).");

            int[] hourlyTotal = new int[8], hourlyNg = new int[8];
            int[] dailyTotal = new int[7], dailyNg = new int[7];
            int total = 0, passed = 0, failed = 0;
            using SqliteConnection conn = new(connectionString);
            conn.Open();
            using SqliteCommand cmd = conn.CreateCommand();
            cmd.CommandText = @"SELECT strftime('%Y-%m-%d %H:%M:%S', created_at, '+9 hours'),
                        UPPER(TRIM(result)) FROM inspection
                  WHERE datetime(created_at, '+9 hours') >= $start
                    AND datetime(created_at, '+9 hours') < $end
                    AND data_source = 'HARDWARE' AND COALESCE(is_demo, 0) = 0;";
            cmd.Parameters.AddWithValue("$start", startText + " 00:00:00");
            cmd.Parameters.AddWithValue("$end", end.AddDays(1).ToString("yyyy-MM-dd") + " 00:00:00");
            using SqliteDataReader r = cmd.ExecuteReader();
            while (r.Read())
            {
                if (r.IsDBNull(0)) continue;
                if (!DateTime.TryParseExact(r.GetString(0), "yyyy-MM-dd HH:mm:ss",
                        culture, System.Globalization.DateTimeStyles.None, out DateTime time)) continue;
                string verdict = r.GetString(1);
                bool ok = verdict is "OK" or "PASS" or "NORMAL";
                bool ng = verdict is "NG" or "FAIL" or "DEFECT";
                if (!ok && !ng) continue;
                total++;
                if (ok) passed++; else failed++;
                if (time.Date == end.Date && time.Hour >= 9 && time.Hour <= 16)
                {
                    hourlyTotal[time.Hour - 9]++;
                    if (ng) hourlyNg[time.Hour - 9]++;
                }
                int dayIndex = 6 - (end.Date - time.Date).Days;
                if (dayIndex >= 0 && dayIndex < 7)
                {
                    dailyTotal[dayIndex]++;
                    if (ng) dailyNg[dayIndex]++;
                }
            }
            return JsonSerializer.Serialize(new
            {
                type = "STATISTICS_RANGE_RESULT", status = "OK", total, passed, failed,
                hourly_total = string.Join(",", hourlyTotal),
                hourly_ng = string.Join(",", hourlyNg),
                daily_total = string.Join(",", dailyTotal),
                daily_ng = string.Join(",", dailyNg)
            });
        }

        private static string GetInspectionReport(JsonElement request)
        {
            string date = GetString(request, "date");
            if (!DateTime.TryParseExact(date, "yyyy-MM-dd",
                System.Globalization.CultureInfo.InvariantCulture,
                System.Globalization.DateTimeStyles.None, out _))
                throw new Exception("보고서 날짜는 YYYY-MM-DD 형식이어야 합니다.");

            long total = 0, passed = 0, failed = 0;
            long wheel = 0, tire = 0, both = 0, wrong = 0, unknown = 0;
            int[] hours = { 9, 10, 11, 13, 14, 15, 16, 17 };
            long[] hTotal = new long[hours.Length];
            long[] hPass = new long[hours.Length];
            long[] hFail = new long[hours.Length];
            List<string> detailRows = new();

            using SqliteConnection conn = new(connectionString);
            conn.Open();
            using SqliteCommand cmd = conn.CreateCommand();
            cmd.CommandText = @"
                SELECT id, COALESCE(tire_id, ''), UPPER(TRIM(COALESCE(result, ''))),
                       COALESCE(defect_type, ''), COALESCE(defect_detail, ''),
                       COALESCE(image_path, ''),
                       strftime('%Y-%m-%d %H:%M:%S', created_at, '+9 hours')
                FROM inspection
                WHERE datetime(created_at, '+9 hours') >= $start
                  AND datetime(created_at, '+9 hours') < $end
                  AND data_source = 'HARDWARE' AND COALESCE(is_demo, 0) = 0
                ORDER BY id;";
            cmd.Parameters.AddWithValue("$start", date + " 00:00:00");
            cmd.Parameters.AddWithValue("$end", DateTime.ParseExact(date, "yyyy-MM-dd",
                System.Globalization.CultureInfo.InvariantCulture).AddDays(1).ToString("yyyy-MM-dd") + " 00:00:00");

            using SqliteDataReader r = cmd.ExecuteReader();
            while (r.Read())
            {
                string verdict = r.GetString(2);
                bool ok = verdict is "OK" or "PASS" or "NORMAL";
                bool ng = verdict is "NG" or "FAIL" or "DEFECT";
                total++;
                if (ok) passed++;
                if (ng) failed++;
                string when = r.IsDBNull(6) ? "" : r.GetString(6);
                if (when.Length >= 13 && int.TryParse(when.Substring(11, 2), out int hour))
                {
                    int idx = Array.IndexOf(hours, hour);
                    if (idx >= 0)
                    {
                        hTotal[idx]++;
                        if (ok) hPass[idx]++;
                        if (ng) hFail[idx]++;
                    }
                }
                if (!ng) continue;
                string type = NormalizeDefectType(r.GetString(3));
                switch (type)
                {
                    case "WHEEL_DAMAGE": wheel++; break;
                    case "TIRE_DAMAGE": tire++; break;
                    case "BOTH_DAMAGE": both++; break;
                    case "WRONG_TIRE": wrong++; break;
                    default: unknown++; break;
                }
                // 콤마·줄바꿈·한글·구분자는 URL 인코딩 후 전송해 파싱 혼란을 방지합니다.
                if (detailRows.Count < 500)
                {
                    string Enc(string x) => Uri.EscapeDataString(x ?? "");
                    detailRows.Add(string.Join("|", r.GetInt64(0).ToString(), Enc(when),
                        Enc(r.GetString(1)), Enc(type), Enc(r.GetString(4)), Enc(r.GetString(5))));
                }
            }
            string hourly = string.Join(";", Enumerable.Range(0, hours.Length).Select(i =>
                $"{hours[i]},{hTotal[i]},{hPass[i]},{hFail[i]}"));
            // Daily equipment event aggregation, separate from production/NG counts.
            long estopCount = 0, recheckCount = 0, clearCount = 0, estopSeconds = 0, recheckSeconds = 0;
            long activeEstop = 0, activeRecheck = 0, yoloCount = 0, yoloSeconds = 0, activeYolo = 0;
            long estopReviewed = 0, recheckReviewed = 0, yoloReviewed = 0;
            long estopValidClosed = 0, recheckValidClosed = 0, yoloValidClosed = 0;
            // Historical replay can pair an old alert with a much later CLEAR.
            // Do not silently count such unverified downtime as real downtime.
            const long MaxVerifiedLegacyDurationSeconds = 600;
            using (SqliteConnection eventsConn = new(connectionString))
            {
                eventsConn.Open();
                using (SqliteCommand sessions = eventsConn.CreateCommand())
                {
                    // Sessions are the authoritative event count: repeated ALERT/LED/BUZZER logs do not inflate totals.
                    sessions.CommandText = @"SELECT UPPER(event_type), UPPER(status),
                        COALESCE(duration_seconds, 0) FROM equipment_event_sessions
                        WHERE datetime(started_at, '+9 hours') >= $start
                          AND datetime(started_at, '+9 hours') < $end";
                    sessions.Parameters.AddWithValue("$start", date + " 00:00:00");
                    sessions.Parameters.AddWithValue("$end", DateTime.ParseExact(date, "yyyy-MM-dd",
                        System.Globalization.CultureInfo.InvariantCulture).AddDays(1).ToString("yyyy-MM-dd") + " 00:00:00");
                    using SqliteDataReader reader = sessions.ExecuteReader();
                    while (reader.Read())
                    {
                        string eventType = reader.GetString(0);
                        string sessionStatus = reader.GetString(1);
                        bool active = sessionStatus == "ACTIVE";
                        bool cleared = sessionStatus == "CLEARED";
                        long duration = reader.GetInt64(2);
                        // A duration over ten minutes is retained in SQLite but excluded
                        // from the verified-duration figure until an operator reviews it.
                        bool review = cleared && duration > MaxVerifiedLegacyDurationSeconds;
                        bool valid = cleared && !review && duration >= 0;
                        if (eventType == "ESTOP") { estopCount++; if (active) activeEstop++; if (review) estopReviewed++; if (valid) { estopSeconds += duration; estopValidClosed++; } }
                        if (eventType == "RECHECK") { recheckCount++; if (active) activeRecheck++; if (review) recheckReviewed++; if (valid) { recheckSeconds += duration; recheckValidClosed++; } }
                        if (eventType == "YOLO_WARNING") { yoloCount++; if (active) activeYolo++; if (review) yoloReviewed++; if (valid) { yoloSeconds += duration; yoloValidClosed++; } }
                    }
                }
                // CLEAR is an action, not an incident: count only actual ESTOP/RECHECK/YOLO session closures.
                clearCount = estopCount - activeEstop + recheckCount - activeRecheck + yoloCount - activeYolo;
            }
            return JsonSerializer.Serialize(new
            {
                estop_review_count = estopReviewed,
                recheck_review_count = recheckReviewed,
                yolo_review_count = yoloReviewed,
                estop_valid_closed = estopValidClosed,
                recheck_valid_closed = recheckValidClosed,
                yolo_valid_closed = yoloValidClosed,
                type = "REPORT_INSPECTIONS_RESULT",
                status = "OK",
                date,
                total,
                passed,
                failed,
                wheel,
                tire,
                both,
                wrong,
                unknown,
                hourly,
                records = string.Join("\n", detailRows),
                estop_count = estopCount, recheck_count = recheckCount, clear_count = clearCount,
                estop_seconds = estopSeconds, recheck_seconds = recheckSeconds,
                active_estop = activeEstop, active_recheck = activeRecheck,
                yolo_count = yoloCount, yolo_seconds = yoloSeconds, active_yolo = activeYolo
            });
        }

        // 전체 누적 검사 통계 및 최근 검사 목록: 대시보드와 DB가 같은 수치를 사용합니다.
        // 불량 유형의 비율은 전체 검사 수가 아니라 전체 NG 수를 분모로 계산합니다.
        private static string GetDashboardStats()
        {
            long total = 0, passed = 0, failed = 0;
            long wheel = 0, tire = 0, both = 0, wrong = 0, unknown = 0;
            List<string> recent = new();

            using SqliteConnection conn = new(connectionString);
            conn.Open();
            using (SqliteCommand stats = conn.CreateCommand())
            {
                stats.CommandText = @"
                    SELECT
                        COUNT(*),
                        COALESCE(SUM(CASE WHEN UPPER(TRIM(result)) IN ('OK','PASS','NORMAL') THEN 1 ELSE 0 END), 0),
                        COALESCE(SUM(CASE WHEN UPPER(TRIM(result)) IN ('NG','FAIL','DEFECT') THEN 1 ELSE 0 END), 0),
                        COALESCE(SUM(CASE WHEN UPPER(TRIM(result)) IN ('NG','FAIL','DEFECT')
                            AND UPPER(TRIM(COALESCE(defect_type,''))) IN ('WHEEL_DAMAGE','WHEEL') THEN 1 ELSE 0 END), 0),
                        COALESCE(SUM(CASE WHEN UPPER(TRIM(result)) IN ('NG','FAIL','DEFECT')
                            AND UPPER(TRIM(COALESCE(defect_type,''))) IN ('TIRE_DAMAGE','TIRE','RUBBER_DAMAGE') THEN 1 ELSE 0 END), 0),
                        COALESCE(SUM(CASE WHEN UPPER(TRIM(result)) IN ('NG','FAIL','DEFECT')
                            AND UPPER(TRIM(COALESCE(defect_type,''))) IN ('BOTH_DAMAGE','BOTH','WHEEL_TIRE_DAMAGE') THEN 1 ELSE 0 END), 0),
                        COALESCE(SUM(CASE WHEN UPPER(TRIM(result)) IN ('NG','FAIL','DEFECT')
                            AND UPPER(TRIM(COALESCE(defect_type,''))) IN ('WRONG_TIRE','NG_WRONG_TIRE') THEN 1 ELSE 0 END), 0)
                    FROM inspection
                    WHERE data_source = 'HARDWARE' AND COALESCE(is_demo, 0) = 0;";
                using SqliteDataReader r = stats.ExecuteReader();
                if (r.Read())
                {
                    total = r.GetInt64(0);
                    passed = r.GetInt64(1);
                    failed = r.GetInt64(2);
                    wheel = r.GetInt64(3);
                    tire = r.GetInt64(4);
                    both = r.GetInt64(5);
                    wrong = r.GetInt64(6);
                }
            }
            unknown = Math.Max(0, failed - wheel - tire - both - wrong);

            using (SqliteCommand rows = conn.CreateCommand())
            {
                rows.CommandText = @"
                    SELECT COALESCE(NULLIF(inspection_time, ''),strftime('%Y-%m-%d %H:%M:%S', created_at, '+9 hours'),''),
                           COALESCE(tire_id, ''), COALESCE(result, ''),
                           COALESCE(defect_type, ''), COALESCE(defect_detail, ''),
                           COALESCE(is_demo, 0)
                    FROM inspection
                    WHERE data_source = 'HARDWARE' AND COALESCE(is_demo, 0) = 0
                    ORDER BY id DESC LIMIT 50;";
                using SqliteDataReader r = rows.ExecuteReader();
                while (r.Read())
                {
                    // 필드별 URL 인코딩으로 한글, 파이프 및 줄바꿈을 안전하게 전달합니다.
                    string Enc(string s) => Uri.EscapeDataString(s);
                    List<string> cols = Enumerable.Range(0, 5).Select(i => r.GetString(i)).ToList();
                    if (r.GetInt64(5) != 0)
                        cols[4] = "[시연] " + cols[4];
                    recent.Add(string.Join("|", cols.Select(Enc)));
                }
            }
            return JsonSerializer.Serialize(new
            {
                type = "DASHBOARD_STATS_RESULT",
                status = "OK",
                total,
                passed,
                failed,
                wheel,
                tire,
                both,
                wrong,
                unknown,
                other = Math.Max(0, total - passed - failed),
                recent = string.Join("\n", recent)
            });
        }

        // DB 연결 및 사용자 수 확인
        private static bool CheckDatabase()
        {
            try
            {
                using SqliteConnection conn =
                    new SqliteConnection(connectionString);

                conn.Open();

                string sql =
                    "SELECT COUNT(*) FROM users;";

                using SqliteCommand cmd =
                    new SqliteCommand(sql, conn);

                object? result =
                    cmd.ExecuteScalar();

                long count =
                    Convert.ToInt64(result);

                Console.WriteLine(
                    $"[DB] 등록 작업자 : {count}명"
                );

                return true;
            }
            catch (Exception ex)
            {
                Console.WriteLine("[DB ERROR]");
                Console.WriteLine(ex.Message);

                return false;
            }
        }

        // 등록 작업자 출력
        private static void ShowUsers()
        {
            try
            {
                using SqliteConnection conn =
                    new SqliteConnection(connectionString);

                conn.Open();

                string sql = @"
                    SELECT id, user_name
                    FROM users
                    ORDER BY id;
                ";

                using SqliteCommand cmd =
                    new SqliteCommand(sql, conn);

                using SqliteDataReader reader =
                    cmd.ExecuteReader();

                Console.WriteLine(
                    "========== 등록 작업자 =========="
                );

                while (reader.Read())
                {
                    long id =
                        reader.GetInt64(0);

                    string userName =
                        reader.GetString(1);

                    Console.WriteLine(
                        $"{id}. {userName}"
                    );
                }

                Console.WriteLine(
                    "================================="
                );
            }
            catch (Exception ex)
            {
                Console.WriteLine(
                    $"[USER ERROR] {ex.Message}"
                );
            }
        }

        // 클라이언트 연결 처리
        private static async Task HandleClient(TcpClient client)
        {
            string clientIp =
                client.Client.RemoteEndPoint?.ToString()
                ?? "UNKNOWN";

            ConnectedClient? connection = null;

            try
            {
                using (client)
                {
                    NetworkStream stream = client.GetStream();

                    using StreamReader reader = new StreamReader(
                        stream,
                        Encoding.UTF8,
                        false,
                        1024,
                        true
                    );

                    using StreamWriter writer = new StreamWriter(
                        stream,
                        new UTF8Encoding(false),
                        1024,
                        true
                    );

                    writer.AutoFlush = true;
                    writer.NewLine = "\n";

                    connection = new ConnectedClient(client, writer);
                    clients[connection.Id] = connection;

                    Console.WriteLine(
                        $"[CLIENTS] 현재 접속 수 : {clients.Count}"
                    );

                    while (client.Connected)
                    {
                        string? message = await reader.ReadLineAsync();

                        if (message == null)
                            break;

                        if (string.IsNullOrWhiteSpace(message))
                            continue;

                        Console.WriteLine();
                        Console.WriteLine($"[RECEIVE] {clientIp}");
                        Console.WriteLine(message);

                        string? response = await ProcessMessageAsync(message, connection);

                        if (!string.IsNullOrWhiteSpace(response))
                        {
                            await connection.SendLineAsync(response);
                        }
                    }
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine();
                Console.WriteLine($"[CLIENT ERROR] {clientIp}");
                Console.WriteLine(ex.Message);
            }
            finally
            {
                if (connection != null)
                {
                    clients.TryRemove(connection.Id, out _);
                }

                Console.WriteLine($"[DISCONNECT] {clientIp}");
                Console.WriteLine($"[CLIENTS] 현재 접속 수 : {clients.Count}");
            }
        }

        private static async Task<string?> ProcessMessageAsync(string message, ConnectedClient connection)
        {
            try
            {
                using JsonDocument document = JsonDocument.Parse(message);
                JsonElement root = document.RootElement;

                if (!root.TryGetProperty("type", out JsonElement typeElement))
                {
                    return JsonSerializer.Serialize(new
                    {
                        status = "ERROR",
                        message = "type 항목이 없습니다."
                    });
                }

                string type = typeElement.GetString()?.ToUpper() ?? "";

                switch (type)
                {
                    case "LOGIN":
                        return ProcessLogin(root);

                    case "INSPECTION":
                        {
                            string tireId = GetString(root, "tire_id");
                            string result = GetString(root, "result").Trim().ToUpperInvariant();
                            string source = GetNullableString(root, "data_source") ?? "";
                            bool hardware = source.Equals("HARDWARE", StringComparison.OrdinalIgnoreCase);
                            string eventId = GetNullableString(root, "event_id") ?? "";
                            if (hardware)
                            {
                                string expected = Environment.GetEnvironmentVariable("INSPECTION_INGEST_TOKEN") ?? "";
                                string received = GetNullableString(root, "ingest_token") ?? "";
                                if (expected.Length < 16 || !System.Security.Cryptography.CryptographicOperations.FixedTimeEquals(
                                        Encoding.UTF8.GetBytes(expected), Encoding.UTF8.GetBytes(received)))
                                    return JsonSerializer.Serialize(new { type = "INSPECTION_ACK", status = "ERROR",
                                        event_id = eventId, message = "실검사 전송 인증 토큰이 없거나 일치하지 않습니다." });
                                if (!Guid.TryParse(eventId, out _) || !tireId.StartsWith("TIRE-", StringComparison.Ordinal) ||
                                    (result != "OK" && result != "NG"))
                                    return JsonSerializer.Serialize(new { type = "INSPECTION_ACK", status = "ERROR",
                                        event_id = eventId, message = "실검사 고유 ID / 타이어 ID / 판정 값 오류" });
                            }
                            bool inserted = SaveInspection(root, hardware ? "HARDWARE" : "LEGACY_UNVERIFIED",
                                hardware ? eventId : null);
                            if (inserted)
                            {
                                string defectType = GetNullableString(root, "defect_type") ?? "";
                                string defectDetail = GetNullableString(root, "defect_detail") ?? "";
                                string imagePath = GetNullableString(root, "image_path") ?? "";
                                await BroadcastAsync(JsonSerializer.Serialize(new
                                {
                                    type = "INSPECTION", tire_id = tireId, result,
                                    defect_type = defectType, defect_detail = defectDetail,
                                    image_path = imagePath,
                                     yolo_result = GetNullableString(root, "yolo_result") ?? "",
                                     opencv_result = GetNullableString(root, "opencv_result") ?? "",
                                     inspection_time = GetNullableString(root, "inspection_time") ?? "",
                                     data_source = hardware ? "HARDWARE" : "LEGACY_UNVERIFIED"
                                }), hardware ? connection.Id : null);
                                Console.WriteLine($"[BROADCAST] INSPECTION : {tireId} / {result}");
                            }
                            return hardware ? JsonSerializer.Serialize(new
                            {
                                type = "INSPECTION_ACK", status = "OK", event_id = eventId,
                                outcome = inserted ? "SAVED" : "DUPLICATE"
                            }) : null;
                        }
                    case "DEMO_INSPECTION":
                        {
                            // 서버와 같은 PC의 관리자 UI에서만 시연 데이터 생성 허용.
                            // 원격 단말이 임의로 실적을 늘리지 못하도록 제한합니다.
                            IPEndPoint? remote = connection.TcpClient.Client.RemoteEndPoint as IPEndPoint;
                            IPEndPoint? local = connection.TcpClient.Client.LocalEndPoint as IPEndPoint;
                            if (remote == null || local == null ||
                                !(IPAddress.IsLoopback(remote.Address) || remote.Address.Equals(local.Address)))
                                return JsonSerializer.Serialize(new
                                {
                                    type = "DEMO_INSPECTION_RESULT",
                                    status = "ERROR",
                                    message = "시연 검사는 서버 PC에서만 추가할 수 있습니다."
                                });

                            string result = GetString(root, "result").Trim().ToUpperInvariant();
                            string defectType = NormalizeDefectType(GetNullableString(root, "defect_type") ?? "");
                            if (result != "OK" && result != "NG")
                                throw new Exception("시연 판정은 OK 또는 NG여야 합니다.");
                            if (result == "NG" && defectType.Length == 0)
                                throw new Exception("불량 유형이 지정되지 않았습니다.");

                            string detail = result == "OK" ? "정상 검사" : defectType switch
                            {
                                "WHEEL_DAMAGE" => "휠 손상 (시연)",
                                "TIRE_DAMAGE" => "타이어 고무 손상 (시연)",
                                "WRONG_TIRE" => "이종 타이어 (시연)",
                                _ => "휠 및 타이어 동시 손상 (시연)"
                            };
                            string tireId = "DEMO-" + DateTime.Now.ToString("yyyyMMdd-HHmmss-fff")
                                + "-" + Guid.NewGuid().ToString("N").Substring(0, 6);
                            string payload = JsonSerializer.Serialize(new
                            {
                                tire_id = tireId,
                                result,
                                defect_type = defectType,
                                defect_detail = detail,
                                image_path = "",
                                is_demo = true
                            });
                            using (JsonDocument demo = JsonDocument.Parse(payload))
                                SaveInspection(demo.RootElement);

                            await BroadcastAsync(JsonSerializer.Serialize(new
                            {
                                type = "INSPECTION",
                                tire_id = tireId,
                                result,
                                defect_type = defectType,
                                defect_detail = detail,
                                image_path = "",
                                is_demo = true
                            }));
                            return JsonSerializer.Serialize(new
                            {
                                type = "DEMO_INSPECTION_RESULT",
                                status = "OK",
                                tire_id = tireId
                            });
                        }

                    case "STATISTICS_RANGE":
                        return GetStatisticsRange(root);

                    case "DASHBOARD_STATS":
                        return GetDashboardStats();

                    case "REPORT_INSPECTIONS":
                        return GetInspectionReport(root);

                    case "HEARTBEAT":
                        {
                            await BroadcastAsync(message);
                            Console.WriteLine("[BROADCAST] HEARTBEAT : RASPBERRY_PI ALIVE");
                            return null;
                        }
                    case "EQUIPMENT":
                        {
                            SaveEquipment(root);

                            string equipment = GetString(root, "equipment");
                            string status = GetString(root, "status");
                            string eventMessage = GetNullableString(root, "message") ?? "";

                            string broadcast = JsonSerializer.Serialize(new
                            {
                                type = "EQUIPMENT",
                                equipment,
                                status,
                                message = eventMessage
                            });

                            await BroadcastAsync(broadcast);
                            Console.WriteLine($"[BROADCAST] EQUIPMENT : {equipment} / {status}");
                            return null;
                        }

                    case "EQUIPMENT_EVENT_STATS":
                        return GetEquipmentEventStats(root);

                    case "CHAT_HISTORY":
                        {
                            await SendChatHistoryAsync(connection);
                            return null;
                        }

                    case "CHAT":
                        {
                            string userName = GetString(root, "user_name");
                            string role = GetString(root, "role");
                            string chatMessage = GetString(root, "message");

                            string createdAt = SaveChat(root);

                            string broadcast = JsonSerializer.Serialize(new
                            {
                                type = "CHAT",
                                user_name = userName,
                                role,
                                message = chatMessage,
                                created_at = createdAt,
                                history = false
                            });

                            await BroadcastAsync(broadcast);

                            Console.WriteLine(
                                $"[BROADCAST] CHAT : {userName} / {clients.Count}명"
                            );

                            return null;
                        }

                    case "HANDOVER_SHIFT_STATS":
                        return GetHandoverShiftStats(root);

                    case "HANDOVER":
                        {
                            long id = SaveHandover(root);
                            return JsonSerializer.Serialize(new
                            {
                                type = "HANDOVER_RESULT",
                                status = "OK",
                                id
                            });
                        }

                    case "HANDOVER_LIST":
                        {
                            await SendHandoverListAsync(connection);
                            return null;
                        }

                    case "HANDOVER_CHECK":
                        {
                            long id = GetInt64(root, "id");
                            string checkedBy = GetString(root, "checked_by");
                            ConfirmHandover(id, checkedBy);

                            return JsonSerializer.Serialize(new
                            {
                                type = "HANDOVER_CHECK_RESULT",
                                status = "OK",
                                id
                            });
                        }

                    default:
                        return JsonSerializer.Serialize(new
                        {
                            status = "ERROR",
                            message = $"알 수 없는 TYPE : {type}"
                        });
                }

                return JsonSerializer.Serialize(new
                {
                    status = "OK"
                });
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[PROCESS ERROR] {ex.Message}");

                return JsonSerializer.Serialize(new
                {
                    status = "ERROR",
                    message = ex.Message
                });
            }
        }

        private static async Task SendChatHistoryAsync(ConnectedClient connection)
        {
            var history = new List<(string UserName, string Role, string Message, string CreatedAt)>();

            using SqliteConnection conn =
                new SqliteConnection(connectionString);

            conn.Open();

            string sql = @"
                SELECT
                    c.user_name,
                    COALESCE(u.role, ''),
                    c.message,
                    strftime('%Y-%m-%d %H:%M:%S', c.created_at, 'localtime')
                FROM chat c
                LEFT JOIN users u
                    ON u.user_name = c.user_name
                ORDER BY c.id DESC
                LIMIT 100;
            ";

            using SqliteCommand cmd =
                new SqliteCommand(sql, conn);

            using SqliteDataReader reader =
                cmd.ExecuteReader();

            while (reader.Read())
            {
                history.Add(
                    (
                        reader.IsDBNull(0) ? "" : reader.GetString(0),
                        reader.IsDBNull(1) ? "" : reader.GetString(1),
                        reader.IsDBNull(2) ? "" : reader.GetString(2),
                        reader.IsDBNull(3) ? "" : reader.GetString(3)
                    )
                );
            }

            history.Reverse();

            foreach (var item in history)
            {
                string json =
                    JsonSerializer.Serialize(new
                    {
                        type = "CHAT",
                        user_name = item.UserName,
                        role = item.Role,
                        message = item.Message,
                        created_at = item.CreatedAt,
                        history = true
                    });

                await connection.SendLineAsync(json);
            }

            await connection.SendLineAsync(
                JsonSerializer.Serialize(new
                {
                    type = "CHAT_HISTORY_END",
                    count = history.Count
                })
            );

            Console.WriteLine(
                $"[CHAT HISTORY] {history.Count}건 전송"
            );
        }

        private static async Task BroadcastAsync(string message, Guid? excludeClientId = null)
        {
            ConnectedClient[] snapshot = clients.Values.ToArray();

            foreach (ConnectedClient target in snapshot)
            {
                if (excludeClientId.HasValue && target.Id == excludeClientId.Value)
                    continue;
                try
                {
                    await target.SendLineAsync(message);
                }
                catch (Exception ex)
                {
                    Console.WriteLine(
                        $"[BROADCAST ERROR] {ex.Message}"
                    );

                    clients.TryRemove(target.Id, out _);

                    try
                    {
                        target.TcpClient.Close();
                    }
                    catch
                    {
                    }
                }
            }
        }

        private static string ProcessLogin(
    JsonElement data)
        {
            string loginId =
                GetString(
                    data,
                    "login_id"
                );

            string password =
                GetString(
                    data,
                    "password"
                );

            using SqliteConnection conn =
                new SqliteConnection(
                    connectionString
                );

            conn.Open();

            string sql = @"
        SELECT
            user_name,
            role,
            fingerprint_id
        FROM users
        WHERE login_id = $login_id
          AND password_hash = $password;
    ";

            using SqliteCommand cmd =
                new SqliteCommand(
                    sql,
                    conn
                );

            cmd.Parameters.AddWithValue(
                "$login_id",
                loginId
            );

            cmd.Parameters.AddWithValue(
                "$password",
                password
            );

            using SqliteDataReader reader =
                cmd.ExecuteReader();

            if (reader.Read())
            {
                string userName =
                    reader.GetString(0);

                string role =
                    reader.GetString(1).ToUpperInvariant();

                int fingerprintId =
                    reader.GetInt32(2);

                Console.WriteLine(
                    $"[LOGIN] 성공 : {userName} / {role}"
                );

                return JsonSerializer.Serialize(new
                {
                    status = "OK",
                    user_name = userName,
                    role = role,
                    fingerprint_id = fingerprintId
                });
            }

            Console.WriteLine(
                $"[LOGIN] 실패 : {loginId}"
            );

            return JsonSerializer.Serialize(new
            {
                status = "FAIL",
                message = "아이디 또는 비밀번호가 일치하지 않습니다."
            });
        }

        // 검사 결과 저장
        private static bool SaveInspection(
            JsonElement data, string forcedSource = "DEMO", string? eventId = null)
        {
            string tireId =
                GetString(
                    data,
                    "tire_id"
                );

            string result =
                GetString(
                    data,
                    "result"
                );

            string? defectType =
                GetNullableString(
                    data,
                    "defect_type"
                );

            string? defectDetail = GetNullableString(data, "defect_detail");

            string? imagePath =
                GetNullableString(
                    data,
                    "image_path"
                );

            bool isDemo = forcedSource == "DEMO" || (data.TryGetProperty("is_demo", out JsonElement demoValue)
                && demoValue.ValueKind == JsonValueKind.True);
            if (forcedSource == "HARDWARE" && isDemo)
                throw new Exception("실검사 데이터는 is_demo=true가 될 수 없습니다.");

            // 알려진 코드만 통일하며, 판단하지 못한 기존 값은 임의로 분류하지 않습니다.
            string normalized = NormalizeDefectType(defectType ?? "");
            if (normalized.Length > 0)
                defectType = normalized;
            if (string.Equals(result, "OK", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(result, "PASS", StringComparison.OrdinalIgnoreCase))
            {
                defectType = null;
                defectDetail = null;
            }

            if (string.IsNullOrWhiteSpace(tireId))
            {
                throw new Exception(
                    "tire_id 값이 없습니다."
                );
            }

            if (string.IsNullOrWhiteSpace(result))
            {
                throw new Exception(
                    "result 값이 없습니다."
                );
            }

            using SqliteConnection conn =
                new SqliteConnection(connectionString);

            conn.Open();

            string sql = @"
                INSERT INTO inspection
                (
                    tire_id,
                    result,
                    defect_type,
                    defect_detail,
                    image_path,
                    is_demo,
                    data_source,
                    event_id,
                    yolo_result,
                    opencv_result,
                    yolo_confidence,
                    inspection_time,
                    created_at
                )
                VALUES
                (
                    $tire_id,
                    $result,
                    $defect_type,
                    $defect_detail,
                    $image_path,
                    $is_demo,
                    $data_source,
                    $event_id,
                    $yolo_result,
                    $opencv_result,
                    $yolo_confidence,
                    $inspection_time,
                    CURRENT_TIMESTAMP
                ) ON CONFLICT DO NOTHING;
            ";

            using SqliteCommand cmd =
                new SqliteCommand(sql, conn);

            cmd.Parameters.AddWithValue(
                "$tire_id",
                tireId
            );

            cmd.Parameters.AddWithValue(
                "$result",
                result
            );

            cmd.Parameters.AddWithValue(
                "$defect_type",
                (object?)defectType
                ?? DBNull.Value
            );

            cmd.Parameters.AddWithValue("$defect_detail", (object?)defectDetail ?? DBNull.Value);

            cmd.Parameters.AddWithValue(
                "$image_path",
                (object?)imagePath
                ?? DBNull.Value
            );

            cmd.Parameters.AddWithValue("$is_demo", isDemo ? 1 : 0);
            cmd.Parameters.AddWithValue("$data_source", forcedSource);
            cmd.Parameters.AddWithValue("$event_id", (object?)eventId ?? DBNull.Value);
                        cmd.Parameters.AddWithValue(
                "$yolo_result",
                (object?)GetNullableString(data, "yolo_result")
                    ?? DBNull.Value);

            cmd.Parameters.AddWithValue(
                "$opencv_result",
                (object?)GetNullableString(data, "opencv_result")
                    ?? DBNull.Value);

            double confidence = 0;
            if (data.TryGetProperty("yolo_confidence", out var confidenceElement)
                && confidenceElement.ValueKind == JsonValueKind.Number)
                confidence = confidenceElement.GetDouble();

            cmd.Parameters.AddWithValue("$yolo_confidence", confidence);

            cmd.Parameters.AddWithValue(
                "$inspection_time",
                (object?)GetNullableString(data, "inspection_time")
                    ?? DBNull.Value);
            bool inserted = cmd.ExecuteNonQuery() == 1;
            // A duplicate stable event_id/tire_id is acknowledged, not counted twice.
            if (!inserted && forcedSource == "HARDWARE")
            {
                using SqliteCommand check = conn.CreateCommand();
                check.CommandText = @"SELECT COUNT(*) FROM inspection
                    WHERE data_source = 'HARDWARE' AND tire_id = $tire
                    AND event_id = $event AND UPPER(TRIM(result)) = $result
                    AND COALESCE(defect_type, '') = $defect
                    AND COALESCE(defect_detail, '') = $detail
                    AND COALESCE(image_path, '') = $image;";
                check.Parameters.AddWithValue("$tire", tireId);
                check.Parameters.AddWithValue("$event", eventId ?? "");
                check.Parameters.AddWithValue("$result", result.Trim().ToUpperInvariant());
                check.Parameters.AddWithValue("$defect", defectType ?? "");
                check.Parameters.AddWithValue("$detail", defectDetail ?? "");
                check.Parameters.AddWithValue("$image", imagePath ?? "");
                if (Convert.ToInt64(check.ExecuteScalar()) != 1)
                    throw new Exception("검사 고유 ID 충돌 또는 서로 다른 검사 결과입니다.");
            }

            Console.WriteLine($"[DB] 검사 {(inserted ? "저장" : "중복 무시")} : {tireId} / {result}");
            return inserted;
        }

        // 인수인계 생산 통계: SQLite의 CURRENT_TIMESTAMP(UTC)를 한국 시간으로 변환.
        // 근무별 범위: AM [09:00,12:00), PM [13:00,18:00).
        private static string GetHandoverShiftStats(JsonElement data)
        {
            string date = GetString(data, "work_date");
            string shift = GetString(data, "shift_type").ToUpperInvariant();
            if (!System.Text.RegularExpressions.Regex.IsMatch(date, @"^\d{4}-\d{2}-\d{2}$") ||
                !DateTime.TryParseExact(date, "yyyy-MM-dd", System.Globalization.CultureInfo.InvariantCulture,
                    System.Globalization.DateTimeStyles.None, out _))
                throw new Exception("work_date 형식은 yyyy-MM-dd여야 합니다.");
            if (shift != "AM" && shift != "PM")
                throw new Exception("shift_type은 AM 또는 PM이어야 합니다.");

            string start = date + (shift == "AM" ? " 09:00:00" : " 13:00:00");
            string end = date + (shift == "AM" ? " 12:00:00" : " 18:00:00");
            using SqliteConnection conn = new(connectionString);
            conn.Open();
            using SqliteCommand cmd = conn.CreateCommand();
            cmd.CommandText = @"
                SELECT COUNT(*),
                       COALESCE(SUM(CASE WHEN UPPER(TRIM(result)) IN ('OK','PASS','NORMAL') THEN 1 ELSE 0 END), 0),
                       COALESCE(SUM(CASE WHEN UPPER(TRIM(result)) IN ('NG','FAIL','DEFECT') THEN 1 ELSE 0 END), 0)
                  FROM inspection
                 WHERE datetime(created_at, '+9 hours') >= $start
                   AND datetime(created_at, '+9 hours') < $end
                   AND data_source = 'HARDWARE' AND COALESCE(is_demo, 0) = 0";
            cmd.Parameters.AddWithValue("$start", start);
            cmd.Parameters.AddWithValue("$end", end);
            using SqliteDataReader reader = cmd.ExecuteReader();
            reader.Read();
            long total = reader.GetInt64(0);
            long passed = reader.GetInt64(1);
            long failed = reader.GetInt64(2);
            // 알 수 없는 판정이 있으면 수치를 왜곡하지 않고 별도 표시.
            return JsonSerializer.Serialize(new
            {
                type = "HANDOVER_SHIFT_STATS_RESULT",
                status = "OK",
                work_date = date,
                shift_type = shift,
                total,
                passed,
                failed,
                other = total - passed - failed
            });
        }

        // 설비 상태 및 이벤트 저장
        private static void SaveEquipment(
            JsonElement data)
        {
            string equipment =
                GetString(
                    data,
                    "equipment"
                );

            string status =
                GetString(
                    data,
                    "status"
                );

            string? message =
                GetNullableString(
                    data,
                    "message"
                );

            if (string.IsNullOrWhiteSpace(equipment))
            {
                throw new Exception(
                    "equipment 값이 없습니다."
                );
            }

            if (string.IsNullOrWhiteSpace(status))
            {
                throw new Exception(
                    "status 값이 없습니다."
                );
            }

            using SqliteConnection conn =
                new SqliteConnection(connectionString);

            conn.Open();

            string sql = @"
                INSERT INTO equipment_log
                (
                    equipment,
                    status,
                    message
                )
                VALUES
                (
                    $equipment,
                    $status,
                    $message
                );
            ";

            using SqliteCommand cmd =
                new SqliteCommand(sql, conn);

            cmd.Parameters.AddWithValue(
                "$equipment",
                equipment
            );

            cmd.Parameters.AddWithValue(
                "$status",
                status
            );

            cmd.Parameters.AddWithValue(
                "$message",
                (object?)message
                ?? DBNull.Value
            );

            cmd.ExecuteNonQuery();
            UpdateEventSession(conn, equipment, status, message ?? "");

            Console.WriteLine(
                $"[DB] 설비 저장 : {equipment} / {status}"
            );
        }

        // Event sessions use server-side UTC timestamps; equipment_log remains untouched.
        private static void EnsureEventSessions(SqliteConnection conn)
        {
            using var cmd = conn.CreateCommand();
            cmd.CommandText = @"CREATE TABLE IF NOT EXISTS equipment_event_sessions (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                event_type TEXT NOT NULL,
                started_at TEXT NOT NULL,
                ended_at TEXT,
                duration_seconds INTEGER,
                reason TEXT,
                status TEXT NOT NULL CHECK(status IN ('ACTIVE','CLEARED'))
            );
            CREATE UNIQUE INDEX IF NOT EXISTS ux_equipment_event_active
                ON equipment_event_sessions(event_type) WHERE status='ACTIVE';
            CREATE INDEX IF NOT EXISTS ix_equipment_event_started
                ON equipment_event_sessions(started_at, event_type);";
            cmd.ExecuteNonQuery();
        }

        // One-time, idempotent recovery of legacy equipment_log incidents.
        // Logs use SQLite CURRENT_TIMESTAMP (UTC), just like new sessions.
        // Existing sessions are never rewritten; a marker prevents replay after restart.
        private static void BackfillLegacyEquipmentSessions(SqliteConnection conn)
        {
            using var tx = conn.BeginTransaction();
            using (var setup = conn.CreateCommand())
            {
                setup.Transaction = tx;
                setup.CommandText = @"CREATE TABLE IF NOT EXISTS equipment_session_migrations
                    (migration_key TEXT PRIMARY KEY, applied_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);";
                setup.ExecuteNonQuery();
                setup.CommandText = "SELECT COUNT(*) FROM equipment_session_migrations WHERE migration_key='legacy_log_v1'";
                if (Convert.ToInt64(setup.ExecuteScalar()) != 0) { tx.Commit(); return; }
                setup.CommandText = "SELECT COUNT(*) FROM equipment_event_sessions";
                if (Convert.ToInt64(setup.ExecuteScalar()) != 0)
                {
                    // Do not risk overlapping reconstructed and live sessions.
                    Console.WriteLine("[EVENT BACKFILL] Existing sessions detected; historical replay skipped.");
                    setup.CommandText = "INSERT INTO equipment_session_migrations(migration_key) VALUES('legacy_log_v1')";
                    setup.ExecuteNonQuery();
                    tx.Commit();
                    return;
                }
            }

            var open = new Dictionary<string, (string Started, string Reason)>();
            long restored = 0;
            var legacyRows = new List<(string Equipment, string State, string Message, string At)>();
            using (var read = conn.CreateCommand())
            {
                read.Transaction = tx;
                read.CommandText = @"SELECT equipment,status,COALESCE(message,''),created_at
                    FROM equipment_log ORDER BY datetime(created_at),id";
                using var reader = read.ExecuteReader();
                while (reader.Read())
                    legacyRows.Add((reader.GetString(0), reader.GetString(1), reader.GetString(2), reader.GetString(3)));
            }
            foreach (var row in legacyRows)
            {
                    string eq = row.Equipment.Trim().ToUpperInvariant();
                    string state = row.State.Trim().ToUpperInvariant();
                    string msg = row.Message;
                    string at = row.At;
                    string type = "";
                    bool start = false, clear = false;
                    bool primary = eq is "ALERT" or "CONVEYOR" or "SYSTEM" or "CONTROLLER" or "EMERGENCY_STOP" or "ESTOP";
                    if (primary && state == "ESTOP") { type = "ESTOP"; start = true; }
                    else if (primary && state == "RECHECK") { type = "RECHECK"; start = true; }
                    else if (primary && (state == "YOLO_WARNING" || state == "YOLO_ALERT")) { type = "YOLO_WARNING"; start = true; }
                    else if (primary && state == "CLEAR")
                    {
                        if (msg.Contains("비상정지")) { type = "ESTOP"; clear = true; }
                        else if (msg.Contains("재확인")) { type = "RECHECK"; clear = true; }
                        else if (msg.Contains("YOLO", StringComparison.OrdinalIgnoreCase)) { type = "YOLO_WARNING"; clear = true; }
                        // Ambiguous CLEAR is not used to guess which incident ended.
                    }
                    if (start && !open.ContainsKey(type)) open[type] = (at, msg);
                    if (clear && open.TryGetValue(type, out var entry))
                    {
                        using var insert = conn.CreateCommand();
                        insert.Transaction = tx;
                        insert.CommandText = @"INSERT INTO equipment_event_sessions
                            (event_type,started_at,ended_at,duration_seconds,reason,status)
                            VALUES($type,$start,$end,MAX(0,CAST(strftime('%s',$end) AS INTEGER)-CAST(strftime('%s',$start) AS INTEGER)),$reason,'CLEARED')";
                        insert.Parameters.AddWithValue("$type", type);
                        insert.Parameters.AddWithValue("$start", entry.Started);
                        insert.Parameters.AddWithValue("$end", at);
                        insert.Parameters.AddWithValue("$reason", entry.Reason);
                        insert.ExecuteNonQuery();
                        restored++;
                        open.Remove(type);
                    }
            }
            // A legacy warning without a matching CLEAR remains ACTIVE, never fabricated as cleared.
            foreach (var entry in open)
            {
                using var insert = conn.CreateCommand();
                insert.Transaction = tx;
                insert.CommandText = @"INSERT OR IGNORE INTO equipment_event_sessions
                    (event_type,started_at,reason,status) VALUES($type,$start,$reason,'ACTIVE')";
                insert.Parameters.AddWithValue("$type", entry.Key);
                insert.Parameters.AddWithValue("$start", entry.Value.Started);
                insert.Parameters.AddWithValue("$reason", entry.Value.Reason);
                restored += insert.ExecuteNonQuery();
            }
            using (var done = conn.CreateCommand())
            {
                done.Transaction = tx;
                done.CommandText = "INSERT INTO equipment_session_migrations(migration_key) VALUES('legacy_log_v1')";
                done.ExecuteNonQuery();
            }
            tx.Commit();
            Console.WriteLine($"[EVENT BACKFILL] Restored {restored} sessions from equipment_log.");
        }

        private static readonly object eventSessionLock = new();

        private static void UpdateEventSession(SqliteConnection conn, string equipment, string status, string message)
        {
            string eq = equipment.Trim().ToUpperInvariant();
            string state = status.Trim().ToUpperInvariant();
            string eventType = "";
            bool? activate = null;
            // ESTOP is global. Only a controller event or an explicit ESTOP message is authoritative.
            // LED/BUZZER notifications must not start or end sessions.
            bool controller = eq is "CONVEYOR" or "SYSTEM" or "CONTROLLER" or "EMERGENCY_STOP" or "ESTOP";
            if ((controller || eq == "ALERT") && state == "ESTOP") { eventType = "ESTOP"; activate = true; }
            else if ((controller || eq == "ALERT") && state == "CLEAR" && message.Contains("비상정지 해제"))
                { eventType = "ESTOP"; activate = false; }
            else if (state == "RECHECK" && (controller || eq == "ALERT"))
                { eventType = "RECHECK"; activate = true; }
            else if (state == "CLEAR" && message.Contains("재확인") && (controller || eq == "ALERT"))
                { eventType = "RECHECK"; activate = false; }
            else if ((state == "YOLO_WARNING" || state == "YOLO_ALERT") && (controller || eq == "ALERT"))
                { eventType = "YOLO_WARNING"; activate = true; }
            else if (state == "CLEAR" && message.Contains("YOLO", StringComparison.OrdinalIgnoreCase) && (controller || eq == "ALERT"))
                { eventType = "YOLO_WARNING"; activate = false; }
            if (activate == null) return;
            lock (eventSessionLock)
            {
                using var tx = conn.BeginTransaction();
                using var cmd = conn.CreateCommand();
                cmd.Transaction = tx;
                if (activate.Value)
                {
                    cmd.CommandText = @"INSERT OR IGNORE INTO equipment_event_sessions
                        (event_type,started_at,reason,status)
                        VALUES ($type,strftime('%Y-%m-%d %H:%M:%S','now'),$reason,'ACTIVE');";
                    cmd.Parameters.AddWithValue("$reason", message);
                }
                else
                {
                    cmd.CommandText = @"UPDATE equipment_event_sessions
                        SET ended_at=strftime('%Y-%m-%d %H:%M:%S','now'),
                            duration_seconds=MAX(0,CAST(strftime('%s','now') AS INTEGER)-CAST(strftime('%s',started_at) AS INTEGER)),
                            status='CLEARED'
                        WHERE event_type=$type AND status='ACTIVE';";
                }
                cmd.Parameters.AddWithValue("$type", eventType);
                cmd.ExecuteNonQuery();
                tx.Commit();
            }
        }

        private static string GetEquipmentEventStats(JsonElement request)
        {
            string start = GetString(request, "start_date");
            string end = GetString(request, "end_date");
            var culture = System.Globalization.CultureInfo.InvariantCulture;
            if (!DateTime.TryParseExact(start, "yyyy-MM-dd", culture,
                System.Globalization.DateTimeStyles.None, out var startDate) ||
                !DateTime.TryParseExact(end, "yyyy-MM-dd", culture,
                System.Globalization.DateTimeStyles.None, out var endDate) ||
                startDate > endDate || (endDate-startDate).TotalDays > 366)
                throw new Exception("조회 기간 오류 (최대 366일)");
            using var conn = new SqliteConnection(connectionString);
            conn.Open();
            using var cmd = conn.CreateCommand();
            cmd.CommandText = @"SELECT event_type,COUNT(*),
                COALESCE(SUM(CASE WHEN status='CLEARED' AND duration_seconds BETWEEN 0 AND 600 THEN duration_seconds ELSE 0 END),0),
                SUM(CASE WHEN status='ACTIVE' THEN 1 ELSE 0 END),
                SUM(CASE WHEN status='CLEARED' AND duration_seconds > 600 THEN 1 ELSE 0 END),
                SUM(CASE WHEN status='CLEARED' AND duration_seconds BETWEEN 0 AND 600 THEN 1 ELSE 0 END)
                FROM equipment_event_sessions
                WHERE datetime(started_at,'+9 hours') >= $start
                  AND datetime(started_at,'+9 hours') < $end
                GROUP BY event_type;";
            cmd.Parameters.AddWithValue("$start", start + " 00:00:00");
            cmd.Parameters.AddWithValue("$end", endDate.AddDays(1).ToString("yyyy-MM-dd") + " 00:00:00");
            var rows = new List<object>();
            using var reader = cmd.ExecuteReader();
            while (reader.Read())
                rows.Add(new { event_type=reader.GetString(0), count=reader.GetInt64(1),
                    total_duration_seconds=reader.GetInt64(2), active_count=reader.GetInt64(3), review_count=reader.GetInt64(4),
                    average_duration_seconds=reader.GetInt64(5) > 0 ?
                        (double)reader.GetInt64(2)/reader.GetInt64(5) : 0 });
            return JsonSerializer.Serialize(new { type="EQUIPMENT_EVENT_STATS_RESULT", status="OK", start_date=start, end_date=end, events=rows });
        }

        // 채팅 저장
        private static string SaveChat(
            JsonElement data)
        {
            string userName =
                GetString(
                    data,
                    "user_name"
                );

            string message =
                GetString(
                    data,
                    "message"
                );

            if (string.IsNullOrWhiteSpace(userName))
            {
                throw new Exception(
                    "user_name 값이 없습니다."
                );
            }

            if (string.IsNullOrWhiteSpace(message))
            {
                throw new Exception(
                    "message 값이 없습니다."
                );
            }

            using SqliteConnection conn =
                new SqliteConnection(connectionString);

            conn.Open();

            string sql = @"
                INSERT INTO chat
                (
                    user_name,
                    message
                )
                VALUES
                (
                    $user_name,
                    $message
                );
            ";

            using SqliteCommand cmd =
                new SqliteCommand(sql, conn);

            cmd.Parameters.AddWithValue(
                "$user_name",
                userName
            );

            cmd.Parameters.AddWithValue(
                "$message",
                message
            );

            cmd.ExecuteNonQuery();

            string createdAt =
                DateTime.Now.ToString(
                    "yyyy-MM-dd HH:mm:ss"
                );

            Console.WriteLine(
                $"[DB] 채팅 저장 : {userName}"
            );

            return createdAt;
        }

        // 인수인계 저장
        private static long SaveHandover(JsonElement data)
        {
            string workDate = GetString(data, "work_date");
            string shiftType = GetString(data, "shift_type").ToUpperInvariant();
            string priority = GetString(data, "priority").ToUpperInvariant();
            string userName = GetString(data, "user_name");
            string message = GetString(data, "message");

            if (string.IsNullOrWhiteSpace(workDate))
                workDate = DateTime.Now.ToString("yyyy-MM-dd");

            if (shiftType != "AM" && shiftType != "PM")
                throw new Exception("shift_type은 AM 또는 PM이어야 합니다.");

            if (priority != "URGENT" && priority != "WARNING" && priority != "NORMAL")
                priority = "NORMAL";

            if (string.IsNullOrWhiteSpace(userName))
                throw new Exception("user_name 값이 없습니다.");

            if (string.IsNullOrWhiteSpace(message))
                throw new Exception("message 값이 없습니다.");

            using SqliteConnection conn = new SqliteConnection(connectionString);
            conn.Open();

            string sql = @"
                INSERT INTO handover
                (
                    work_date,
                    shift_type,
                    priority,
                    user_name,
                    message,
                    checked,
                    checked_by,
                    checked_at,
                    created_at
                )
                VALUES
                (
                    $work_date,
                    $shift_type,
                    $priority,
                    $user_name,
                    $message,
                    0,
                    NULL,
                    NULL,
                    CURRENT_TIMESTAMP
                );
            ";

            using SqliteCommand cmd = new SqliteCommand(sql, conn);
            cmd.Parameters.AddWithValue("$work_date", workDate);
            cmd.Parameters.AddWithValue("$shift_type", shiftType);
            cmd.Parameters.AddWithValue("$priority", priority);
            cmd.Parameters.AddWithValue("$user_name", userName);
            cmd.Parameters.AddWithValue("$message", message);

            cmd.ExecuteNonQuery();

            using SqliteCommand idCommand =
                new SqliteCommand("SELECT last_insert_rowid();", conn);
            long id = Convert.ToInt64(idCommand.ExecuteScalar());

            Console.WriteLine($"[DB] 인수인계 저장 : {userName} / #{id}");
            return id;
        }

        private static async Task SendHandoverListAsync(ConnectedClient connection)
        {
            using SqliteConnection conn = new SqliteConnection(connectionString);
            conn.Open();

            string sql = @"
                SELECT
                    id,
                    COALESCE(work_date, date(created_at)),
                    COALESCE(shift_type, ''),
                    COALESCE(priority, 'NORMAL'),
                    user_name,
                    message,
                    COALESCE(checked, 0),
                    COALESCE(checked_by, ''),
                    COALESCE(strftime('%Y-%m-%d %H:%M:%S', checked_at, 'localtime'), ''),
                    COALESCE(strftime('%Y-%m-%d %H:%M:%S', created_at, 'localtime'), '')
                FROM handover
                ORDER BY id DESC
                LIMIT 100;
            ";

            using SqliteCommand cmd = new SqliteCommand(sql, conn);
            using SqliteDataReader reader = cmd.ExecuteReader();

            int count = 0;
            while (reader.Read())
            {
                string json = JsonSerializer.Serialize(new
                {
                    type = "HANDOVER_ITEM",
                    id = reader.GetInt64(0),
                    work_date = reader.IsDBNull(1) ? "" : reader.GetString(1),
                    shift_type = reader.IsDBNull(2) ? "" : reader.GetString(2),
                    priority = reader.IsDBNull(3) ? "NORMAL" : reader.GetString(3),
                    user_name = reader.IsDBNull(4) ? "" : reader.GetString(4),
                    message = reader.IsDBNull(5) ? "" : reader.GetString(5),
                    @checked = !reader.IsDBNull(6) && reader.GetInt64(6) != 0,
                    checked_by = reader.IsDBNull(7) ? "" : reader.GetString(7),
                    checked_at = reader.IsDBNull(8) ? "" : reader.GetString(8),
                    created_at = reader.IsDBNull(9) ? "" : reader.GetString(9)
                });

                await connection.SendLineAsync(json);
                count++;
            }

            await connection.SendLineAsync(JsonSerializer.Serialize(new
            {
                type = "HANDOVER_LIST_END",
                count
            }));

            Console.WriteLine($"[HANDOVER LIST] {count}건 전송");
        }

        private static void ConfirmHandover(long id, string checkedBy)
        {
            if (id <= 0)
                throw new Exception("유효하지 않은 인수인계 id입니다.");

            if (string.IsNullOrWhiteSpace(checkedBy))
                throw new Exception("checked_by 값이 없습니다.");

            using SqliteConnection conn = new SqliteConnection(connectionString);
            conn.Open();

            string sql = @"
                UPDATE handover
                SET
                    checked = 1,
                    checked_by = $checked_by,
                    checked_at = CURRENT_TIMESTAMP
                WHERE id = $id;
            ";

            using SqliteCommand cmd = new SqliteCommand(sql, conn);
            cmd.Parameters.AddWithValue("$checked_by", checkedBy);
            cmd.Parameters.AddWithValue("$id", id);

            int changed = cmd.ExecuteNonQuery();
            if (changed == 0)
                throw new Exception("해당 인수인계를 찾을 수 없습니다.");

            Console.WriteLine($"[DB] 인수인계 확인 완료 : #{id} / {checkedBy}");
        }

        private static long GetInt64(JsonElement data, string propertyName)
        {
            if (!data.TryGetProperty(propertyName, out JsonElement value))
                return 0;

            if (value.ValueKind == JsonValueKind.Number && value.TryGetInt64(out long number))
                return number;

            if (value.ValueKind == JsonValueKind.String &&
                long.TryParse(value.GetString(), out long parsed))
                return parsed;

            return 0;
        }

        private static string GetString(
            JsonElement data,
            string propertyName)
        {
            if (!data.TryGetProperty(
                    propertyName,
                    out JsonElement value))
            {
                return "";
            }

            if (value.ValueKind ==
                JsonValueKind.Null)
            {
                return "";
            }

            return value.GetString() ?? "";
        }

        private static string? GetNullableString(
            JsonElement data,
            string propertyName)
        {
            if (!data.TryGetProperty(
                    propertyName,
                    out JsonElement value))
            {
                return null;
            }

            if (value.ValueKind ==
                JsonValueKind.Null)
            {
                return null;
            }

            string? result =
                value.GetString();

            if (string.IsNullOrWhiteSpace(result))
            {
                return null;
            }

            return result;
        }
    }
}