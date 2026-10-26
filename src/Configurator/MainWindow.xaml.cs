using System.Collections.ObjectModel;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Threading;
using Microsoft.Win32;

namespace RemapCopilotKey.Configurator;

// ---------------------------------------------------------------------------
// 主窗口 —— 编辑 config.json + 管理服务进程
//
// 定位：配置程序**不参与按键拦截**，只做两件事：
//   1. 编辑配置并写入 config.json（服务程序热加载）
//   2. 用四个按钮管理服务进程（启动 / 停止 / 注册自启 / 结束并清除）
//
// 这是从 WinForms 迁移到 WPF 的版本，界面改成了 WinUpdatePauser 那套
// 浅色 Win11 卡片风格（Themes/Styles.xaml）。
//
// 文本仍然全部由代码设置（不用 XAML 绑定到资源），原因和原来一样：
// 多语言需要在运行时切换全部文本，代码设置更容易保持一致，
// 也不会在改语言时产生大量噪音 diff。真正需要联动的只有
// "某个单选按钮是否勾选 → 对应那一组控件是否可用"，
// 这一层用 ElementName 绑定（见 MainWindow.xaml）。
// ---------------------------------------------------------------------------

public partial class MainWindow : Window
{
    /// <summary>开源地址。同时也是"打开网页"的默认值。</summary>
    public const string ProjectUrl = "https://github.com/Anlerway/RemapCopilotKey";

    private static readonly string[] LogLevels = { "off", "error", "warn", "info", "debug" };

    /// <summary>三个组合键捕获框（XAML 里是独立控件，这里合成数组统一处理）。</summary>
    private HotkeyBox[] _chordBoxes = Array.Empty<HotkeyBox>();

    private readonly ObservableCollection<LanguageItem> _languages = new();
    private readonly ObservableCollection<LogLevelItem> _logLevels = new();
    private readonly ObservableCollection<SystemActionEntry> _systemActions = new();

    private readonly DispatcherTimer _statusTimer = new();

    /// <summary>自动保存的防抖计时器：停手 600ms 才落盘，避免边打字边写文件。</summary>
    private readonly DispatcherTimer _autoSaveTimer = new();

    private AppConfig _config = new();

    /// <summary>最近一次"已保存/已加载"状态的 JSON，用于判断有没有未保存的改动。</summary>
    private string _savedJson = string.Empty;

    private bool _loading;

    public MainWindow()
    {
        InitializeComponent();

        // 窗口宽度固定、高度由内容决定；这里只把它限制在屏幕工作区内。
        // 内容本身在 ScrollViewer 里，所以超出部分照样能滚到，
        // 只是不必为了 5 行内容把窗口开得比屏幕还高。
        Loaded += (_, _) => ClampToWorkArea();

        // 窗口高度按内容定，但别顶到屏幕外面（小屏 / 125% 缩放时尤其明显）。
        // 内容本身在 ScrollViewer 里，所以超出部分照样能滚到。
        var workArea = SystemParameters.WorkArea;
        MaxWidth = Math.Max(700, workArea.Width * 0.95);
        if (Width > MaxWidth) Width = MaxWidth;

        _chordBoxes = new[] { HotkeyChord0, HotkeyChord1, HotkeyChord2 };

        // 一切从磁盘上的配置开始（语言也来自它）
        LoadConfigFromDisk(showError: false);

        // 单选按钮与三个捕获框的事件（捕获到按键后刷新提示文字）
        foreach (var box in _chordBoxes)
        {
            box.KeyCaptured += OnHotkeyCaptured;
        }
        HotkeySendKey.KeyCaptured += OnHotkeyCaptured;

        // 自动保存：任何控件变化都只排队，停手 600ms 后统一写盘。
        _autoSaveTimer.Interval = TimeSpan.FromMilliseconds(600);
        _autoSaveTimer.Tick += OnAutoSaveTick;
        HookAutoSaveEvents();

        SystemActionCombo.ItemsSource = _systemActions;
        SystemActionCombo.DisplayMemberPath = nameof(SystemActionEntry.Display);
        SystemActionCombo.SelectedValuePath = nameof(SystemActionEntry.Id);

        LogLevelCombo.ItemsSource = _logLevels;
        LogLevelCombo.DisplayMemberPath = nameof(LogLevelItem.Label);
        LogLevelCombo.SelectedValuePath = nameof(LogLevelItem.Code);

        LanguageCombo.ItemsSource = _languages;
        LanguageCombo.DisplayMemberPath = nameof(LanguageItem.Name);
        LanguageCombo.SelectedValuePath = nameof(LanguageItem.Code);

        ApplyLocalization();
        LoadConfigIntoUi();
        RefreshStatus();

        Localization.LanguageChanged += OnLanguageChanged;

        _statusTimer.Interval = TimeSpan.FromSeconds(2);
        _statusTimer.Tick += (_, _) => RefreshStatus();
        _statusTimer.Start();

        Closing += OnWindowClosing;
    }

