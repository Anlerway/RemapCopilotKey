namespace RemapCopilotKey.Configurator;

// ---------------------------------------------------------------------------
// 键位目录
//
// Name 必须与 C++ 侧 Keys.cpp 的规范名完全一致（"VK_RCONTROL" 这种），
// 因为配置文件里存的就是它；Vk 用于把"按下的实体键"映射回规范名（输入捕获）。
// 中文/英文名只用于界面显示。
// ---------------------------------------------------------------------------

public sealed record KeyEntry(string Name, int Vk, string Zh, string En);

public static class KeyCatalog
{

    public static IReadOnlyList<KeyEntry> All { get; } = Build();

    public static string Display(KeyEntry entry) =>
        Localization.IsChinese ? entry.Zh : entry.En;

    public static KeyEntry? Find(string? name)
    {
        if (string.IsNullOrWhiteSpace(name)) return null;
        return All.FirstOrDefault(k => string.Equals(k.Name, name, StringComparison.OrdinalIgnoreCase));
    }

    /// <summary>按虚拟键码查找（输入捕获时用）。</summary>
    public static KeyEntry? FindByVirtualKey(int vk) =>
        All.FirstOrDefault(k => k.Vk == vk);

    /// <summary>把规范名转成界面显示名；找不到就原样返回规范名。</summary>
    public static string DisplayName(string? name)
    {
        var entry = Find(name);
        return entry is null ? (name ?? string.Empty) : Display(entry);
    }

    /// <summary>把虚拟键码转成显示名；找不到就给一个可读的十六进制形式。</summary>
    public static string DisplayNameByVirtualKey(int vk)
    {
        var entry = FindByVirtualKey(vk);
        return entry is null ? $"0x{vk:X2}" : Display(entry);
    }

