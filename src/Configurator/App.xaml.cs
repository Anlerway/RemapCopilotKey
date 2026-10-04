using System.Windows;

namespace RemapCopilotKey.Configurator;

/// <summary>
/// 应用程序入口。
///
/// 启动流程：命令行诊断开关 → 单实例检查 → 按 config.json 里的 language 选语言 → 打开主窗口。
///
/// 注意：net48 + WPF 没有 .NET 6 的 ApplicationConfiguration.Initialize()，
/// 所有初始化都由 WPF 自己完成（DPI 在 app.manifest 里声明）。
/// </summary>
public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        // 诊断用开关：把一份默认配置写到指定路径后立刻退出，不创建任何窗口。
        // 用于在命令行里验证"配置程序写出的 JSON"与"C++ 服务程序读入的 JSON"一致。
        if (e.Args.Length >= 2 && string.Equals(e.Args[0], "--dump-default-config", StringComparison.Ordinal))
        {
            Shutdown(DumpDefaultConfig(e.Args[1]));
            return;
        }

        // 诊断用开关：导出**系统里已注册**的那个任务的真实定义后退出。
        //
        // 注册走的是 schtasks 的命令行开关（见 AutoStart.cs），任务定义由
        // 计划任务程序自己生成。所以"我们请求的"没什么好看的，要排查
        // "任务主体 / 运行级别不对"只能看系统实际存下来的那一份 ——
        // 主体被系统改写这种事真的发生过。
        if (e.Args.Length >= 2 && string.Equals(e.Args[0], "--export-task", StringComparison.Ordinal))
        {
            Shutdown(ExportRegisteredTask(e.Args[1]));
            return;
        }

        // 诊断用开关：把启动过程中的每一步写进日志。
        // 界面"启动了但什么也没出现"这类问题（例如被单实例逻辑挡掉）
        // 在 GUI 程序里完全没有输出，只能靠这个。
        var diagnose = Array.Exists(e.Args, a => string.Equals(a, "--diagnose", StringComparison.Ordinal));
        if (diagnose)
        {
            Diagnostics.Enable();
        }

        Diagnostics.Log("OnStartup: 参数 " + string.Join(" ", e.Args));

        // 语言跟随配置文件；读不出来就用语言包里的默认（简体中文）
        var config = AppConfig.Load(ServiceControl.ConfigPath);
        Diagnostics.Log("已读取配置: language=" + config.Language);

        if (!string.IsNullOrWhiteSpace(config.Language))
        {
            Localization.SetLanguage(config.Language);
        }

        // 单实例：第二个实例把已有窗口拉到前台后退出。
        // --diagnose 时跳过这个检查：诊断本来就可能是为了排查"为什么单实例把它挡住了"。
        if (!diagnose && !SingleInstance.TryAcquire())
        {
            Diagnostics.Log("已有实例在运行，本实例退出");
            if (!SingleInstance.BringExistingWindowToFront())
            {
                // 找不到已有窗口又不让启动，就必须说清楚，否则用户看到的是"双击没反应"
                MessageBox.Show(
                    Localization.T("msg.alreadyRunning.body"),
                    Localization.T("app.title"),
                    MessageBoxButton.OK, MessageBoxImage.Information);
            }
            Shutdown(0);
            return;
        }

        Diagnostics.Log("取得单实例，正在打开主窗口");
        new MainWindow().Show();
        Diagnostics.Log("主窗口已显示");
    }

    /// <summary>写出默认配置；返回进程退出码。</summary>
    private static int DumpDefaultConfig(string path)
    {
        try
        {
            new AppConfig().Save(path);
            return 0;
        }
        catch (Exception ex)
        {
            return WriteError(path, ex);
        }
    }

    /// <summary>
    /// 导出**已注册任务**的原始任务定义（schtasks 的原生字节）；返回进程退出码。
    /// 用于确认系统最终保存的主体 / 运行级别与我们的意图是否一致。
    /// </summary>
    private static int ExportRegisteredTask(string path)
    {
        if (AutoStart.ExportRegisteredTask(path, out var error))
        {
            return 0;
        }

        try
        {
            File.WriteAllText(path + ".error.txt", error, new System.Text.UTF8Encoding(true));
        }
        catch
        {
            // 连错误文件都写不出去就只能靠退出码了
        }

        return 1;
    }

    private static int WriteError(string path, Exception ex)
    {
        try
        {
            File.WriteAllText(path + ".error.txt", ex.ToString());
        }
        catch
        {
            // 连错误文件都写不出去就只能靠退出码了
        }
        return 1;
    }
}
