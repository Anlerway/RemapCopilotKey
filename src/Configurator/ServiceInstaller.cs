using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace RemapCopilotKey.Configurator;

// ---------------------------------------------------------------------------
// 把服务程序注册成 **真正的 Windows 服务**（SCM），这是开机自启的宿主。
//
// ── 为什么不再用计划任务 ────────────────────────────────────────────────
//
// 旧实现用计划任务（RunLevel=HighestAvailable + LogonType=InteractiveToken）
// 登录时静默启动钩子进程。理论上这该给出高完整性，但实测给的是
// **低完整性受限令牌**（boot.log: "完整性=低(0x1000) 提权=是"）—— 钩子装得上、
// 日志照写、一个按键都收不到。试遍五种任务形状（ONLOGON 不带 /RU、
// RequiredPrivileges、GroupId+InteractiveToken、UserId+HighestAvailable、
// 启动时 SYSTEM 任务 + Session 0 宿主）都不行，详见 docs/设计说明.md 第 8 节。
//
// 现在的做法：注册一个 LocalSystem 的自动启动服务（RemapCopilotKeyAgent）。
// SCM 用**完整 SYSTEM 令牌**启动它，服务再把同一个 exe 以用户完整令牌
// CreateProcessAsUser 投递进交互式会话（见 src/Service/src/ServiceHost.cpp），
// 于是钩子进程拿到高完整性，全程不弹 UAC。
//
// 配置程序本身是 requireAdministrator，所以这里的 SCM 调用不需要额外提权。
// ---------------------------------------------------------------------------

public static class ServiceInstaller
{
    // -- 服务标识（与 src/Service/src/ServiceHost.h 的 kServiceName 一致）-----

    public const string ServiceName = "RemapCopilotKeyAgent";

    /// <summary>旧版本用的计划任务名，安装/卸载时顺手清掉。</summary>
    public const string LegacyTaskName = "RemapCopilotKey";

    private static string DisplayName => Localization.T("service.displayName");

    private static string Description => Localization.T("service.description");

    // -- 常量 ---------------------------------------------------------------

    private const uint SC_MANAGER_CONNECT = 0x0001;
    private const uint SC_MANAGER_CREATE_SERVICE = 0x0002;

    private const uint SERVICE_QUERY_STATUS = 0x0004;
    private const uint SERVICE_START = 0x0010;
    private const uint SERVICE_STOP = 0x0020;
    private const uint SERVICE_CHANGE_CONFIG = 0x0002;
    private const uint SERVICE_ALL_ACCESS = 0xF01FF;

    private const uint SERVICE_WIN32_OWN_PROCESS = 0x00000010;
    private const uint SERVICE_AUTO_START = 0x00000002;
    private const uint SERVICE_DEMAND_START = 0x00000003;
    private const uint SERVICE_ERROR_NORMAL = 0x00000001;

    private const uint SERVICE_CONTROL_STOP = 0x00000001;

    private const uint SERVICE_STOPPED = 0x00000001;
    private const uint SERVICE_START_PENDING = 0x00000002;
    private const uint SERVICE_STOP_PENDING = 0x00000003;
    private const uint SERVICE_RUNNING = 0x00000004;

    private const uint SERVICE_CONFIG_DESCRIPTION = 1;
    private const uint SERVICE_CONFIG_FAILURE_ACTIONS = 2;

    private const int SC_STATUS_PROCESS_INFO = 0;
    private const int SC_ACTION_RESTART = 1;

    private const int ERROR_ACCESS_DENIED = 5;
    private const int ERROR_SERVICE_EXISTS = 1073;
    private const int ERROR_SERVICE_ALREADY_RUNNING = 1056;
    private const int ERROR_SERVICE_NOT_ACTIVE = 1062;
    private const int ERROR_SERVICE_DISABLED = 1058;
    private const int ERROR_SERVICE_MARKED_FOR_DELETE = 1072;
    private const int ERROR_SERVICE_DOES_NOT_EXIST = 1060;

