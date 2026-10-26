using System.Diagnostics;
using System.Runtime.InteropServices;

namespace RemapCopilotKey.Configurator;

// ---------------------------------------------------------------------------
// 与 C++ 服务程序（RemapCopilotKeyService.exe）的交互
//
// 三条通道：
//   1. 配置文件 —— 配置程序写 config.json，服务程序轮询发现变化后热加载。
//      这是主要通道，不需要任何 IPC 机制。
//   2. 窗口消息 —— 少数控制指令（重新加载、退出）直接发给服务程序的隐藏窗口。
//   3. 服务控制 —— 启动 / 停止 / 注册开机自启，走 SCM，见 ServiceInstaller.cs。
//
// 消息常量与窗口类名必须与 src/Service/src/main.cpp 保持一致。
//
// 注意：配置程序以管理员权限运行（app.manifest 里 requireAdministrator），
// 和服务程序同级，所以这里的所有操作都不再需要单独提权。
// ---------------------------------------------------------------------------

public static class ServiceControl
{
    /// <summary>服务程序隐藏窗口的类名（见 main.cpp 的 kWindowClassName）。</summary>
    public const string WindowClassName = "RemapCopilotKeyService.MessageWindow";

    // WM_APP + N
    public const uint WmRckReload = 0x8000 + 4;
    public const uint WmRckQuit = 0x8000 + 5;

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr FindWindowW(string? lpClassName, string? lpWindowName);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool PostMessageW(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    // -- 路径 ---------------------------------------------------------------

    /// <summary>
    /// 程序所在目录。
    /// net48 没有 AppContext.BaseDirectory，等价物是 AppDomain.CurrentDomain.BaseDirectory
    /// （它总是带结尾的反斜杠，Path.Combine 能正确处理）。
    /// </summary>
    public static string BaseDirectory => AppDomain.CurrentDomain.BaseDirectory;

    public static string ConfigPath => Path.Combine(BaseDirectory, "config.json");

    public static string LogPath => Path.Combine(BaseDirectory, LogFileName);

    public static string LogFileName => "RemapCopilotKeyService.log";

    public static string ServiceExeName => "RemapCopilotKeyService.exe";

    public static string ServiceExePath => Path.Combine(BaseDirectory, ServiceExeName);

    // -- 进程 / 窗口 --------------------------------------------------------

    public static IntPtr FindServiceWindow() => FindWindowW(WindowClassName, null);

    public static bool IsRunning => FindServiceWindow() != IntPtr.Zero;

    /// <summary>让服务立刻重新读取配置（无需重启）。</summary>
    public static bool SignalReload()
    {
        var hwnd = FindServiceWindow();
        return hwnd != IntPtr.Zero && PostMessageW(hwnd, WmRckReload, IntPtr.Zero, IntPtr.Zero);
    }

    /// <summary>
    /// 请求服务退出。
    /// 服务端对 WM_RCK_QUIT 调用了 ChangeWindowMessageFilterEx 放行，
    /// 所以即使权限不同也能直接发消息。
    /// </summary>
    public static bool SignalQuit()
    {
        var hwnd = FindServiceWindow();
        return hwnd != IntPtr.Zero && PostMessageW(hwnd, WmRckQuit, IntPtr.Zero, IntPtr.Zero);
    }

    /// <summary>等待服务退出，最多等待 timeoutMs 毫秒。</summary>
    public static bool WaitForExit(int timeoutMs = 5000)
    {
        // net48 没有 Environment.TickCount64，用 Stopwatch 取单调时间
        var deadline = Stopwatch.GetTimestamp() + (long)(timeoutMs / 1000.0 * Stopwatch.Frequency);
        while (Stopwatch.GetTimestamp() < deadline)
        {
            if (!IsRunning) return true;
            Thread.Sleep(100);
        }
        return !IsRunning;
    }

    /// <summary>服务程序是否存在。</summary>
    public static bool ServiceExists => File.Exists(ServiceExePath);

    /// <summary>
    /// 启动服务。
    ///
    /// 已注册成 SCM 服务时走 SCM（ServiceInstaller.Start）：服务宿主一起来就会
    /// 立刻把钩子进程投递进当前会话，**不弹 UAC**，因为服务是 LocalSystem。
    ///
    /// 还没注册时退回直接启动：配置程序自己已经是管理员，直接 CreateProcess
    /// 出来的子进程同样是管理员，也不弹 UAC（这个子进程不受服务监督，
    /// 关掉配置程序后它仍然活着，只是不会跨会话）。
    /// </summary>
    public static bool StartService(out string error)
    {
        error = string.Empty;

        if (!ServiceExists)
        {
            error = string.Format(Localization.T("msg.serviceNotFound.body"), ServiceExeName);
            return false;
        }

        if (ServiceInstaller.IsInstalled())
        {
            return ServiceInstaller.Start(out error);
        }

        try
        {
            var psi = new ProcessStartInfo(ServiceExePath)
            {
                UseShellExecute = true,
                WorkingDirectory = BaseDirectory,
            };
            Process.Start(psi);
            return true;
        }
        catch (Exception ex)
        {
            error = ex.Message;
            return false;
        }
    }

    /// <summary>
    /// 停止服务。
    ///
    /// 已注册成 SCM 服务时先停服务 —— 服务宿主会顺手结束各会话里的钩子进程，
    /// 作业对象的 KILL_ON_JOB_CLOSE 再兜一次底 —— 然后确认窗口真的没了。
    /// 没注册时退回发 WM_RCK_QUIT，请进程自己退出（和旧版本行为一致）。
    /// </summary>
    public static bool StopService(out string error)
    {
        error = string.Empty;

        if (ServiceInstaller.IsInstalled())
        {
            if (!ServiceInstaller.Stop(out error)) return false;
        }

        if (IsRunning)
        {
            SignalQuit();
            if (!WaitForExit())
            {
                error = Localization.T("service.error.stopTimeout");
                return false;
            }
        }

        return true;
    }

    /// <summary>等待服务窗口出现（启动后需要一点时间安装钩子）。</summary>
    public static bool WaitForStart(int timeoutMs = 5000)
    {
        var deadline = Stopwatch.GetTimestamp() + (long)(timeoutMs / 1000.0 * Stopwatch.Frequency);
        while (Stopwatch.GetTimestamp() < deadline)
        {
            if (IsRunning) return true;
            Thread.Sleep(100);
        }
        return IsRunning;
    }
}
