using System.IO;
using System.Security.Principal;
using System.Text;
using TaskScheduler;

namespace RemapCopilotKey.Configurator;

// ---------------------------------------------------------------------------
// 开机自启：注册一个"登录时触发、以当前用户身份、最高权限运行"的计划任务。
//
// ── 任务形状（三处都不能动）────────────────────────────────────────────
//
//   触发器  ：登录时，<UserId> = 当前用户 SID（只对这个用户生效）
//   主体    ：<UserId> = 当前用户 SID
//   登录方式：TASK_LOGON_INTERACTIVE_TOKEN（RegisterTaskDefinition 的**参数**）
//   运行级别：HighestAvailable（静默提权，不弹 UAC）
//
// 配上 `--run` 的动作，钩子进程就跑在用户交互式会话里、而且是**高完整性**。
//
// ── 试过并放弃的三条路（都别再走回去）──────────────────────────────────
//
//  1. 任务定义里写 `<RequiredPrivileges>`
//     ❌ 那是任务定义 v1.3 的元素，任务计划程序只允许把它用在 LOCAL SERVICE /
//        NETWORK SERVICE 这类计算机保留账户的主体上。主体写成登录用户时
//        校验器直接拒绝（`(20,57):Privilege:SeImpersonatePrivilege`），
//        `schtasks /Create /XML` 必然失败 —— **注册从来没有成功过**。
//        用 ITaskDefinition::XmlText 实测过：34 个合法特权值逐个填进登录用户的
//        主体，无一被接受；换成 LOCAL SERVICE 立刻通过。
//        而 SeImpersonatePrivilege 本来就不需要这样声明：主体是 UserId +
//        InteractiveToken 时用的就是登录用户自己的会话令牌（管理员令牌本就含它）。
//
//  2. 主体写 `GroupId`，或者用 `schtasks /Create /SC ONLOGON` 不带 /RU
//     ❌ 不带 /RU 时 ONLOGON 的语义是"**任何用户**登录时运行"，主体退化成
//        按组激活；计划任务程序会把登录方式锁成 `TASK_LOGON_GROUP`，
//        连 `RegisterTaskDefinition` 里显式传的 InteractiveToken 都被无视。
//        任务进程被打上**低完整性(0x1000)** 标签：钩子装得上、日志照写，
//        但受 UIPI 限制**一个按键都收不到**。
//        只补 /RU 也不行 —— schtasks 文档写明即使对当前用户它也会提示输入密码。
//
//  3. "系统启动时 + SYSTEM 任务 + Session 0 宿主 + CreateProcessAsUser 投递"
//     ❌ 本想绕开 UAC 走 SYSTEM，结果计划任务程序给任务进程发的是
//        **低完整性受限令牌，连 SYSTEM 主体也一样**。实测现场：
//
//           宿主身份: 账户=NT AUTHORITY\SYSTEM 会话=0 ... 完整性=低(0x1000) 提权=是
//           特权: SeTcb=1 SeAssignPrimaryToken=1 SeIncreaseQuota=1
//           WTSQueryUserToken(会话 1) 失败：错误码 5      ← ERROR_ACCESS_DENIED
//
//        账户是 SYSTEM、特权也确实打开了，照样被拒 —— 受限令牌下调不动
//        WTSQueryUserToken。要真走这条路，载体必须换成 **SCM 注册的 Windows
//        服务**（服务登录方式给的是完整 SYSTEM 令牌），计划任务做不到。
//
// ── 怎么判断它到底成没成 ────────────────────────────────────────────────
//
// **只看"注册成功"是不够的** —— 上面第 2 条是完全静默的。看日志里
// DescribeExecutionEnvironment() 那一行，必须是「完整性=高(0x3000)」。
// 这一行同时写进不受 logLevel 影响的 RemapCopilotKey.boot.log。
//
// 所以 Install 注册完会**回读** `Principal.LogonType`，不是 InteractiveToken
// 就删掉任务并明确报错。
// ---------------------------------------------------------------------------

/// <summary>登录自启计划任务的注册 / 查询 / 删除 / 按需启动。</summary>
internal static class AutoStart
{
    /// <summary>计划任务名。只用英文数字，避免个别系统上解析异常。</summary>
    public const string TaskName = "RemapCopilotKey";

    /// <summary>让服务程序进入"常驻运行"的命令行参数（也是它不带参数时的默认行为）。</summary>
    private const string ServiceArguments = "--run";

    private const string TaskAuthor = "RemapCopilotKey";

    private const string TaskDescription =
        "RemapCopilotKey：用低级键盘钩子识别 Copilot 键（LeftWin + LeftShift + F23），"
        + "把它映射成可用的按键、程序、网页或 Windows 实用功能。";

    private const int TaskCreateOrUpdate = 6;   // TASK_CREATE_OR_UPDATE

    // =======================================================================
    // 对外接口
    // =======================================================================