    private static List<KeyEntry> Build()
    {
        var list = new List<KeyEntry>();

        void Add(string name, int vk, string zh, string en) =>
            list.Add(new KeyEntry(name, vk, zh, en));

        // --- 字母（虚拟键码就是大写 ASCII 值）------------------------------
        for (var c = 'A'; c <= 'Z'; c++)
        {
            Add($"VK_{c}", c, c.ToString(), c.ToString());
        }

        // --- 数字 ---------------------------------------------------------
        for (var c = '0'; c <= '9'; c++)
        {
            Add($"VK_{c}", c, c.ToString(), c.ToString());
        }

        // --- 功能键 -------------------------------------------------------
        for (var i = 1; i <= 24; i++)
        {
            Add($"VK_F{i}", 0x70 + (i - 1), $"F{i}", $"F{i}");
        }

        // --- 修饰键 -------------------------------------------------------
        Add("VK_CONTROL",  0x11, "Ctrl（通用）",  "Ctrl (generic)");
        Add("VK_SHIFT",    0x10, "Shift（通用）", "Shift (generic)");
        Add("VK_MENU",     0x12, "Alt（通用）",   "Alt (generic)");
        Add("VK_LCONTROL", 0xA2, "左 Ctrl",       "Left Ctrl");
        Add("VK_RCONTROL", 0xA3, "右 Ctrl",       "Right Ctrl");
        Add("VK_LSHIFT",   0xA0, "左 Shift",      "Left Shift");
        Add("VK_RSHIFT",   0xA1, "右 Shift",      "Right Shift");
        Add("VK_LMENU",    0xA4, "左 Alt",        "Left Alt");
        Add("VK_RMENU",    0xA5, "右 Alt",        "Right Alt");
        Add("VK_LWIN",     0x5B, "左 Windows",    "Left Windows");
        Add("VK_RWIN",     0x5C, "右 Windows",    "Right Windows");

        // --- 导航键 -------------------------------------------------------
        Add("VK_LEFT",  0x25, "← 左方向键", "Left arrow");
        Add("VK_RIGHT", 0x27, "→ 右方向键", "Right arrow");
        Add("VK_UP",    0x26, "↑ 上方向键", "Up arrow");
        Add("VK_DOWN",  0x28, "↓ 下方向键", "Down arrow");
        Add("VK_HOME",  0x24, "Home",       "Home");
        Add("VK_END",   0x23, "End",        "End");
        Add("VK_PRIOR", 0x21, "Page Up",    "Page Up");
        Add("VK_NEXT",  0x22, "Page Down",  "Page Down");

        // --- 编辑键 -------------------------------------------------------
        Add("VK_INSERT",   0x2D, "Insert",         "Insert");
        Add("VK_DELETE",   0x2E, "Delete",         "Delete");
        Add("VK_BACK",     0x08, "退格 Backspace", "Backspace");
        Add("VK_TAB",      0x09, "Tab",            "Tab");
        Add("VK_RETURN",   0x0D, "回车 Enter",     "Enter");
        Add("VK_ESCAPE",   0x1B, "Esc",            "Esc");
        Add("VK_SPACE",    0x20, "空格",           "Space");
        Add("VK_CAPITAL",  0x14, "大写锁定",       "Caps Lock");
        Add("VK_APPS",     0x5D, "菜单键",         "Menu key");
        Add("VK_SNAPSHOT", 0x2C, "Print Screen",   "Print Screen");
        Add("VK_SCROLL",   0x91, "Scroll Lock",    "Scroll Lock");
        Add("VK_PAUSE",    0x13, "Pause",          "Pause");
        Add("VK_NUMLOCK",  0x90, "Num Lock",       "Num Lock");
        Add("VK_CLEAR",    0x0C, "Clear",          "Clear");
        Add("VK_HELP",     0x2F, "Help",           "Help");
        Add("VK_SLEEP",    0x5F, "睡眠",           "Sleep");

        // --- 小键盘 -------------------------------------------------------
        for (var i = 0; i <= 9; i++)
        {
            Add($"VK_NUMPAD{i}", 0x60 + i, $"小键盘 {i}", $"Numpad {i}");
        }
        Add("VK_MULTIPLY",  0x6A, "小键盘 *",      "Numpad *");
        Add("VK_ADD",       0x6B, "小键盘 +",      "Numpad +");
        Add("VK_SUBTRACT",  0x6D, "小键盘 -",      "Numpad -");
        Add("VK_DECIMAL",   0x6E, "小键盘 .",      "Numpad .");
        Add("VK_DIVIDE",    0x6F, "小键盘 /",      "Numpad /");
        Add("VK_SEPARATOR", 0x6C, "小键盘分隔符",  "Numpad separator");

        // --- 媒体键 -------------------------------------------------------
        Add("VK_VOLUME_MUTE",        0xAD, "静音",        "Mute");
        Add("VK_VOLUME_DOWN",        0xAE, "音量 -",      "Volume down");
        Add("VK_VOLUME_UP",          0xAF, "音量 +",      "Volume up");
        Add("VK_MEDIA_NEXT_TRACK",   0xB0, "下一曲",      "Next track");
        Add("VK_MEDIA_PREV_TRACK",   0xB1, "上一曲",      "Previous track");
        Add("VK_MEDIA_STOP",         0xB2, "停止播放",    "Stop media");
        Add("VK_MEDIA_PLAY_PAUSE",   0xB3, "播放 / 暂停", "Play / Pause");
        Add("VK_BROWSER_BACK",       0xA6, "浏览器后退",  "Browser back");
        Add("VK_BROWSER_FORWARD",    0xA7, "浏览器前进",  "Browser forward");
        Add("VK_BROWSER_REFRESH",    0xA8, "浏览器刷新",  "Browser refresh");
        Add("VK_BROWSER_HOME",       0xAC, "浏览器主页",  "Browser home");
        Add("VK_LAUNCH_MAIL",        0xB4, "打开邮件",    "Launch mail");
        Add("VK_LAUNCH_MEDIA_SELECT",0xB5, "打开媒体",    "Launch media");
        Add("VK_LAUNCH_APP1",        0xB6, "启动应用 1",  "Launch app 1");
        Add("VK_LAUNCH_APP2",        0xB7, "启动应用 2",  "Launch app 2");

        // --- 符号键（OEM）-------------------------------------------------
        Add("VK_OEM_1",      0xBA, "; :",  "; :");
        Add("VK_OEM_PLUS",   0xBB, "= +",  "= +");
        Add("VK_OEM_COMMA",  0xBC, ", <",  ", <");
        Add("VK_OEM_MINUS",  0xBD, "- _",  "- _");
        Add("VK_OEM_PERIOD", 0xBE, ". >",  ". >");
        Add("VK_OEM_2",      0xBF, "/ ?",  "/ ?");
        Add("VK_OEM_3",      0xC0, "` ~",  "` ~");
        Add("VK_OEM_4",      0xDB, "[ {",  "[ {");
        Add("VK_OEM_5",      0xDC, "\\ |", "\\ |");
        Add("VK_OEM_6",      0xDD, "] }",  "] }");
        Add("VK_OEM_7",      0xDE, "' \"", "' \"");
        Add("VK_OEM_8",      0xDF, "OEM 8", "OEM 8");
        Add("VK_OEM_102",    0xE2, "OEM 102（<> 键）", "OEM 102");
        Add("VK_OEM_AX",     0xE1, "OEM AX", "OEM AX");
        Add("VK_OEM_CLEAR",  0xFE, "OEM Clear", "OEM Clear");

        return list;
    }
}

