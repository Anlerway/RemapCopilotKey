using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace RemapCopilotKey.Configurator;

// ---------------------------------------------------------------------------
// 单键捕获框（WPF 版，替代原来的 WinForms HotkeyBox）
//
// 每个框只捕获**一个**按键；组合键由多个这样的框拼起来（框 + 框 + 框）。
//
// 交互（与原实现保持一致）：
//   * 点击框 → 进入捕获状态，**原来的按键先被清空**
//   * 捕获状态下显示 `<按下键盘任意按键捕获>`
//   * 按任意键 → 捕获该键，**立即退出捕获状态**
//   * 空框显示 `<空白>`
//   * 控件被禁用（例如组合键那一组没被选中）时不画任何文字
//   * "取消捕获"按钮 → 中途放弃捕获（框保持为空）
//   * "清空"按钮 → 直接清掉已捕获的按键
//
// 三个关键细节：
//   1. 区分左右修饰键。WPF 的 Key 事件对 Ctrl/Shift/Alt 只给出通用键码，
//      所以捕获时用 GetAsyncKeyState 判断到底按的是左键还是右键。
//   2. 屏蔽非法输入。只接受在 KeyCatalog 里有定义的实体键，
//      这样 0x15(KANA)、0xE5(PROCESSKEY) 这类输入法/伪键码不会被录进来。
//   3. 外观。Win11 浅色卡片里的小方框：白底、圆角、细边框，
//      捕获中 / 聚焦时边框变强调色，禁用时灰底。
//
// 已知限制：Windows 键被操作系统抢先处理，普通窗口收不到它的按键消息，
// 因此捕获不到 Win。需要 Win 组合请使用预置的系统功能，或手改配置文件。
// ---------------------------------------------------------------------------

public sealed class HotkeyBox : Control
{
    private const int VK_SHIFT = 0x10;
    private const int VK_CONTROL = 0x11;
    private const int VK_MENU = 0x12;
    private const int VK_LSHIFT = 0xA0;
    private const int VK_RSHIFT = 0xA1;
    private const int VK_LCONTROL = 0xA2;
    private const int VK_RCONTROL = 0xA3;
    private const int VK_LMENU = 0xA4;
    private const int VK_RMENU = 0xA5;

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern short GetAsyncKeyState(int vKey);

    // 虚拟键码 -> 显示文字。语言切换时整体失效，避免每个框自己查表。
    private static readonly Dictionary<int, string> s_displayNames = new();
    private static bool s_displayNamesValid;

    static HotkeyBox()
    {
        DefaultStyleKeyProperty.OverrideMetadata(
            typeof(HotkeyBox), new FrameworkPropertyMetadata(typeof(HotkeyBox)));

        // 语言切换时所有框的文字都要跟着变
        Localization.LanguageChanged += InvalidateDisplayNames;
    }

    public static void InvalidateDisplayNames()
    {
        s_displayNamesValid = false;
        s_displayNames.Clear();
    }

    private static string DisplayNameFor(int virtualKey)
    {
        if (!s_displayNamesValid)
        {
            foreach (var entry in KeyCatalog.All)
            {
                s_displayNames[entry.Vk] = KeyCatalog.Display(entry);
            }
            s_displayNamesValid = true;
        }

        return s_displayNames.TryGetValue(virtualKey, out var name)
            ? name
            : $"0x{virtualKey:X2}";
    }

    // -- 依赖属性 -----------------------------------------------------------

    public static readonly DependencyProperty VirtualKeyProperty =
        DependencyProperty.Register(
            nameof(VirtualKey), typeof(int), typeof(HotkeyBox),
            new FrameworkPropertyMetadata(0, OnVirtualKeyChanged));

    /// <summary>捕获到的虚拟键码；0 表示空白。</summary>
    public int VirtualKey
    {
        get => (int)GetValue(VirtualKeyProperty);
        set => SetValue(VirtualKeyProperty, value);
    }

    public static readonly DependencyProperty IsCapturingProperty =
        DependencyProperty.Register(
            nameof(IsCapturing), typeof(bool), typeof(HotkeyBox),
            new FrameworkPropertyMetadata(false, OnVisualStateChanged));