    /// <summary>计划任务当前是否已注册。</summary>
    public static bool IsRegistered()
    {
        try
        {
            return TryGetTask(out _);
        }
        catch
        {
            return false;
        }
    }

    /// <summary>
    /// 注册（或覆盖）登录自启计划任务。成功返回 true；
    /// 失败时 error 里是可直接展示给用户的原因。
    /// </summary>
    public static bool Install(string serviceExePath, out string error)
    {
        error = string.Empty;

        if (!File.Exists(serviceExePath))
        {
            error = string.Format(Localization.T("msg.serviceNotFound.body"),
                                  Path.GetFileName(serviceExePath));
            return false;
        }

        string userSid;
        try
        {
            userSid = WindowsIdentity.GetCurrent().User?.Value ?? string.Empty;
        }
        catch (Exception ex)
        {
            error = "无法获取当前用户 SID：" + ex.Message;
            return false;
        }

        if (userSid.Length == 0)
        {
            error = "无法获取当前用户 SID，无法创建计划任务。";
            return false;
        }

        try
        {
            var service = CreateTaskService();
            var root = service.GetFolder("\\");

            var definition = service.NewTask(0);

            definition.RegistrationInfo.Author = TaskAuthor;
            definition.RegistrationInfo.Description = TaskDescription;

            var action = (IExecAction)definition.Actions.Create(_TASK_ACTION_TYPE.TASK_ACTION_EXEC);
            action.Path = serviceExePath;
            action.Arguments = ServiceArguments;
            action.WorkingDirectory = Path.GetDirectoryName(serviceExePath) ?? string.Empty;

            // ★ 主体必须是 UserId，不能是 GroupId ★
            // 写成 GroupId 时计划任务程序会把登录方式锁成 TASK_LOGON_GROUP
            // （下面那个 InteractiveToken 参数被无视），任务进程变成低完整性、
            // 键盘钩子静默失效。详见文件顶部第 2 条。
            var principal = definition.Principal;
            principal.UserId = userSid;
            principal.RunLevel = _TASK_RUNLEVEL.TASK_RUNLEVEL_HIGHEST;

            // 触发器也限定到同一个用户：只在他登录时触发，而不是"任何用户登录"。
            var trigger = (ILogonTrigger)definition.Triggers.Create(
                _TASK_TRIGGER_TYPE2.TASK_TRIGGER_LOGON);
            trigger.UserId = userSid;
            trigger.Enabled = true;

            var settings = definition.Settings;
            settings.Compatibility = _TASK_COMPATIBILITY.TASK_COMPATIBILITY_V2_1;
            settings.Enabled = true;
            settings.AllowDemandStart = true;      // 「启动服务」按钮走 Run
            settings.AllowHardTerminate = true;
            settings.DisallowStartIfOnBatteries = false;   // 笔记本用电池时也要启动
            settings.StopIfGoingOnBatteries = false;
            settings.RunOnlyIfIdle = false;
            settings.RunOnlyIfNetworkAvailable = false;
            settings.StartWhenAvailable = false;
            // PT0S = 不限时。服务是常驻进程，默认 72 小时上限会把它结束掉。
            settings.ExecutionTimeLimit = "PT0S";
            settings.MultipleInstances = _TASK_INSTANCES_POLICY.TASK_INSTANCES_IGNORE_NEW;

            // ★ 关键的一行 ★
            // logonType 是**参数**，不在任务定义里 —— 任务定义 XML 的 schema
            // 根本表达不出"GroupId + 非 GROUP 的登录方式"这种组合。
            // 它必须配 UserId 主体才生效。
            var registered = root.RegisterTaskDefinition(
                TaskName, definition, TaskCreateOrUpdate,
                null, null, _TASK_LOGON_TYPE.TASK_LOGON_INTERACTIVE_TOKEN, null);

            // 回读校验。这一步不是多余的：登录方式错成 GROUP 时注册**照样报成功**，
            // 只在很久以后表现为"钩子收不到按键"。
            var registeredLogonType = registered.Definition.Principal.LogonType;
            if (registeredLogonType != _TASK_LOGON_TYPE.TASK_LOGON_INTERACTIVE_TOKEN)
            {
                Diagnostics.Log("回读的登录方式不对：" + registeredLogonType);

                try
                {
                    root.DeleteTask(TaskName, 0);
                }
                catch
                {
                    // 删不掉也要把真实原因报出去
                }

                error = "任务已创建，但登录方式不是「交互式令牌」，实际是 " + registeredLogonType
                        + "。\n\n这种情况下服务会以低完整性运行，键盘钩子收不到任何按键，"
                        + "所以已把刚创建的任务删除。";
                return false;
            }

            Diagnostics.Log("已注册计划任务 " + TaskName + "（UserId=" + userSid
                            + "，RunLevel=HighestAvailable，LogonType=InteractiveToken）");
            return true;
        }
        catch (Exception ex)
        {
            Diagnostics.Log("注册计划任务失败：" + ex);
            error = DescribeFailure(ex);
            return false;
        }
    }

