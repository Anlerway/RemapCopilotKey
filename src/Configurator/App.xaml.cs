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

        // 给安装包用的开关：静默注册 / 清除开机自启服务后退出，不开窗口。
        //
        // 安装包（Inno Setup）本来就是管理员权限，安装完直接调这个开关，
        // 用户装完就是"已注册自启 + 服务已在跑"的状态，不需要再点一次注册。
        if (Array.Exists(e.Args, a => string.Equals(a, "--register-service", StringComparison.Ordinal)))
        {
            ApplyConfiguredLanguage();
            Shutdown(ServiceInstaller.Install(ServiceControl.ServiceExePath, out _) ? 0 : 1);
            return;
        }

        if (Array.Exists(e.Args, a => string.Equals(a, "--unregister-service", StringComparison.Ordinal)))
        {
            ApplyConfiguredLanguage();
            Shutdown(ServiceInstaller.Uninstall(out _) ? 0 : 1);
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

    /// <summary>
    /// 按 config.json 里的 language 选一次界面语言。
    ///
    /// 静默分支不经过主窗口，但服务的显示名与描述取自语言包（ServiceInstaller 的
    /// service.displayName / service.description），不选一次就会写进 services.msc
    /// 里与用户界面语言不一致的文案。
    /// </summary>
    private static void ApplyConfiguredLanguage()
    {
        var language = AppConfig.Load(ServiceControl.ConfigPath).Language;
        if (!string.IsNullOrWhiteSpace(language))
        {
            Localization.SetLanguage(language);
        }
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
