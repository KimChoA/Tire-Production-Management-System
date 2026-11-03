using System.Text.Json;

namespace TireEquipmentHMI;

internal sealed class AppConfig
{
    private const string ConfigFileName = "client_config.json";

    public string ServerIp { get; init; } = "192.168.0.104";
    public int ServerPort { get; init; } = 5000;

    public static AppConfig Current { get; } = Load();

    private static AppConfig Load()
    {
        string path = Path.Combine(AppContext.BaseDirectory, ConfigFileName);

        try
        {
            if (!File.Exists(path))
                return new AppConfig();

            AppConfig? config = JsonSerializer.Deserialize<AppConfig>(
                File.ReadAllText(path),
                new JsonSerializerOptions { PropertyNameCaseInsensitive = true });

            if (config is null ||
                string.IsNullOrWhiteSpace(config.ServerIp) ||
                config.ServerPort is < 1 or > 65535)
            {
                return new AppConfig();
            }

            return config;
        }
        catch
        {
            return new AppConfig();
        }
    }
}
