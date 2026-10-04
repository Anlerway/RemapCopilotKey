#pragma once
// ---------------------------------------------------------------------------
// RemapCopilotKey - 虚拟键名 <-> 虚拟键码 映射
//
// 配置文件里用规范化名字（"VK_RCONTROL"）而不是裸数字，好处：
//   * 配置可读、可手改
//   * 跨语言版本稳定，不会因为键盘布局差异而含义漂移
// ---------------------------------------------------------------------------

#include <windows.h>

#include <string>
#include <utility>
#include <vector>

namespace rck {

// 一个可映射的键
struct KeyInfo {
    const wchar_t* name;   // 规范名，例如 L"VK_RCONTROL"
    WORD           vkey;   // 虚拟键码
};

// 全部已知键（顺序即 UI 显示顺序）
const std::vector<KeyInfo>& AllKeys();

// "VK_RCONTROL" / "RCONTROL" / "A" / "F23" -> 虚拟键码。
// 未识别返回 0（0 不是合法的可映射键）。
WORD VKeyFromName(const std::wstring& name);

// 虚拟键码 -> 规范名。未识别返回 L"VK_0xXX" 这种形式，便于日志排查。
std::wstring VKeyName(WORD vkey);

// 大小写不敏感的宽字符比较，供内部与外部共用
bool EqualsIgnoreCase(const std::wstring& a, const std::wstring& b);

} // namespace rck
