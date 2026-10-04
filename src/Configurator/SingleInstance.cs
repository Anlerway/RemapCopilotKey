using System.Diagnostics;
using System.Runtime.InteropServices;

namespace RemapCopilotKey.Configurator;

// ---------------------------------------------------------------------------
// 单实例保护（WPF 版，替代原来 WinForms Program.cs 里的实现）
//
// 用命名互斥体做单实例，第二个实例把已有窗口拉到前台后退出。
// （服务程序自己也有单实例保护，两边各自独立。）
//
// 互斥体名不带 Global\ 前缀：配置程序是 asInvoker（普通权限），
// 在当前会话内唯一就够了，也避免在受限帐户下创建失败。
// ---------------------------------------------------------------------------

internal static class SingleInstance
{
    private const string MutexName = "RemapCopilotKey.Configurator.SingleInstance";

    private const int SW_RESTORE = 9;

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

    /// <summary>进程存活期间一直持有的互斥体。字段持有引用，不能让它被 GC 回收。</summary>
    private static Mutex? s_mutex;

    /// <summary>
    /// 尝试成为唯一实例。
    /// 注意：这里不释放互斥体 —— 它随进程退出由操作系统回收，
    /// 显式释放反而会在窗口关闭但进程尚未退出时放行第二个实例。
    /// </summary>
    public static bool TryAcquire()
    {
        s_mutex = new Mutex(initiallyOwned: true, MutexName, out var createdNew);
        return createdNew;
    }

    /// <summary>
    /// 找到已经在运行的配置程序窗口并激活它。找不到窗口返回 false。
    /// </summary>
    public static bool BringExistingWindowToFront()
    {
        try
        {
            var current = Process.GetCurrentProcess();
            var others = Process.GetProcessesByName(current.ProcessName);

            foreach (var process in others)
            {
                if (process.Id == current.Id) continue;

                var handle = process.MainWindowHandle;
                if (handle == IntPtr.Zero) continue;

                SetForegroundWindow(handle);
                ShowWindow(handle, SW_RESTORE);
                return true;
            }
        }
        catch
        {
            // 激活失败无所谓，反正另一个实例已经在运行
        }

        return false;
    }
}
