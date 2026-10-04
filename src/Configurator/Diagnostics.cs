using System.Text;

namespace RemapCopilotKey.Configurator;

// ---------------------------------------------------------------------------
// 极简诊断日志（仅在带 --diagnose 启动时启用）
//
// 为什么需要：GUI 程序**没有任何标准输出**，"双击了但什么都没出现"这类问题
// 在命令行里看不到任何线索（进程直接以 0 退出）。配置程序的启动路径上有几个
// 会静默返回的分支（单实例已存在、配置文件读不出来），没有日志就只能猜。
//
// 日志写到 %TEMP%\RemapCopilotKey.Configurator.log，不写程序目录
// （装在 Program Files 下时不一定可写，而这正是最需要日志的场景）。
// ---------------------------------------------------------------------------

internal static class Diagnostics
{
    private static string? s_path;
    private static readonly object s_gate = new();

    public static bool Enabled { get; private set; }

    public static string LogPath =>
        s_path ?? Path.Combine(Path.GetTempPath(), "RemapCopilotKey.Configurator.log");

    public static void Enable()
    {
        Enabled = true;
        s_path = LogPath;

        lock (s_gate)
        {
            try
            {
                File.WriteAllText(
                    s_path,
                    "=== RemapCopilotKey 配置程序诊断日志 " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + " ===\n",
                    new UTF8Encoding(false));
            }
            catch
            {
                // 连日志都写不出去就只能放弃诊断了，不影响主流程
            }
        }
    }

    public static void Log(string message)
    {
        if (!Enabled) return;

        lock (s_gate)
        {
            try
            {
                // 用 FileStream(Append) + StreamWriter，而不是 File.AppendAllText：
                // 后者每次都要读回整个文件再重写，日志一长就很慢，也更容易出问题。
                using var stream = new FileStream(
                    LogPath, FileMode.Append, FileAccess.Write, FileShare.ReadWrite);
                using var writer = new StreamWriter(stream, new UTF8Encoding(false));
                writer.WriteLine(DateTime.Now.ToString("HH:mm:ss.fff") + "  " + message);
            }
            catch
            {
                // 日志写不出去不影响主流程
            }
        }
    }
}
