using System;
using System.IO;
using System.Linq;
using Translator.Core.CodeGen;
using Translator.Core.IO;
using Xunit;

namespace Translator.Tests;

public class AssemblyTargetOsTests
{
    // AppendLine uses the host newline, so compare whole lines rather than LF-delimited substrings.
    private static string[] Lines(string text) =>
        text.Split('\n').Select(line => line.TrimEnd('\r')).ToArray();

    private static string GenerateBlobAssembly(AssemblyTargetOs targetOs, string tag)
    {
        var dol = SyntheticDolFactory.Create(
            0x80004000,
            sections: [SyntheticDolFactory.Data(5, 0x80008000, 1, 2, 3, 4)]);
        var tempDir = Path.Combine(Path.GetTempPath(), $"mkw_blob_target_{tag}_{Guid.NewGuid():N}");
        try
        {
            var output = Path.Combine(tempDir, "data_sections_init.cpp");
            DataSectionGenerator.Generate(dol, rel: null, output, targetOs: targetOs);
            return File.ReadAllText(Path.Combine(tempDir, "data_sections_init_blobs.S"));
        }
        finally
        {
            if (Directory.Exists(tempDir)) Directory.Delete(tempDir, recursive: true);
        }
    }

    [Theory]
    [InlineData("windows", AssemblyTargetOs.Windows)]
    [InlineData("MacOS", AssemblyTargetOs.MacOS)]
    [InlineData("linux", AssemblyTargetOs.Linux)]
    [InlineData("android", AssemblyTargetOs.Linux)]
    public void ParsesUserFacingNames(string text, AssemblyTargetOs expected)
    {
        Assert.True(AssemblyTargetOsSyntax.TryParse(text, out var parsed));
        Assert.Equal(expected, parsed);
    }

    [Theory]
    [InlineData("")]
    [InlineData("quest")]
    [InlineData(null)]
    public void RejectsUnknownNames(string? text)
    {
        Assert.False(AssemblyTargetOsSyntax.TryParse(text, out _));
    }

    [Fact]
    public void HostFlavourMatchesTheRunningOperatingSystem()
    {
        var expected = OperatingSystem.IsWindows() ? AssemblyTargetOs.Windows
            : OperatingSystem.IsMacOS() ? AssemblyTargetOs.MacOS
            : AssemblyTargetOs.Linux;
        Assert.Equal(expected, AssemblyTargetOsSyntax.Host());
    }

    [Fact]
    public void ElfFlavourEmitsRodataUndecoratedSymbolsAndStackNote()
    {
        var assembly = GenerateBlobAssembly(AssemblyTargetOs.Linux, "elf");
        var lines = Lines(assembly);
        Assert.Contains(".section .rodata,\"a\",@progbits", lines);
        Assert.Contains(".globl kData__data", lines);
        Assert.DoesNotContain(".rdata", assembly, StringComparison.Ordinal);
        Assert.DoesNotContain("__TEXT,__const", assembly, StringComparison.Ordinal);
        Assert.Contains(".section .note.GNU-stack,\"\",@progbits", lines);
    }

    [Fact]
    public void WindowsFlavourEmitsRdataWithoutStackNote()
    {
        var assembly = GenerateBlobAssembly(AssemblyTargetOs.Windows, "win");
        var lines = Lines(assembly);
        Assert.Contains(".section .rdata,\"dr\"", lines);
        Assert.Contains(".globl kData__data", lines);
        Assert.DoesNotContain(".note.GNU-stack", assembly, StringComparison.Ordinal);
    }

    [Fact]
    public void MacOsFlavourDecoratesSymbols()
    {
        var assembly = GenerateBlobAssembly(AssemblyTargetOs.MacOS, "mac");
        var lines = Lines(assembly);
        Assert.Contains(".section __TEXT,__const", lines);
        Assert.Contains(".globl _kData__data", lines);
        Assert.DoesNotContain(".note.GNU-stack", assembly, StringComparison.Ordinal);
    }
}