    /// <summary>删除开机自启计划任务；任务本来就不存在也算成功。</summary>
    public static bool Remove(out string error)
    {
        error = string.Empty;

        try
        {
            var service = CreateTaskService();
            var root = service.GetFolder("\\");

            if (!TryGetTask(out _))
            {
                return true;   // 本来就没有，算成功
            }

            root.DeleteTask(TaskName, 0);
            return true;
        }
        catch (Exception ex)
        {
            Diagnostics.Log("删除计划任务失败：" + ex);
            error = DescribeFailure(ex);
            return false;
        }
    }

    /// <summary>
    /// 通过计划任务按需启动服务。
    ///
    /// 走计划任务而不是直接启动服务程序，是为了**不弹 UAC**：
    /// 任务本身是 HighestAvailable，由计划任务服务代为提权启动。
    /// </summary>
    public static bool RunServiceTask(out string error)
    {
        error = string.Empty;

        try
        {
            if (!TryGetTask(out var task))
            {
                error = "计划任务 " + TaskName + " 尚未注册，请先注册开机自启。";
                return false;
            }

            task.Run(null);
            return true;
        }
        catch (Exception ex)
        {
            Diagnostics.Log("启动计划任务失败：" + ex);
            error = DescribeFailure(ex);
            return false;
        }
    }

    /// <summary>
    /// 把**系统里已注册**的任务定义写成文件（UTF-8），供 --export-task 诊断开关使用。
    /// </summary>
    public static bool ExportRegisteredTask(string path, out string error)
    {
        error = string.Empty;

        try
        {
            if (!TryGetTask(out var task))
            {
                error = "计划任务 " + TaskName + " 尚未注册。";
                return false;
            }

            var text = new StringBuilder();
            text.Append("=== RemapCopilotKey 自启任务 ===").Append("\r\n");
            text.Append("时间: ").Append(DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss")).Append("\r\n\r\n");

            var definition = task.Definition;
            var principal = definition.Principal;
            text.Append("登录方式 (LogonType) : ").Append(principal.LogonType).Append("\r\n");
            text.Append("运行级别 (RunLevel)  : ").Append(principal.RunLevel).Append("\r\n");
            text.Append("主体 UserId         : ").Append(principal.UserId ?? "(无)").Append("\r\n");
            text.Append("主体 GroupId        : ").Append(principal.GroupId ?? "(无)").Append("\r\n");

            if (definition.Actions.Count > 0 && definition.Actions[1] is IExecAction exec)
            {
                text.Append("动作                : ").Append(exec.Path)
                    .Append(" ").Append(exec.Arguments).Append("\r\n");
            }

            text.Append("\r\n--- 完整任务定义 ---\r\n");
            text.Append(task.Xml).Append("\r\n");

            File.WriteAllText(path, text.ToString(), new UTF8Encoding(true));
            return true;
        }
        catch (Exception ex)
        {
            error = DescribeFailure(ex);
            return false;
        }
    }

    // =======================================================================
    // 内部实现
    // =======================================================================

    /// <summary>
    /// 连上本机的任务计划程序服务。
    ///
    /// 每次都用新实例、不缓存：COM 对象有线程亲和性，缓存成静态字段后
    /// 一旦被别的线程用到就会出莫名其妙的问题。
    /// </summary>
    private static ITaskService CreateTaskService()
    {
        var type = Type.GetTypeFromCLSID(new Guid("0F87369F-A4E5-4CFC-BD3E-73E6154572DD"), throwOnError: true)!;
        var service = (ITaskService)Activator.CreateInstance(type)!;
        service.Connect(null, null, null, null);
        return service;
    }

    /// <summary>取已注册的任务；不存在时返回 false 而不是抛异常。</summary>
    private static bool TryGetTask(out IRegisteredTask task)
    {
        task = null!;

        var service = CreateTaskService();
        var root = service.GetFolder("\\");

        try
        {
            task = root.GetTask(TaskName);
            return true;
        }
        catch
        {
            // 任务不存在时 GetTask 抛 FILENAME_NOT_FOUND，这里当"没有"处理
            return false;
        }
    }

    /// <summary>把 COM 异常翻译成可以直接弹给用户的一段话。</summary>
    private static string DescribeFailure(Exception ex)
    {
        var text = new StringBuilder();
        text.Append("计划任务操作失败（HRESULT 0x")
            .Append((ex.HResult & 0xFFFFFFFFL).ToString("X8"))
            .Append("）。\n\n")
            .Append(ex.Message);

        text.Append("\n\n排查方向：\n")
            .Append("· 权限不足：注册开机自启需要管理员权限，确认配置程序是以管理员身份运行的。\n")
            .Append("· 路径问题：确认 ")
            .Append(ServiceControl.ServiceExeName)
            .Append(" 与本程序在同一个目录下。\n")
            .Append("· 任务残留：到「任务计划程序」里删除名为 ")
            .Append(TaskName)
            .Append(" 的任务后重试。");

        return text.ToString();
    }
}