    /// <summary>是否正在等待用户按键。</summary>
    public bool IsCapturing
    {
        get => (bool)GetValue(IsCapturingProperty);
        private set => SetValue(IsCapturingProperty, value);
    }

    public static readonly DependencyPropertyKey IsEmptyPropertyKey =
        DependencyProperty.RegisterReadOnly(
            nameof(IsEmpty), typeof(bool), typeof(HotkeyBox),
            new FrameworkPropertyMetadata(true));

    public static readonly DependencyProperty IsEmptyProperty = IsEmptyPropertyKey.DependencyProperty;

    /// <summary>框是否为空（供模板触发器使用）。</summary>
    public bool IsEmpty => (bool)GetValue(IsEmptyProperty);

    public static readonly DependencyProperty CapturedTextProperty =
        DependencyProperty.Register(
            nameof(CapturedText), typeof(string), typeof(HotkeyBox),
            new FrameworkPropertyMetadata(string.Empty));

    /// <summary>框内显示的文字（供模板绑定；由捕获状态与虚拟键码推导）。</summary>
    public string CapturedText
    {
        get => (string)GetValue(CapturedTextProperty);
        private set => SetValue(CapturedTextProperty, value);
    }

    public static readonly DependencyProperty EmptyTextProperty =
        DependencyProperty.Register(
            nameof(EmptyText), typeof(string), typeof(HotkeyBox),
            new FrameworkPropertyMetadata(string.Empty, OnVisualStateChanged));

    /// <summary>空框占位文字（默认取语言包的 hotkey.empty）。</summary>
    public string EmptyText
    {
        get => (string)GetValue(EmptyTextProperty);
        set => SetValue(EmptyTextProperty, value);
    }

    public static readonly DependencyProperty CapturingTextProperty =
        DependencyProperty.Register(
            nameof(CapturingText), typeof(string), typeof(HotkeyBox),
            new FrameworkPropertyMetadata(string.Empty, OnVisualStateChanged));

    /// <summary>捕获中提示文字（默认取语言包的 hotkey.capturing）。</summary>
    public string CapturingText
    {
        get => (string)GetValue(CapturingTextProperty);
        set => SetValue(CapturingTextProperty, value);
    }

