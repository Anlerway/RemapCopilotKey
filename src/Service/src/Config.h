#pragma once
// ---------------------------------------------------------------------------
// RemapCopilotKey - 配置模型
//
// 配置以 JSON 保存在程序所在目录的 config.json（UTF-8，无 BOM）。
// 服务程序读取它并支持热加载；配置程序负责写入它。
//
// 设计原则：
//   * 键名用规范字符串（"VK_RCONTROL"），不用裸数字，配置可读可手改
//   * 未知字段不报错、原样忽略，方便配置程序先行升级
//   * 任何一项非法都退化为默认值并记日志，绝不让服务起不来
// ---------------------------------------------------------------------------

#include <windows.h>

#include <string>
#include <vector>

namespace rck {

// ---------------------------------------------------------------------------
// 动作类型
// ---------------------------------------------------------------------------
enum class ActionType {
    SendKey,       // 映射为单个按键（按下/松开跟随 Copilot 键）
    SendChord,     // 映射为组合键（触发时一次性按下再松开）
    OpenApp,       // 打开应用程序
    OpenUrl,       // 打开网页
    SystemAction,  // Windows 实用功能
};

const wchar_t* ActionTypeName(ActionType type);
bool ActionTypeFromName(const std::wstring& name, ActionType& out);

// ---------------------------------------------------------------------------
// Windows 实用功能
// ---------------------------------------------------------------------------
enum class SystemActionId {
    Explorer,        // 资源管理器
    TaskManager,     // 任务管理器
    Cmd,             // 命令提示符
    PowerShell,      // PowerShell
    Screenshot,      // 截图（Win+Shift+S）
    SnippingTool,    // 截图工具
    Settings,        // 设置
    ControlPanel,    // 控制面板
    Calculator,      // 计算器
    Notepad,         // 记事本
    TaskView,        // 任务视图（Win+Tab）
    ClipboardHistory,// 剪贴板历史（Win+V）
    EmojiPanel,      // 表情符号面板（Win+.）
    LockScreen,      // 锁定屏幕（Win+L）
};

const wchar_t* SystemActionName(SystemActionId id);
bool SystemActionFromName(const std::wstring& name, SystemActionId& out);

// ---------------------------------------------------------------------------
// 动作
// ---------------------------------------------------------------------------
struct Action {
    ActionType type = ActionType::SendKey;

    WORD vkey = VK_RCONTROL;          // SendKey
    std::vector<WORD> chord;          // SendChord
    std::wstring path;                // OpenApp
    std::wstring arguments;           // OpenApp
    bool runAsAdmin = false;          // OpenApp：被打开的程序是否以管理员权限运行
    bool toggleWindow = false;        // OpenApp：程序已在运行时改为最小化/还原它的窗口
    std::wstring url;                 // OpenUrl
    SystemActionId systemAction = SystemActionId::Explorer;  // SystemAction
};

// ---------------------------------------------------------------------------
// 完整配置
// ---------------------------------------------------------------------------
struct Config {
    int          version       = 1;
    std::wstring language      = L"zh-CN";
    std::string  logLevel      = "error";
    bool         enabled       = true;
    bool         reloadOnChange = true;  // 配置文件变化后自动重载

    std::wstring trigger       = L"copilotKey";  // 目前只支持 Copilot 键，保留字段便于扩展
    Action       action;                          // 默认：映射为右 Ctrl
};

Config DefaultConfig();

// ---------------------------------------------------------------------------
// 读写
// ---------------------------------------------------------------------------

// 从文件读取。返回 false 时 out 被填为默认值，并把原因写入 error（中文）。
bool ConfigLoad(const std::wstring& path, Config& out, std::wstring& error);

// 写入文件（UTF-8，无 BOM，缩进 2 空格）。
bool ConfigSave(const std::wstring& path, const Config& config, std::wstring& error);

// 把配置序列化为 JSON 文本（供测试与配置程序参考）
std::string ConfigToJson(const Config& config);

// 从 JSON 文本解析配置
bool ConfigFromJson(const std::string& text, Config& out, std::wstring& error);

// ---------------------------------------------------------------------------
// 配置变更监视（轮询式，简单且不需要额外线程）
// ---------------------------------------------------------------------------
class ConfigWatcher {
public:
    void Watch(const std::wstring& path);

    // 首次调用返回 false（只是建立基线）；之后文件的时间戳或大小变化时返回 true
    bool PollChanged();

private:
    std::wstring m_path;
    ULONGLONG    m_lastWrite = 0;
    ULONGLONG    m_lastSize  = 0;
    bool         m_hasBaseline = false;
};

// ---------------------------------------------------------------------------
// 路径与字符串工具
// ---------------------------------------------------------------------------
std::wstring GetModuleDirectory();                        // 结尾不带反斜杠
std::wstring PathJoin(const std::wstring& a, const std::wstring& b);
std::wstring GetDefaultConfigPath();                      // <程序目录>\config.json
std::wstring GetDefaultLogPath();                         // <程序目录>\RemapCopilotKey.log

std::string  WideToUtf8(const std::wstring& text);
std::wstring Utf8ToWide(const std::string& text);

// 读取整个文件为 UTF-8 文本（自动去掉 UTF-8 BOM）
bool ReadFileUtf8(const std::wstring& path, std::string& out, std::wstring& error);

// 以 UTF-8（无 BOM）写入文件；目录不存在时会尝试创建
bool WriteFileUtf8(const std::wstring& path, const std::string& text, std::wstring& error);

} // namespace rck
