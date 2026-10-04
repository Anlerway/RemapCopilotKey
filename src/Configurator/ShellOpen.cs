using System.Diagnostics;

namespace RemapCopilotKey.Configurator;

// 用系统默认程序打开 URL 或文件。
// URL 走与服务程序相同的一条路径：借 rundll32 的 FileProtocolHandler 交给默认浏览器。
internal static class ShellOpen
{
    public static void OpenUrl(string url)
    {
        if (string.IsNullOrWhiteSpace(url)) return;

        Process.Start(new ProcessStartInfo
        {
            FileName = "rundll32.exe",
            Arguments = "url.dll,FileProtocolHandler \"" + url + "\"",
            UseShellExecute = false,
            CreateNoWindow = true,
        });
    }

    public static void OpenPath(string path)
    {
        if (string.IsNullOrWhiteSpace(path)) return;
        Process.Start(new ProcessStartInfo(path) { UseShellExecute = true });
    }
}