    private static void OnVirtualKeyChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        var box = (HotkeyBox)d;
        box.IsCapturing = false;
        box.SetValue(IsEmptyPropertyKey, box.VirtualKey == 0);
        box.RefreshText();
    }

    private static void OnVisualStateChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        ((HotkeyBox)d).RefreshText();
    }

    // -- 构造 ---------------------------------------------------------------

    public HotkeyBox()
    {
        Focusable = true;
        Cursor = Cursors.Hand;
        MinHeight = 34;
        MinWidth = 120;
        VerticalContentAlignment = VerticalAlignment.Center;
        HorizontalContentAlignment = HorizontalAlignment.Center;

        IsEnabledChanged += (_, _) => RefreshText();
        RefreshText();
    }

    /// <summary>对应的规范键名（"VK_RCONTROL"）；空白返回空串。</summary>
    public string KeyName =>
        VirtualKey == 0
            ? string.Empty
            : KeyCatalog.FindByVirtualKey(VirtualKey)?.Name ?? string.Empty;

    // -- 交互 ---------------------------------------------------------------

    protected override void OnMouseLeftButtonDown(MouseButtonEventArgs e)
    {
        base.OnMouseLeftButtonDown(e);

        Focus();
        BeginCapture();
        e.Handled = true;
    }

    /// <summary>
    /// 失去键盘焦点时结束捕获并卸掉 Win 键钩子。
    /// 不做这一步的话，用户点一下框再点别处（没按键），钩子会一直挂着 ——
    /// 一个全局键盘钩子不该在没人录制的时候存在。
    /// </summary>
    protected override void OnLostKeyboardFocus(KeyboardFocusChangedEventArgs e)
    {
        base.OnLostKeyboardFocus(e);

        StopWinKeyWatch();
        if (IsCapturing) IsCapturing = false;
    }

    protected override void OnPreviewKeyDown(KeyEventArgs e)
    {
        base.OnPreviewKeyDown(e);

        var key = e.Key == Key.System ? e.SystemKey : e.Key;
        var virtualKey = KeyInterop.VirtualKeyFromKey(key);
        if (virtualKey == 0)
        {
            e.Handled = true;
            return;
        }

        if (IsCapturing)
        {
            // 区分左右修饰键
            virtualKey = ResolveModifierSide(virtualKey);

            // 屏蔽输入法 / 伪键码（0x15、0xE5 之类）：只接受键表里有定义的实体键
            if (KeyCatalog.FindByVirtualKey(virtualKey) is null)
            {
                e.Handled = true;
                return;   // 保持捕获状态，等一个合法的键
            }

            VirtualKey = virtualKey;   // 属性回调会退出捕获状态并刷新文字
            e.Handled = true;

            KeyCaptured?.Invoke(this, EventArgs.Empty);
            return;
        }

        // 没在捕获状态时，用回车 / 空格也可以开始捕获（方便纯键盘操作）
        if (key is Key.Enter or Key.Space)
        {
            e.Handled = true;
            BeginCapture();
        }
    }

    /// <summary>捕获到一个按键后触发。</summary>
    public event EventHandler? KeyCaptured;

    /// <summary>进入捕获状态：先清空原有按键，再等待用户按下键盘。</summary>
    public void BeginCapture()
    {
        if (!IsEnabled) return;
        StopWinKeyWatch();
        VirtualKey = 0;
        IsCapturing = true;
        HookWinKey(this);
    }

    /// <summary>取消捕获：退出捕获状态，框保持为空。</summary>
    public void CancelCapture()
    {
        StopWinKeyWatch();
        if (!IsCapturing) return;
        IsCapturing = false;
    }

    /// <summary>清空已捕获的按键。</summary>
    public void ClearKey()
    {
        StopWinKeyWatch();
        VirtualKey = 0;
        IsCapturing = false;
    }

    // -- Win 键 ------------------------------------------------------------
    //
    // 为什么需要单独处理：Windows 键是**操作系统的保留键**。
    // LWin / RWin 的按键消息在到达任何窗口（包括 WPF 的 PreviewKeyDown）之前
    // 就被系统拿去开开始菜单了，所以普通窗口永远收不到它 ——
    // 无论用 KeyDown、PreviewKeyDown 还是 SystemKey 都拿不到。
    //
    // 为了能"录到 Win 键"，捕获期间临时装一个低级键盘钩子（WH_KEYBOARD_LL）。
    // 它在系统处理之前就能看到按键，因此能拿到 LWIN / RWIN。
    // 钩子在退出捕获时立刻卸掉，不做常驻 —— 这是个输入捕获控件，
    // 不应该长年挂着一个全局钩子。

    private const int WH_KEYBOARD_LL = 13;
    private const int WM_KEYDOWN = 0x0100;
    private const int WM_SYSKEYDOWN = 0x0104;
    private const int VK_LWIN = 0x5B;
    private const int VK_RWIN = 0x5C;

    private delegate IntPtr LowLevelKeyboardProc(int nCode, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr SetWindowsHookExW(int idHook, LowLevelKeyboardProc lpfn, IntPtr hMod, uint dwThreadId);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool UnhookWindowsHookEx(IntPtr hhk);

    [DllImport("user32.dll")]
    private static extern IntPtr CallNextHookEx(IntPtr hhk, int nCode, IntPtr wParam, IntPtr lParam);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr GetModuleHandleW(string? lpModuleName);

    private static LowLevelKeyboardProc? s_winKeyProc;      // 委托必须保活，否则会被 GC 回收
    private static HotkeyBox? s_winKeyTarget;               // 同一时间只可能有一个框在捕获
    private static IntPtr s_winKeyHook = IntPtr.Zero;

    private static void HookWinKey(HotkeyBox box)
    {
        StopWinKeyWatch();
        s_winKeyTarget = box;

        try
        {
            s_winKeyProc = WinKeyHookProc;
            s_winKeyHook = SetWindowsHookExW(
                WH_KEYBOARD_LL, s_winKeyProc, GetModuleHandleW(null), 0);
        }
        catch
        {
            s_winKeyHook = IntPtr.Zero;
        }

        if (s_winKeyHook == IntPtr.Zero)
        {
            s_winKeyProc = null;
            s_winKeyTarget = null;
        }
    }

    private static void StopWinKeyWatch()
    {
        if (s_winKeyHook != IntPtr.Zero)
        {
            UnhookWindowsHookEx(s_winKeyHook);
            s_winKeyHook = IntPtr.Zero;
        }
        s_winKeyProc = null;
        s_winKeyTarget = null;
    }

    private static IntPtr WinKeyHookProc(int nCode, IntPtr wParam, IntPtr lParam)
    {
        if (nCode < 0 || s_winKeyTarget is null)
        {
            return CallNextHookEx(IntPtr.Zero, nCode, wParam, lParam);
        }

        var message = wParam.ToInt32();
        if (message is WM_KEYDOWN or WM_SYSKEYDOWN)
        {
            var virtualKey = System.Runtime.InteropServices.Marshal.ReadInt32(lParam);
            if (virtualKey is VK_LWIN or VK_RWIN)
            {
                var target = s_winKeyTarget;

                // 必须在低级钩子回调里完成状态更新：WPF 的 Dispatcher 此刻不跑消息循环，
                // 等到那边再处理时钩子早卸掉了。
                target?.OnWinKeyCaptured(virtualKey);

                // 故意不拦截：Win 键的按下仍会交给系统（录完就退出捕获，
                // 不会因为录键而把 Win 键"吞"掉）。
            }
        }

        return CallNextHookEx(IntPtr.Zero, nCode, wParam, lParam);
    }

    /// <summary>低级钩子回调里捕获到 Win 键。</summary>
    private void OnWinKeyCaptured(int virtualKey)
    {
        if (!IsCapturing) return;

        StopWinKeyWatch();
        VirtualKey = virtualKey;
        KeyCaptured?.Invoke(this, EventArgs.Empty);
    }

    /// <summary>语言切换后重新取占位文字并刷新显示。</summary>
    public void RefreshLocalization()
    {
        RefreshText();
    }

    private void RefreshText()
    {
        // 禁用状态（例如"组合键"这一组没被选中）不显示任何文字
        if (!IsEnabled)
        {
            CapturedText = string.Empty;
            return;
        }

        if (IsCapturing)
        {
            CapturedText = CapturingText.Length > 0
                ? CapturingText
                : Localization.T("hotkey.capturing");
            return;
        }

        if (VirtualKey != 0)
        {
            CapturedText = DisplayNameFor(VirtualKey);
            return;
        }

        CapturedText = EmptyText.Length > 0 ? EmptyText : Localization.T("hotkey.empty");
    }

    /// <summary>
    /// WPF 对修饰键只给出通用的 VK_CONTROL / VK_SHIFT / VK_MENU，
    /// 这里用 GetAsyncKeyState 判断物理上是左边还是右边那个。
    /// </summary>
    private static int ResolveModifierSide(int vk)
    {
        static bool IsDown(int virtualKey) => (GetAsyncKeyState(virtualKey) & 0x8000) != 0;

        switch (vk)
        {
            case VK_CONTROL:
                if (IsDown(VK_LCONTROL)) return VK_LCONTROL;
                if (IsDown(VK_RCONTROL)) return VK_RCONTROL;
                break;
            case VK_SHIFT:
                if (IsDown(VK_LSHIFT)) return VK_LSHIFT;
                if (IsDown(VK_RSHIFT)) return VK_RSHIFT;
                break;
            case VK_MENU:
                if (IsDown(VK_LMENU)) return VK_LMENU;
                if (IsDown(VK_RMENU)) return VK_RMENU;
                break;
            default:
                break;
        }
        return vk;
    }
}