    // -- P/Invoke -----------------------------------------------------------

    [StructLayout(LayoutKind.Sequential)]
    private struct SERVICE_STATUS_PROCESS
    {
        public uint dwServiceType;
        public uint dwCurrentState;
        public uint dwControlsAccepted;
        public uint dwWin32ExitCode;
        public uint dwServiceSpecificExitCode;
        public uint dwCheckPoint;
        public uint dwWaitHint;
        public uint dwProcessId;
        public uint dwServiceFlags;
    }

    // ControlService / StartService 的出参。**不能用 NULL**：文档写明 lpServiceStatus
    // 必须指向一个可写的 SERVICE_STATUS，传 NULL 时调用直接失败（拿不到错误码语义，
    // 看起来就像"停服务没生效"）。
    [StructLayout(LayoutKind.Sequential)]
    private struct SERVICE_STATUS
    {
        public uint dwServiceType;
        public uint dwCurrentState;
        public uint dwControlsAccepted;
        public uint dwWin32ExitCode;
        public uint dwServiceSpecificExitCode;
        public uint dwCheckPoint;
        public uint dwWaitHint;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct SERVICE_DESCRIPTION
    {
        public IntPtr lpDescription;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct SC_ACTION
    {
        public int Type;
        public uint Delay;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct SERVICE_FAILURE_ACTIONS
    {
        public uint dwResetPeriod;
        public IntPtr lpRebootMsg;
        public IntPtr lpCommand;
        public int cActions;
        public IntPtr lpsaActions;
    }

    [DllImport("advapi32.dll", EntryPoint = "OpenSCManagerW", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr OpenSCManager(string? machineName, string? databaseName, uint desiredAccess);

    [DllImport("advapi32.dll", EntryPoint = "CreateServiceW", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateService(
        IntPtr hSCManager,
        string lpServiceName,
        string? lpDisplayName,
        uint dwDesiredAccess,
        uint dwServiceType,
        uint dwStartType,
        uint dwErrorControl,
        string lpBinaryPathName,
        string? lpLoadOrderGroup,
        IntPtr lpdwTagId,
        string? lpDependencies,
        string? lpServiceStartName,
        string? lpPassword);

    [DllImport("advapi32.dll", EntryPoint = "OpenServiceW", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr OpenService(IntPtr hSCManager, string lpServiceName, uint dwDesiredAccess);

    [DllImport("advapi32.dll", EntryPoint = "ChangeServiceConfigW", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ChangeServiceConfig(
        IntPtr hService,
        uint dwServiceType,
        uint dwStartType,
        uint dwErrorControl,
        string? lpBinaryPathName,
        string? lpLoadOrderGroup,
        IntPtr lpdwTagId,
        string? lpDependencies,
        string? lpServiceStartName,
        string? lpPassword,
        string? lpDisplayName);

    [DllImport("advapi32.dll", EntryPoint = "ChangeServiceConfig2W", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ChangeServiceConfig2(IntPtr hService, uint dwInfoLevel, IntPtr lpInfo);

    [DllImport("advapi32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DeleteService(IntPtr hService);

    [DllImport("advapi32.dll", EntryPoint = "StartServiceW", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool StartService(IntPtr hService, int dwNumServiceArgs, IntPtr lpServiceArgVectors);

    [DllImport("advapi32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ControlService(IntPtr hService, uint dwControl, out SERVICE_STATUS lpServiceStatus);

    [DllImport("advapi32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool QueryServiceStatusEx(
        IntPtr hService,
        int infoLevel,
        out SERVICE_STATUS_PROCESS lpBuffer,
        int cbBufSize,
        out int pcbBytesNeeded);

    [DllImport("advapi32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseServiceHandle(IntPtr hSCObject);

    // -- 错误描述 -----------------------------------------------------------

    private static string Describe(int errorCode, string action)
    {
        string text;
        try
        {
            text = new Win32Exception(errorCode).Message;
        }
        catch (Exception)
        {
            text = "错误码 " + errorCode;
        }

        var hint = string.Empty;
        switch (errorCode)
        {
            case ERROR_ACCESS_DENIED:
                hint = Localization.T("service.error.accessDenied");
                break;
            case ERROR_SERVICE_DISABLED:
                hint = Localization.T("service.error.disabled");
                break;
            case ERROR_SERVICE_MARKED_FOR_DELETE:
                hint = Localization.T("service.error.markedForDelete");
                break;
            case ERROR_SERVICE_DOES_NOT_EXIST:
                hint = Localization.T("service.error.notInstalled");
                break;
        }

        return string.Format(Localization.T("service.error.format"), action, errorCode, text) +
               (hint.Length > 0 ? Environment.NewLine + hint : string.Empty);
    }

    // -- 状态查询 -----------------------------------------------------------

    /// <summary>服务是否已注册到 SCM（不关心是否在运行）。</summary>
    public static bool IsInstalled()
    {
        var scm = OpenSCManager(null, null, SC_MANAGER_CONNECT);
        if (scm == IntPtr.Zero) return false;

        try
        {
            var service = OpenService(scm, ServiceName, SERVICE_QUERY_STATUS);
            if (service == IntPtr.Zero) return false;

            CloseServiceHandle(service);
            return true;
        }
        finally
        {
            CloseServiceHandle(scm);
        }
    }

    /// <summary>查询服务是否正在运行（服务进程在跑 = 钩子已经投递到会话）。</summary>
    public static bool IsServiceRunning()
    {
        var scm = OpenSCManager(null, null, SC_MANAGER_CONNECT);
        if (scm == IntPtr.Zero) return false;

        try
        {
            var service = OpenService(scm, ServiceName, SERVICE_QUERY_STATUS);
            if (service == IntPtr.Zero) return false;

            try
            {
                var status = new SERVICE_STATUS_PROCESS();
                var needed = 0;
                if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, out status,
                                          Marshal.SizeOf(typeof(SERVICE_STATUS_PROCESS)), out needed))
                {
                    return false;
                }
                return status.dwCurrentState == SERVICE_RUNNING;
            }
            finally
            {
                CloseServiceHandle(service);
            }
        }
        finally
        {
            CloseServiceHandle(scm);
        }
    }

    // -- 注册 / 卸载 --------------------------------------------------------

    /// <summary>
    /// 注册（或更新）服务并立即启动。
    ///
    /// 幂等：服务已存在时改为更新二进制路径/启动类型/描述，不会因为
    /// "已注册"而失败 —— 用户换了安装目录再点一次注册也能自愈。
    /// </summary>
    public static bool Install(string serviceExePath, out string error)
    {
        error = string.Empty;

        if (!File.Exists(serviceExePath))
        {
            error = string.Format(Localization.T("msg.serviceNotFound.body"), Path.GetFileName(serviceExePath));
            return false;
        }

        // 命令行必须是"带引号的完整路径 + --service"：路径里有空格时
        // 不引号会被 SCM 拆错（这是 Windows 服务最经典的坑）。
        var binaryPath = "\"" + serviceExePath + "\" --service";

        var scm = OpenSCManager(null, null, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
        if (scm == IntPtr.Zero)
        {
            error = Describe(Marshal.GetLastWin32Error(), Localization.T("service.action.openManager"));
            return false;
        }

        try
        {
            var service = CreateService(scm, ServiceName, DisplayName, SERVICE_ALL_ACCESS,
                                        SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                        binaryPath, null, IntPtr.Zero, null, null, null);

            if (service == IntPtr.Zero)
            {
                var code = Marshal.GetLastWin32Error();
                if (code != ERROR_SERVICE_EXISTS)
                {
                    error = Describe(code, Localization.T("service.action.create"));
                    return false;
                }

                // 已注册过：打开它并更新配置
                service = OpenService(scm, ServiceName, SERVICE_ALL_ACCESS);
                if (service == IntPtr.Zero)
                {
                    error = Describe(Marshal.GetLastWin32Error(), Localization.T("service.action.open"));
                    return false;
                }
            }

            try
            {
                // lpServiceStartName 显式写 "LocalSystem"：ChangeServiceConfig 传 NULL 的语义是
                // "这一项不改"，那样服务账户曾被改成别的账户时就不会被纠正回来。
                if (!ChangeServiceConfig(service, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                         binaryPath, null, IntPtr.Zero, null, "LocalSystem", null, DisplayName))
                {
                    error = Describe(Marshal.GetLastWin32Error(), Localization.T("service.action.configure"));
                    return false;
                }

                SetDescription(service);
                SetRestartOnFailure(service);
            }
            finally
            {
                CloseServiceHandle(service);
            }
        }
        finally
        {
            CloseServiceHandle(scm);
        }

        RemoveLegacyTask();

        return Start(out error);
    }

    /// <summary>停止服务并从 SCM 删除注册（同时清掉旧版本留下的计划任务）。</summary>
    public static bool Uninstall(out string error)
    {
        error = string.Empty;

        var scm = OpenSCManager(null, null, SC_MANAGER_CONNECT);
        if (scm == IntPtr.Zero)
        {
            error = Describe(Marshal.GetLastWin32Error(), Localization.T("service.action.openManager"));
            return false;
        }

        try
        {
            var service = OpenService(scm, ServiceName, SERVICE_ALL_ACCESS);
            if (service == IntPtr.Zero)
            {
                var code = Marshal.GetLastWin32Error();
                if (code == ERROR_SERVICE_DOES_NOT_EXIST)
                {
                    // 没注册也照样清旧任务，不算失败
                    RemoveLegacyTask();
                    return true;
                }

                error = Describe(code, Localization.T("service.action.open"));
                return false;
            }

            try
            {
                // 必须先停再删。DeleteService 只是"标记删除"：服务还在跑的时候名字不会
                // 消失，SCM 还会把它的启动类型改成"禁用"，调用方看到的就是
                // "删了但还在跑、而且再也起不来"这种自相矛盾的状态。所以停不下来就报错。
                if (!RequestStopAndWait(service, out error)) return false;

                if (!DeleteService(service))
                {
                    var code = Marshal.GetLastWin32Error();
                    // 1072：已经被别的实例标记删除，交给下面的等待收尾
                    if (code != ERROR_SERVICE_MARKED_FOR_DELETE)
                    {
                        error = Describe(code, Localization.T("service.action.delete"));
                        return false;
                    }
                }
            }
            finally
            {
                CloseServiceHandle(service);
            }

            // 等服务真的从 SCM 里消失，"已清除"才算数。
            if (!WaitForDeletion(scm, 15000))
            {
                error = Localization.T("service.error.deleteTimeout");
                return false;
            }
        }
        finally
        {
            CloseServiceHandle(scm);
        }

        RemoveLegacyTask();
        return true;
    }

    // -- 启动 / 停止 --------------------------------------------------------

    /// <summary>启动服务，并等到 SCM 报 SERVICE_RUNNING。</summary>
    public static bool Start(out string error)
    {
        error = string.Empty;

        var scm = OpenSCManager(null, null, SC_MANAGER_CONNECT);
        if (scm == IntPtr.Zero)
        {
            error = Describe(Marshal.GetLastWin32Error(), Localization.T("service.action.openManager"));
            return false;
        }

        try
        {
            var service = OpenService(scm, ServiceName, SERVICE_START | SERVICE_QUERY_STATUS);
            if (service == IntPtr.Zero)
            {
                error = Describe(Marshal.GetLastWin32Error(), Localization.T("service.action.open"));
                return false;
            }

            try
            {
                if (!StartService(service, 0, IntPtr.Zero))
                {
                    var code = Marshal.GetLastWin32Error();
                    if (code != ERROR_SERVICE_ALREADY_RUNNING)
                    {
                        error = Describe(code, Localization.T("service.action.start"));
                        return false;
                    }
                }

                if (!WaitForState(service, SERVICE_RUNNING, 20000))
                {
                    error = Localization.T("service.error.startTimeout");
                    return false;
                }
            }
            finally
            {
                CloseServiceHandle(service);
            }
        }
        finally
        {
            CloseServiceHandle(scm);
        }

        return true;
    }

    /// <summary>停止服务（服务宿主会先收掉各会话里的钩子进程）。</summary>
    public static bool Stop(out string error)
    {
        error = string.Empty;

        var scm = OpenSCManager(null, null, SC_MANAGER_CONNECT);
        if (scm == IntPtr.Zero)
        {
            error = Describe(Marshal.GetLastWin32Error(), Localization.T("service.action.openManager"));
            return false;
        }

        try
        {
            var service = OpenService(scm, ServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS);
            if (service == IntPtr.Zero)
            {
                error = Describe(Marshal.GetLastWin32Error(), Localization.T("service.action.open"));
                return false;
            }

            try
            {
                if (!RequestStopAndWait(service, out error)) return false;
            }
            finally
            {
                CloseServiceHandle(service);
            }
        }
        finally
        {
            CloseServiceHandle(scm);
        }

        return true;
    }

    // -- 内部工具 -----------------------------------------------------------

    /// <summary>
    /// 请求停止并等到 SERVICE_STOPPED。已经停着、或本来就没在运行（1062）都算成功。
    ///
    /// 注意 ControlService 的第三个参数必须是一个真的 SERVICE_STATUS 缓冲区：
    /// 早期版本传 IntPtr.Zero，调用直接失败、返回值又被忽略，于是"删不掉也停不下"。
    /// </summary>
    private static bool RequestStopAndWait(IntPtr service, out string error)
    {
        error = string.Empty;

        var status = new SERVICE_STATUS_PROCESS();
        var needed = 0;
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, out status,
                                  Marshal.SizeOf(typeof(SERVICE_STATUS_PROCESS)), out needed))
        {
            error = Describe(Marshal.GetLastWin32Error(), Localization.T("service.action.query"));
            return false;
        }

        if (status.dwCurrentState == SERVICE_STOPPED) return true;

        if (!ControlService(service, SERVICE_CONTROL_STOP, out _))
        {
            var code = Marshal.GetLastWin32Error();
            if (code != ERROR_SERVICE_NOT_ACTIVE)
            {
                error = Describe(code, Localization.T("service.action.stop"));
                return false;
            }
            return true;
        }

        if (!WaitForState(service, SERVICE_STOPPED, 20000))
        {
            error = Localization.T("service.error.stopTimeout");
            return false;
        }

        return true;
    }

    /// <summary>
    /// 等服务从 SCM 里真正消失。DeleteService 只打标记，服务进程退出后名字才会消失；
    /// 不等这一步就报成功，用户会看到"已清除"但 services.msc 里还挂着它。
    /// </summary>
    private static bool WaitForDeletion(IntPtr scm, int timeoutMs)
    {
        var deadline = Stopwatch.GetTimestamp() + (long)(timeoutMs / 1000.0 * Stopwatch.Frequency);

        while (Stopwatch.GetTimestamp() < deadline)
        {
            var service = OpenService(scm, ServiceName, SERVICE_QUERY_STATUS);
            if (service == IntPtr.Zero)
            {
                if (Marshal.GetLastWin32Error() == ERROR_SERVICE_DOES_NOT_EXIST) return true;
            }
            else
            {
                CloseServiceHandle(service);
            }

            Thread.Sleep(150);
        }

        return false;
    }

    private static bool WaitForState(IntPtr service, uint desiredState, int timeoutMs)
    {
        var deadline = Stopwatch.GetTimestamp() + (long)(timeoutMs / 1000.0 * Stopwatch.Frequency);

        while (Stopwatch.GetTimestamp() < deadline)
        {
            var status = new SERVICE_STATUS_PROCESS();
            var needed = 0;
            if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, out status,
                                     Marshal.SizeOf(typeof(SERVICE_STATUS_PROCESS)), out needed))
            {
                if (status.dwCurrentState == desiredState) return true;

                // 启动过程中直接停在 STOPPED = 启动失败，别白等到超时
                if (desiredState == SERVICE_RUNNING && status.dwCurrentState == SERVICE_STOPPED) return false;
            }

            Thread.Sleep(150);
        }

        return false;
    }

    private static void SetDescription(IntPtr service)
    {
        var description = Description;
        var buffer = Marshal.StringToHGlobalUni(description);
        var info = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(SERVICE_DESCRIPTION)));

        try
        {
            var descriptionStruct = new SERVICE_DESCRIPTION { lpDescription = buffer };
            Marshal.StructureToPtr(descriptionStruct, info, false);
            ChangeServiceConfig2(service, SERVICE_CONFIG_DESCRIPTION, info);
        }
        finally
        {
            Marshal.FreeHGlobal(info);
            Marshal.FreeHGlobal(buffer);
        }
    }

    /// <summary>
    /// 服务进程意外退出时让 SCM 自动重启它（5 秒 / 15 秒 / 60 秒各一次）。
    /// 钩子进程会随服务一起被杀（作业对象），所以重启服务就能把钩子补回来。
    /// </summary>
    private static void SetRestartOnFailure(IntPtr service)
    {
        var actionSize = Marshal.SizeOf(typeof(SC_ACTION));
        var actionCount = 3;

        var actions = Marshal.AllocHGlobal(actionSize * actionCount);
        var info = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(SERVICE_FAILURE_ACTIONS)));

        try
        {
            var delays = new uint[] { 5000, 15000, 60000 };
            for (var i = 0; i < actionCount; i++)
            {
                var action = new SC_ACTION { Type = SC_ACTION_RESTART, Delay = delays[i] };
                Marshal.StructureToPtr(action, new IntPtr(actions.ToInt64() + actionSize * i), false);
            }

            var failureActions = new SERVICE_FAILURE_ACTIONS
            {
                dwResetPeriod = 86400,
                lpRebootMsg = IntPtr.Zero,
                lpCommand = IntPtr.Zero,
                cActions = actionCount,
                lpsaActions = actions,
            };

            Marshal.StructureToPtr(failureActions, info, false);
            ChangeServiceConfig2(service, SERVICE_CONFIG_FAILURE_ACTIONS, info);
        }
        finally
        {
            Marshal.FreeHGlobal(info);
            Marshal.FreeHGlobal(actions);
        }
    }

    /// <summary>
    /// 删除旧版本注册的计划任务（`RemapCopilotKey`）。
    ///
    /// 老版本把开机自启做成了计划任务；升级到本版本后如果不清掉，登录时
    /// 会多起一个低完整性的钩子进程，表现为"新的自启看着成了，但按键还是没反应"。
    /// 任务不存在时 schtasks 会报错，这里刻意忽略。
    /// </summary>
    public static void RemoveLegacyTask()
    {
        try
        {
            var psi = new ProcessStartInfo("schtasks.exe", "/Delete /TN \"" + LegacyTaskName + "\" /F")
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
            };

            using (var process = Process.Start(psi))
            {
                if (process != null)
                {
                    process.WaitForExit(5000);
                }
            }
        }
        catch (Exception)
        {
            // 任务不存在、schtasks 不可用都不影响主流程
        }
    }
}
