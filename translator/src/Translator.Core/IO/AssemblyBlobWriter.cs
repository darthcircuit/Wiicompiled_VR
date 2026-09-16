using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using Translator.Core.Loading;

namespace Translator.Core.IO;

internal sealed record AssemblyBlob(string FileName, string Symbol, ReadOnlyMemory<byte> Data, string Comment);

internal static class AssemblyBlobWriter
{
    public static void Write(
        string assemblyPath,
        string blobDirectory,
        string blobReferenceDirectory,
        IReadOnlyList<AssemblyBlob> blobs,
        AssemblyTargetOs targetOs,
        params string[] headerLines)
    {
        Directory.CreateDirectory(blobDirectory);
        var expectedFiles = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var assembly = new StringBuilder();
        foreach (var header in headerLines) assembly.AppendLine(header);
        // PE/COFF (Windows), Mach-O (macOS), and ELF (Linux/Android) spell a read-only data
        // section differently in GNU-as syntax. The caller names the object-file flavour the
        // product is assembled for; it defaults to the translator's own host, and a cross-compiled
        // product (the Android build generated on a Windows host) passes its target explicitly.
        assembly.AppendLine(AssemblyTargetOsSyntax.SectionDirective(targetOs));
        assembly.AppendLine();

        foreach (var blob in blobs)
        {
            var blobPath = Path.Combine(blobDirectory, blob.FileName);
            expectedFiles.Add(Path.GetFullPath(blobPath));
            FileOutput.WriteBytesIfChanged(blobPath, blob.Data.Span);
            var hash = ChecksumUtilities.Sha256Hex(blob.Data.Span);
            assembly.AppendLine($"// {blob.Comment}; sha256={hash}");
            assembly.AppendLine(".p2align 4");
            var assemblySymbol = AssemblyTargetOsSyntax.SymbolName(targetOs, blob.Symbol);
            assembly.AppendLine($".globl {assemblySymbol}");
            assembly.AppendLine($"{assemblySymbol}:");
            var referencePath = Path.Combine(blobReferenceDirectory, blob.FileName);
            assembly.AppendLine($".incbin \"{SanitizeAssemblyPath(referencePath)}\"");
            assembly.AppendLine();
        }

        foreach (var stalePath in Directory.EnumerateFiles(blobDirectory, "*.bin"))
        {
            if (!expectedFiles.Contains(Path.GetFullPath(stalePath))) File.Delete(stalePath);
        }
        if (AssemblyTargetOsSyntax.EmitsGnuStackNote(targetOs))
        {
            // Absence of a .note.GNU-stack section makes the linker assume the oldest, most
            // conservative default for this object (an executable stack) and warn about it; this
            // file has no code needing one, so mark it explicitly like every other GNU-as ELF
            // object linked into the binary already does (the norm on modern toolchains, just not
            // producible without an explicit section since this file is hand-assembled, not
            // compiler-emitted).
            assembly.AppendLine(AssemblyTargetOsSyntax.GnuStackNoteDirective);
        }
        FileOutput.WriteTextIfChanged(assemblyPath, assembly.ToString());
    }

    private static string SanitizeAssemblyPath(string path) => Path.GetFullPath(path).Replace('\\', '/');
}