// ---------------------------------------------------------------------------
// Windows 实用功能目录
// Id 必须与 C++ 侧 Config.cpp 的 kSystemActions 表一致。
// ---------------------------------------------------------------------------

public sealed record SystemActionEntry(string Id, string Zh, string En)
{
    /// <summary>
    /// 当前语言下的显示名。用属性而不是方法：
    /// 下拉框通过 DisplayMemberPath 绑定它，切换语言时记录本身不变，
    /// 由主窗口重建列表来触发重新取值。
    /// </summary>
    public string Display => SystemActionCatalog.Display(this);
}

public static class SystemActionCatalog
{
    public static IReadOnlyList<SystemActionEntry> All { get; } = new[]
    {
        new SystemActionEntry("explorer",         "文件资源管理器",        "File Explorer"),
        new SystemActionEntry("taskManager",      "任务管理器",            "Task Manager"),
        new SystemActionEntry("cmd",              "命令提示符 (cmd)",      "Command Prompt (cmd)"),
        new SystemActionEntry("powershell",       "PowerShell",            "PowerShell"),
        new SystemActionEntry("screenshot",       "截图（Win+Shift+S）",   "Screenshot (Win+Shift+S)"),
        new SystemActionEntry("snippingTool",     "截图工具",              "Snipping Tool"),
        new SystemActionEntry("settings",         "设置",                  "Settings"),
        new SystemActionEntry("controlPanel",     "控制面板",              "Control Panel"),
        new SystemActionEntry("calculator",       "计算器",                "Calculator"),
        new SystemActionEntry("notepad",          "记事本",                "Notepad"),
        new SystemActionEntry("taskView",         "任务视图（Win+Tab）",   "Task View (Win+Tab)"),
        new SystemActionEntry("clipboardHistory", "剪贴板历史（Win+V）",   "Clipboard history (Win+V)"),
        new SystemActionEntry("emojiPanel",       "表情符号（Win+.）",     "Emoji panel (Win+.)"),
        new SystemActionEntry("lockScreen",       "锁定屏幕（Win+L）",     "Lock screen (Win+L)"),
    };

    public static string Display(SystemActionEntry entry) =>
        Localization.IsChinese ? entry.Zh : entry.En;

    public static SystemActionEntry? Find(string? id) =>
        string.IsNullOrWhiteSpace(id)
            ? null
            : All.FirstOrDefault(a => string.Equals(a.Id, id, StringComparison.OrdinalIgnoreCase));
}