    protected override void OnClosed(EventArgs e)
    {
        Localization.LanguageChanged -= OnLanguageChanged;
        _statusTimer.Stop();
        _autoSaveTimer.Stop();
        base.OnClosed(e);
    }

    /// <summary>
    /// SizeToContent 会把窗口撑到内容的全高，这里再收进屏幕工作区，
    /// 保证标题栏和底部操作栏始终看得见（剩余内容靠 ScrollViewer 滚）。
    /// </summary>
    private void ClampToWorkArea()
    {
        var workArea = SystemParameters.WorkArea;
        var maxHeight = Math.Max(320, workArea.Height - 24);

        // 再留一点余量给窗口边框，免得任务栏方向算不进去
        if (ActualHeight > maxHeight)
        {
            SizeToContent = SizeToContent.Manual;
            Height = maxHeight;
        }
    }

    // -----------------------------------------------------------------------
    // 多语言
    // -----------------------------------------------------------------------

    private void OnLanguageChanged()
    {
        ApplyLocalization();
        RefreshStatus();
    }

    private void ApplyLocalization()
    {
        _loading = true;
        try
        {
            Title = Localization.T("app.title");
            TitleText.Text = Localization.T("app.title");
            LanguageLabel.Text = Localization.T("label.language");

            MappingCardTitle.Text = Localization.T("group.mapping");
            RadioSendKey.Content = Localization.T("radio.sendKey");
            RadioSendChord.Content = Localization.T("radio.sendChord");
            RadioSystemAction.Content = Localization.T("radio.systemAction");
            RadioOpenApp.Content = Localization.T("radio.openApp");
            RadioOpenUrl.Content = Localization.T("radio.openUrl");

            ButtonClearSendKey.Content = Localization.T("button.clear");
            ButtonCancelSendKey.Content = Localization.T("button.cancelCapture");
            ButtonClearChord.Content = Localization.T("button.clear");
            ButtonCancelChord.Content = Localization.T("button.cancelCapture");

            SystemActionLabel.Text = Localization.T("label.systemAction");
            AppPathLabel.Text = Localization.T("label.appPath");
            AppArgsLabel.Text = Localization.T("label.appArgs");
            UrlLabel.Text = Localization.T("label.url");
            BrowseButton.Content = Localization.T("button.browse");

            CheckRunAsAdmin.Content = Localization.T("check.runAsAdmin");
            CheckToggleWindow.Content = Localization.T("check.toggleWindow");
            CheckRunAsAdmin.ToolTip = Localization.T("hint.runAsAdmin");
            CheckToggleWindow.ToolTip = Localization.T("hint.toggleWindow");
            AppArgsBox.ToolTip = Localization.T("hint.appArgsPlaceholder");
            foreach (var box in _chordBoxes) box.ToolTip = Localization.T("hint.hotkey");
            HotkeySendKey.ToolTip = Localization.T("hint.hotkey");

            GeneralCardTitle.Text = Localization.T("group.general");
            CheckEnabled.Content = Localization.T("check.enabled");
            CheckReloadOnChange.Content = Localization.T("check.reloadOnChange");
            LogLevelLabel.Text = Localization.T("label.logLevel");

            ServiceCardTitle.Text = Localization.T("group.service");
            ButtonStartService.Content = Localization.T("button.startService");
            ButtonStopService.Content = Localization.T("button.stopService");
            ButtonRegisterService.Content = Localization.T("button.registerService");
            ButtonRemoveService.Content = Localization.T("button.removeService");

            ButtonRestoreDefaults.Content = Localization.T("button.restoreDefaults");
            ButtonReload.Content = Localization.T("button.reload");
            ButtonClose.Content = Localization.T("button.close");
            ButtonProjectUrl.Content = Localization.T("button.projectUrl");

            // 自动保存提示（切换语言时回到默认文案，下一次改动会再刷新）
            ShowSaveHint(Localization.T("status.autoSave"), isWarning: false);

            // 配置文件路径放在服务卡片里（底部栏太窄，长路径会被挤到换行）
            ConfigPathText.Text = Localization.T("status.configPath") + " " + ServiceControl.ConfigPath;
            ConfigPathText.ToolTip = ServiceControl.ConfigPath;
            ButtonProjectUrl.ToolTip = ProjectUrl;

            // 捕获框的占位文字来自语言包，语言变了要整体刷新
            HotkeyBox.InvalidateDisplayNames();
            foreach (var box in _chordBoxes) box.RefreshLocalization();
            HotkeySendKey.RefreshLocalization();

            RebuildLanguageItems();
            RebuildSystemActionItems();
            RebuildLogLevelItems();
        }
        finally
        {
            _loading = false;
        }
    }

