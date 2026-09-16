using System;

namespace Translator.Core.IO;

/// <summary>
/// The object-file flavour the hand-written blob assembly is assembled into. PE/COFF (Windows),
/// Mach-O (macOS) and ELF (Linux and Android) spell a read-only data section, external symbol
/// decoration and the stack-note trailer differently in GNU-as syntax. Historically the writer
/// keyed this off the operating system the translator itself ran on; a cross-compiled product
/// (the Quest build is generated on a Windows host and assembled by the Android NDK) must be able
/// to ask for another flavour explicitly.
/// </summary>
public enum AssemblyTargetOs
{
    Windows,
    MacOS,
    Linux,
}

public static class AssemblyTargetOsSyntax
{
    /// <summary>The flavour matching the host the translator is running on.</summary>
    public static AssemblyTargetOs Host()
    {
        if (OperatingSystem.IsWindows()) return AssemblyTargetOs.Windows;
        if (OperatingSystem.IsMacOS()) return AssemblyTargetOs.MacOS;
        return AssemblyTargetOs.Linux;
    }

    /// <summary>
    /// Parses a user-facing name. "android" is accepted as a spelling of the ELF flavour so a
    /// build script can name the platform it is actually producing.
    /// </summary>
    public static bool TryParse(string? text, out AssemblyTargetOs target)
    {
        switch (text?.Trim().ToLowerInvariant())
        {
            case "windows":
            case "win32":
            case "mingw":
                target = AssemblyTargetOs.Windows;
                return true;
            case "macos":
            case "darwin":
                target = AssemblyTargetOs.MacOS;
                return true;
            case "linux":
            case "android":
            case "elf":
                target = AssemblyTargetOs.Linux;
                return true;
            default:
                target = default;
                return false;
        }
    }

    public const string AcceptedNames = "windows, macos, linux, android";

    /// <summary>The directive that opens the read-only payload section.</summary>
    public static string SectionDirective(AssemblyTargetOs target) => target switch
    {
        AssemblyTargetOs.Windows => ".section .rdata,\"dr\"",
        AssemblyTargetOs.MacOS => ".section __TEXT,__const",
        _ => ".section .rodata,\"a\",@progbits",
    };

    /// <summary>
    /// C/C++ external symbols carry a leading underscore in Mach-O, unlike ELF and COFF. The
    /// generated C++ still names the symbol without that ABI decoration, so the assembly spelling
    /// is produced here.
    /// </summary>
    public static string SymbolName(AssemblyTargetOs target, string symbol) =>
        target == AssemblyTargetOs.MacOS ? "_" + symbol : symbol;

    /// <summary>
    /// Only ELF objects need an explicit non-executable-stack note: without one the linker assumes
    /// the most conservative default (an executable stack) and warns.
    /// </summary>
    public static bool EmitsGnuStackNote(AssemblyTargetOs target) => target == AssemblyTargetOs.Linux;

    public const string GnuStackNoteDirective = ".section .note.GNU-stack,\"\",@progbits";
}
