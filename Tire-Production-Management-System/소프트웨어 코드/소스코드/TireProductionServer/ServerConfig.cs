using System.Text.Json;

namespace TireProductionServer;

internal sealed class ServerConfig
{
    private const string ConfigFileName = "server_config.json";

    public int Port { get; init; } = 5000;
    public string DatabasePath { get; init; } = "data/production.db";

    public static ServerConfig Load()
    {
        string path = Path.Combine(AppContext.BaseDirectory, ConfigFileName);
        ServerConfig config = new();

        try
        {
            if (File.Exists(path))
            {
                config = JsonSerializer.Deserialize<ServerConfig>(
                    File.ReadAllText(path),
                    new JsonSerializerOptions { PropertyNameCaseInsensitive = true })
                    ?? new ServerConfig();
            }
        }
        catch
        {
            config = new ServerConfig();
        }

        if (config.Port is < 1 or > 65535)
            config = new ServerConfig { DatabasePath = config.DatabasePath };

        return config;
    }

    public string ResolveDatabasePath()
    {
        if (Path.IsPathRooted(DatabasePath))
            return DatabasePath;

        return Path.GetFullPath(
            Path.Combine(AppContext.BaseDirectory, DatabasePath));
    }
}