    private void RebuildLanguageItems()
    {
        var current = LanguageCombo.SelectedValue as string ?? _config.Language;

        _languages.Clear();
        foreach (var pack in Localization.Packs)
        {
            _languages.Add(new LanguageItem(pack.Code, pack.Name));
        }

        SelectByValue(LanguageCombo, current, fallbackIndex: 0);
    }

    private void RebuildSystemActionItems()
    {
        var current = SystemActionCombo.SelectedValue as string
                      ?? _config.Mapping.Action.Action
                      ?? "explorer";

        _systemActions.Clear();
        foreach (var entry in SystemActionCatalog.All)
        {
            _systemActions.Add(entry);
        }

        SelectByValue(SystemActionCombo, current, fallbackIndex: 0);
    }

    private void RebuildLogLevelItems()
    {
        var current = LogLevelCombo.SelectedValue as string ?? _config.LogLevel;

        _logLevels.Clear();
        foreach (var level in LogLevels)
        {
            _logLevels.Add(new LogLevelItem(level, Localization.T("log." + level)));
        }

        if (SelectByValue(LogLevelCombo, current, fallbackIndex: -1)) return;

        // 配置里的级别不认识时退回 error（与默认值一致）
        if (!SelectByValue(LogLevelCombo, "error", fallbackIndex: -1) && _logLevels.Count > 0)
        {
            LogLevelCombo.SelectedIndex = 0;
        }
    }

    /// <summary>按 SelectedValuePath 选中一项；选不到时用 fallbackIndex（-1 表示不动）。</summary>
    private static bool SelectByValue(ComboBox combo, string? value, int fallbackIndex)
    {
        foreach (var item in combo.Items)
        {
            if (item is LanguageItem language &&
                string.Equals(language.Code, value, StringComparison.OrdinalIgnoreCase))
            {
                combo.SelectedItem = item;
                return true;
            }

            if (item is LogLevelItem level &&
                string.Equals(level.Code, value, StringComparison.OrdinalIgnoreCase))
            {
                combo.SelectedItem = item;
                return true;
            }

            if (item is SystemActionEntry action &&
                string.Equals(action.Id, value, StringComparison.OrdinalIgnoreCase))
            {
                combo.SelectedItem = item;
                return true;
            }
        }

        if (fallbackIndex >= 0 && combo.Items.Count > fallbackIndex)
        {
            combo.SelectedIndex = fallbackIndex;
            return true;
        }

        return false;
    }

