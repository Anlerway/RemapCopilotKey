// ---------------------------------------------------------------------------
// C# 9 的 init 访问器 / record 需要这个标记类型，而它只在 .NET 5+ 的 BCL 里。
// .NET Framework 4.8 没有，所以在项目里补一个 —— 这是编译器认可的标准做法，
// 不需要任何 NuGet 包（否则就得引 IsExternalInit 包）。
//
// 本文件只影响编译，不产生任何运行时行为。
// ---------------------------------------------------------------------------

namespace System.Runtime.CompilerServices
{
    internal static class IsExternalInit
    {
    }
}