    private void OnLanguageSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading) return;
        if (LanguageCombo.SelectedItem is not LanguageItem item) return;

        _config.Language = item.Code;
        Localization.SetLanguage(item.Code);
        QueueAutoSave();
    }

    // -----------------------------------------------------------------------
    // 配置 <-> 界面
    // -----------------------------------------------------------------------

    private void LoadConfigFromDisk(bool showError)
    {
        try
        {
            _config = AppConfig.Load(ServiceControl.ConfigPath);
        }
        catch (Exception ex)
        {
            if (showError)
            {
                MessageBox.Show(
                    this,
                    Localization.T("msg.loadFailed.body", ServiceControl.ConfigPath, ex.Message),
                    Localization.T("msg.loadFailed.title"),
                    MessageBoxButton.OK, MessageBoxImage.Warning);
            }
            _config = new AppConfig();
        }
    }

    private void LoadConfigIntoUi()
    {
        LoadConfigFromDisk(showError: true);
        ApplyConfigToUi();
    }

    /// <summary>把 _config 的内容铺到界面控件上（不读磁盘）。</summary>
    private void ApplyConfigToUi()
    {
        _loading = true;
        try
        {
            if (!string.IsNullOrWhiteSpace(_config.Language))
            {
                Localization.SetLanguage(_config.Language);
            }

            CheckEnabled.IsChecked = _config.Enabled;
            CheckReloadOnChange.IsChecked = _config.Mapping.ReloadOnChange;
            SelectByValue(LogLevelCombo, _config.LogLevel, fallbackIndex: -1);

            var action = _config.Mapping.Action;
            switch (action.Type)
            {
                case ActionTypes.SendChord: RadioSendChord.IsChecked = true; break;
                case ActionTypes.SystemAction: RadioSystemAction.IsChecked = true; break;
                case ActionTypes.OpenApp: RadioOpenApp.IsChecked = true; break;
                case ActionTypes.OpenUrl: RadioOpenUrl.IsChecked = true; break;
                default: RadioSendKey.IsChecked = true; break;
            }

            var singleName = action.Type == ActionTypes.SendKey ? action.Vkey : "VK_RCONTROL";
            var singleEntry = KeyCatalog.Find(singleName) ?? KeyCatalog.Find("VK_RCONTROL");
            HotkeySendKey.VirtualKey = singleEntry?.Vk ?? 0;

            var chordVks = new List<int>();
            if (action.Keys is not null)
            {
                foreach (var name in action.Keys)
                {
                    var entry = KeyCatalog.Find(name);
                    if (entry is not null) chordVks.Add(entry.Vk);
                }
            }
            if (chordVks.Count == 0) chordVks.AddRange(new[] { 0xA2, 0xA0, 0x1B });   // 默认 左Ctrl+左Shift+Esc

            for (var i = 0; i < _chordBoxes.Length; i++)
            {
                _chordBoxes[i].VirtualKey = i < chordVks.Count ? chordVks[i] : 0;
            }

            AppPathBox.Text = action.Path ?? string.Empty;
            AppArgsBox.Text = action.Args ?? string.Empty;
            CheckRunAsAdmin.IsChecked = action.RunAsAdmin;
            CheckToggleWindow.IsChecked = action.ToggleWindow;
            UrlBox.Text = action.Url ?? string.Empty;

            RebuildSystemActionItems();
            SelectByValue(SystemActionCombo, action.Action ?? "explorer", fallbackIndex: -1);
        }
        finally
        {
            _loading = false;
        }

        UpdateHintAndEnabledStates();

        // 记录基线，用于判断"有没有未保存的改动"
        _savedJson = CollectConfigFromUi().ToJson();
    }

    private AppConfig CollectConfigFromUi()
    {
        var action = new ActionConfig();

        if (RadioSendChord.IsChecked == true)
        {
            action.Type = ActionTypes.SendChord;
            var keys = new List<string>();
            foreach (var box in _chordBoxes)
            {
                if (!box.IsEmpty) keys.Add(box.KeyName);
            }
            action.Keys = keys;
        }
        else if (RadioSystemAction.IsChecked == true)
        {
            action.Type = ActionTypes.SystemAction;
            action.Action = SystemActionCombo.SelectedValue as string ?? "explorer";
        }
        else if (RadioOpenApp.IsChecked == true)
        {
            action.Type = ActionTypes.OpenApp;
            action.Path = AppPathBox.Text.Trim();
            action.Args = AppArgsBox.Text.Trim();
            action.RunAsAdmin = CheckRunAsAdmin.IsChecked == true;
            action.ToggleWindow = CheckToggleWindow.IsChecked == true;
        }
        else if (RadioOpenUrl.IsChecked == true)
        {
            action.Type = ActionTypes.OpenUrl;
            action.Url = UrlBox.Text.Trim();
        }
        else
        {
            action.Type = ActionTypes.SendKey;
            action.Vkey = HotkeySendKey.IsEmpty ? "VK_RCONTROL" : HotkeySendKey.KeyName;
        }

        return new AppConfig
        {
            Version = _config.Version,
            Language = _config.Language,
            LogLevel = LogLevelCombo.SelectedValue as string ?? "error",
            Enabled = CheckEnabled.IsChecked == true,
            Mapping = new MappingConfig
            {
                Trigger = "copilotKey",
                ReloadOnChange = CheckReloadOnChange.IsChecked == true,
                Action = action,
            },
        };
    }

    // -----------------------------------------------------------------------
    // 交互
    // -----------------------------------------------------------------------

    private void OnActionTypeChanged(object sender, RoutedEventArgs e)
    {
        UpdateHintAndEnabledStates();
        QueueAutoSave();
    }

    /// <summary>捕获到按键后刷新提示（与 WinForms 版的 KeyCaptured 回调等价）。</summary>
    private void OnHotkeyCaptured(object? sender, EventArgs e)
    {
        UpdateHintAndEnabledStates();
        QueueAutoSave();
    }

    /// <summary>界面任何一处改动都走这里：排队自动保存。签名兼容 Routed/TextChanged 事件。</summary>
    private void OnConfigChanged(object sender, EventArgs e) => QueueAutoSave();

    private void OnOpenUrlChecked(object sender, RoutedEventArgs e)
    {
        // 勾选"打开网页"且网址还是空的，就填上开源地址作为默认值
        if (UrlBox is not null && UrlBox.Text.Trim().Length == 0)
        {
            UrlBox.Text = ProjectUrl;
        }
        UpdateHintAndEnabledStates();
    }

    /// <summary>
    /// 组合键那一组控件没有用绑定（HotkeyBox 的 IsEnabled 会影响其文字，
    /// 而且需要和三个 "+" 一起联动），统一在这里处理。
    /// </summary>
    private void UpdateHintAndEnabledStates()
    {
        if (_chordBoxes.Length == 0) return;

        var chord = RadioSendChord.IsChecked == true;
        foreach (var box in _chordBoxes) box.IsEnabled = chord;
        ButtonClearChord.IsEnabled = chord;
        ButtonCancelChord.IsEnabled = chord;
    }

    private void OnClearSendKeyClick(object sender, RoutedEventArgs e)
    {
        HotkeySendKey.ClearKey();
        QueueAutoSave();
    }

    private void OnCancelSendKeyClick(object sender, RoutedEventArgs e) => HotkeySendKey.CancelCapture();

    private void OnClearChordClick(object sender, RoutedEventArgs e)
    {
        foreach (var box in _chordBoxes) box.ClearKey();
        QueueAutoSave();
    }

    private void OnCancelChordClick(object sender, RoutedEventArgs e)
    {
        foreach (var box in _chordBoxes) box.CancelCapture();
    }

    private void OnBrowseClick(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog
        {
            Title = Localization.T("label.appPath"),
            // 覆盖 Windows 上所有可执行类型与快捷方式
            Filter = "所有可执行程序与快捷方式|*.exe;*.com;*.bat;*.cmd;*.lnk;*.msc;*.ps1;*.vbs;*.js;*.jar;*.py"
                     + "|可执行文件 (*.exe;*.com)|*.exe;*.com"
                     + "|批处理 (*.bat;*.cmd)|*.bat;*.cmd"
                     + "|快捷方式 (*.lnk)|*.lnk"
                     + "|所有文件 (*.*)|*.*",
            CheckFileExists = true,
        };

        var current = AppPathBox.Text.Trim();
        if (File.Exists(current))
        {
            dialog.InitialDirectory = Path.GetDirectoryName(current);
            dialog.FileName = Path.GetFileName(current);
        }

        if (dialog.ShowDialog(this) == true)
        {
            AppPathBox.Text = dialog.FileName;
        }
    }

    private void OnProjectUrlClick(object sender, RoutedEventArgs e) => OpenProjectUrl();

    private void OpenProjectUrl()
    {
        try
        {
            ShellOpen.OpenUrl(ProjectUrl);
        }
        catch (Exception ex)
        {
            // 打不开浏览器时把地址显示出来，方便手动复制
            MessageBox.Show(this, ProjectUrl + "\n\n" + ex.Message,
                Localization.T("msg.error.title"), MessageBoxButton.OK, MessageBoxImage.Warning);
        }
    }

    private void OnReloadClick(object sender, RoutedEventArgs e) => LoadConfigIntoUi();

    private void OnCloseClick(object sender, RoutedEventArgs e) => Close();

    /// <summary>
    /// 恢复默认配置：把内存里的配置换成 <see cref="AppConfig"/> 的默认值，
    /// 铺回界面后立刻写盘（不再需要"保存"按钮）。
    /// </summary>
    private void OnRestoreDefaultsClick(object sender, RoutedEventArgs e)
    {
        var answer = MessageBox.Show(this, Localization.T("msg.confirmRestore.body"),
            Localization.T("msg.confirmRestore.title"), MessageBoxButton.YesNo, MessageBoxImage.Question);
        if (answer != MessageBoxResult.Yes) return;

        _config = new AppConfig();
        ApplyConfigToUi();          // 界面换成默认值，并记录新的基线
        AutoSave();                 // 立刻落盘，不等防抖

        MessageBox.Show(this, Localization.T("msg.restored.body"),
            Localization.T("msg.restored.title"), MessageBoxButton.OK, MessageBoxImage.Information);
        RefreshStatus();
    }

    // --- 自动保存 ---------------------------------------------------------

    /// <summary>把所有会改动配置的控件挂上"改动即排队保存"。</summary>
    private void HookAutoSaveEvents()
    {
        CheckEnabled.Checked += OnConfigChanged;
        CheckEnabled.Unchecked += OnConfigChanged;
        CheckReloadOnChange.Checked += OnConfigChanged;
        CheckReloadOnChange.Unchecked += OnConfigChanged;
        CheckRunAsAdmin.Checked += OnConfigChanged;
        CheckRunAsAdmin.Unchecked += OnConfigChanged;
        CheckToggleWindow.Checked += OnConfigChanged;
        CheckToggleWindow.Unchecked += OnConfigChanged;

        AppPathBox.TextChanged += OnConfigChanged;
        AppArgsBox.TextChanged += OnConfigChanged;
        UrlBox.TextChanged += OnConfigChanged;

        LogLevelCombo.SelectionChanged += OnConfigChanged;
        SystemActionCombo.SelectionChanged += OnConfigChanged;
    }

    /// <summary>排队一次自动保存（防抖 600ms，连续输入只写一次文件）。</summary>
    private void QueueAutoSave()
    {
        if (_loading) return;

        ShowSaveHint(Localization.T("status.autoSaveDirty"), isWarning: false);
        _autoSaveTimer.Stop();
        _autoSaveTimer.Start();
    }

    private void OnAutoSaveTick(object? sender, EventArgs e)
    {
        _autoSaveTimer.Stop();
        AutoSave();
    }

    /// <summary>
    /// 把界面上的配置静默写盘。
    ///
    /// 校验不通过 / 写文件失败都只改底部那行提示，不弹窗 —— 自动保存不能打断输入，
    /// 也不能因为用户正在敲网址就一天弹八次对话框。
    /// </summary>
    private void AutoSave()
    {
        if (_loading) return;

        // 正在捕获按键时界面里还是"按下按键"的中间态，先不落盘
        if (HotkeySendKey.IsCapturing) return;
        foreach (var box in _chordBoxes)
        {
            if (box.IsCapturing) return;
        }

        var config = CollectConfigFromUi();
        var json = config.ToJson();
        if (json == _savedJson) return;             // 没有实质改动

        var problems = config.Validate();
        if (problems.Count > 0)
        {
            ShowSaveHint(Localization.T("status.autoSaveBlocked", string.Join("；", problems)), isWarning: true);
            return;
        }

        try
        {
            var directory = Path.GetDirectoryName(ServiceControl.ConfigPath);
            if (!string.IsNullOrEmpty(directory)) Directory.CreateDirectory(directory);

            config.Save(ServiceControl.ConfigPath);
            _config = config;
            _savedJson = json;

            // 服务有文件监视会自动重载；这里再发一条消息让它立刻生效。
            ServiceControl.SignalReload();
            ShowSaveHint(Localization.T("status.autoSaved", DateTime.Now.ToString("HH:mm:ss")), isWarning: false);
        }
        catch (Exception ex)
        {
            ShowSaveHint(Localization.T("status.saveFailed", ex.Message), isWarning: true);
        }
    }

    /// <summary>底部那行保存状态：灰字=已保存 / 待保存，红字=需要人工处理。</summary>
    private void ShowSaveHint(string text, bool isWarning)
    {
        SaveHintText.Text = text;
        SaveHintText.Foreground = isWarning
            ? new SolidColorBrush(Color.FromRgb(0xB3, 0x1C, 0x1C))
            : new SolidColorBrush(Color.FromRgb(0x60, 0x60, 0x60));
    }

    private void OnWindowClosing(object? sender, System.ComponentModel.CancelEventArgs e)
    {
        // 自动保存是防抖的，关窗口前把还没落盘的那一次补上
        _autoSaveTimer.Stop();
        AutoSave();
    }

    // --- 服务管理 ---------------------------------------------------------

    private void OnStartServiceClick(object sender, RoutedEventArgs e)
    {
        if (ServiceControl.IsRunning)
        {
            RefreshStatus();
            MessageBox.Show(this, Localization.T("msg.startOk.body"),
                Localization.T("msg.info.title"), MessageBoxButton.OK, MessageBoxImage.Information);
            return;
        }

        if (ServiceControl.StartService(out var error))
        {
            ServiceControl.WaitForStart();

            MessageBox.Show(this, Localization.T("msg.startOk.body"),
                Localization.T("msg.startOk.title"), MessageBoxButton.OK, MessageBoxImage.Information);
        }
        else
        {
            MessageBox.Show(this,
                Localization.T("msg.startFailed.body", ServiceControl.ServiceExePath, error),
                Localization.T("msg.startFailed.title"), MessageBoxButton.OK, MessageBoxImage.Error);
        }

        RefreshStatus();
    }

    private void OnStopServiceClick(object sender, RoutedEventArgs e)
    {
        if (!ServiceControl.IsRunning && !ServiceInstaller.IsInstalled())
        {
            RefreshStatus();
            return;
        }

        if (ServiceControl.StopService(out var error))
        {
            MessageBox.Show(this, Localization.T("msg.stopOk.body"),
                Localization.T("msg.stopOk.title"), MessageBoxButton.OK, MessageBoxImage.Information);
        }
        else
        {
            MessageBox.Show(this, Localization.T("msg.stopFailed.body", error),
                Localization.T("msg.stopFailed.title"), MessageBoxButton.OK, MessageBoxImage.Warning);
        }

        RefreshStatus();
    }

    /// <summary>
    /// 注册开机自启。
    ///
    /// 注册一个 LocalSystem 的自动启动服务（见 ServiceInstaller.cs）：开机后由
    /// SCM 启动服务宿主，服务再把高完整性的钩子进程投递进用户会话 ——
    /// 静默提权、全程不弹 UAC。注册完顺手清掉旧版本留下的计划任务。
    /// </summary>
    private void OnRegisterServiceClick(object sender, RoutedEventArgs e)
    {
        if (ServiceInstaller.Install(ServiceControl.ServiceExePath, out var error))
        {
            MessageBox.Show(this, Localization.T("msg.registered.body"),
                Localization.T("msg.registered.title"), MessageBoxButton.OK, MessageBoxImage.Information);
        }
        else
        {
            MessageBox.Show(this, Localization.T("msg.registerFailed.body", error),
                Localization.T("msg.registerFailed.title"), MessageBoxButton.OK, MessageBoxImage.Error);
        }

        RefreshStatus();
    }

    private void OnRemoveServiceClick(object sender, RoutedEventArgs e)
    {
        var answer = MessageBox.Show(this, Localization.T("msg.confirmRemove.body"),
            Localization.T("msg.confirmRemove.title"), MessageBoxButton.YesNo, MessageBoxImage.Warning);
        if (answer != MessageBoxResult.Yes) return;

        // 停服务 + 从 SCM 删除注册 + 清掉旧版本的计划任务，一次做完
        if (!ServiceInstaller.Uninstall(out var error))
        {
            MessageBox.Show(this, Localization.T("msg.unregisterFailed.body", error),
                Localization.T("msg.unregisterFailed.title"), MessageBoxButton.OK, MessageBoxImage.Error);
            RefreshStatus();
            return;
        }

        MessageBox.Show(this, Localization.T("msg.removed.body"),
            Localization.T("msg.removed.title"), MessageBoxButton.OK, MessageBoxImage.Information);
        RefreshStatus();
    }

    private void RefreshStatus()
    {
        // running   = 钩子进程在跑（这才代表"按键真的被接管了"）
        // registered= 服务已注册到 SCM（代表"下次开机自动起来"）
        var running = ServiceControl.IsRunning;
        var registered = ServiceInstaller.IsInstalled();

        var autoStartText = registered
            ? Localization.T("status.autostartOn")
            : Localization.T("status.autostartOff");

        ServiceStatusText.Text =
            (running ? Localization.T("status.serviceRunning") : Localization.T("status.serviceStopped"))
            + "    |    " + autoStartText;
        ServiceStatusText.Foreground = running
            ? new SolidColorBrush(Color.FromRgb(0x10, 0x7C, 0x10))
            : new SolidColorBrush(Color.FromRgb(0x96, 0x5A, 0x00));

        StatusText.Text = running
            ? Localization.T("status.serviceRunning")
            : Localization.T("status.serviceStopped");
        StatusText.Foreground = ServiceStatusText.Foreground;

        ButtonStartService.IsEnabled = !running;
        ButtonStopService.IsEnabled = running;
        ButtonRemoveService.IsEnabled = running || registered;
    }

    // -----------------------------------------------------------------------
    // 小类型
    // -----------------------------------------------------------------------

    private sealed record LanguageItem(string Code, string Name);

    private sealed record LogLevelItem(string Code, string Label);
}
