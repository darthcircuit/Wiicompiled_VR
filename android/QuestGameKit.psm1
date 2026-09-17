# The Quest game kit: everything libmain.so links except the translated game, plus the recipe
# that compiles and links the player's own translation against it.
#
# The APK never contains translated Mario Kart code. Its assets carry this kit instead, and the
# player's libmain.so is built from their own disc, either on the headset (the launcher's
# on-device builder) or on a PC (Build-QuestGame.ps1, the PC launcher's "Build for Quest"), then
# loaded from the app's private storage. Both builders replay one recipe, kit.json, which is
# exported from CMake's own build graph so its flags and link order cannot drift from a normal
# build:
#
#  * the link line of mkw_quest_kit_probe (runtime/cmake/PublicProducts.cmake), which is
#    WiiCompiled without the game, becomes kit.json's link inputs, with {game:*} markers where
#    the game's objects sat in WiiCompiled's own link;
#  * the compile commands CMake recorded for one source of each generated kind (translated shard,
#    registration shard, disc-generated runtime source, blob assembly) become kit.json's
#    compile flags, with include paths reduced to the kit's copy of runtime/include.
#
# Placeholders in kit.json: {kit} the extracted kit, {sysroot} the NDK sysroot the compiler
# uses, {workspace} the directory holding the translator's generated/ tree.
#
# Windows PowerShell 5.1 compatible.

Set-StrictMode -Version Latest

$script:KitSchema = 1
$script:GameSchema = 1

function ConvertTo-ForwardPath([string]$Path) {
    return [IO.Path]::GetFullPath($Path).Replace('\', '/').TrimEnd('/')
}

function ConvertFrom-NinjaPath([string]$Text) {
    return $Text.Replace('$:', ':').Replace('$ ', ' ').Replace('$$', '$')
}

# Splits a CMake compile command. CMake quotes only arguments that need it and escapes inner
# quotes as \" (for example -DIMGUI_USER_CONFIG=\"aurora/imgui_config.h\").
function Split-CommandLine([string]$Command) {
    $arguments = New-Object System.Collections.Generic.List[string]
    $current = New-Object System.Text.StringBuilder
    $inQuotes = $false
    $pending = $false
    for ($i = 0; $i -lt $Command.Length; $i++) {
        $c = $Command[$i]
        if ($c -eq '\' -and $i + 1 -lt $Command.Length -and $Command[$i + 1] -eq '"') {
            [void]$current.Append('"'); $i++; $pending = $true; continue
        }
        if ($c -eq '"') { $inQuotes = -not $inQuotes; $pending = $true; continue }
        if (-not $inQuotes -and [char]::IsWhiteSpace($c)) {
            if ($pending) { $arguments.Add($current.ToString()); [void]$current.Clear(); $pending = $false }
            continue
        }
        [void]$current.Append($c); $pending = $true
    }
    if ($pending) { $arguments.Add($current.ToString()) }
    return ,$arguments.ToArray()
}

# One argument for a response file or a Windows command line (CommandLineToArgvW rules, which
# clang's GNU response-file tokenizer reads the same way for these arguments).
function ConvertTo-QuotedArgument([string]$Argument) {
    if ($Argument -notmatch '[\s"]') { return $Argument }
    return '"' + $Argument.Replace('"', '\"') + '"'
}

function Get-Sha256Hex([string]$Path) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $stream = [IO.File]::OpenRead($Path)
        try { $bytes = $sha.ComputeHash($stream) } finally { $stream.Dispose() }
    } finally { $sha.Dispose() }
    return (($bytes | ForEach-Object { $_.ToString('x2') }) -join '')
}

function Get-StringSha256Hex([string]$Text) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $bytes = $sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text)) } finally { $sha.Dispose() }
    return (($bytes | ForEach-Object { $_.ToString('x2') }) -join '')
}

# Identity of a runtime/include tree. Translated code compiles against these headers, so a game is
# only built from a translation whose runtime headers are the kit's own.
function Get-RuntimeIncludeFingerprint([string]$Directory) {
    $lines = Get-RelativeFiles $Directory | Sort-Object FullName | ForEach-Object {
        $_.Relative + ' ' + (Get-Sha256Hex $_.FullName)
    }
    return Get-StringSha256Hex ($lines -join "`n")
}

# The files below a directory with their paths relative to it. .NET enumeration keeps the root
# exactly as given; Resolve-Path and Get-ChildItem can disagree about an 8.3 short name (TEMP),
# which would shift every relative path.
function Get-RelativeFiles([string]$Directory) {
    $root = [IO.Path]::GetFullPath($Directory).TrimEnd('\', '/')
    foreach ($path in [IO.Directory]::EnumerateFiles($root, '*', [IO.SearchOption]::AllDirectories)) {
        [pscustomobject]@{ FullName = $path; Relative = $path.Substring($root.Length + 1).Replace('\', '/') }
    }
}

# compile_commands.json as {file, command} objects. Windows PowerShell's ConvertFrom-Json refuses
# documents over 2 MB, so 5.1 uses the serializer underneath it with the limit lifted.
function Read-CompileCommands([string]$Path) {
    $text = [IO.File]::ReadAllText($Path)
    if ($PSVersionTable.PSVersion.Major -ge 6) {
        return @($text | ConvertFrom-Json | ForEach-Object { [pscustomobject]@{ file = $_.file; command = $_.command } })
    }
    Add-Type -AssemblyName System.Web.Extensions
    $serializer = New-Object System.Web.Script.Serialization.JavaScriptSerializer
    $serializer.MaxJsonLength = [int]::MaxValue
    return @($serializer.DeserializeObject($text) | ForEach-Object { [pscustomobject]@{ file = $_['file']; command = $_['command'] } })
}

# Compile flags of one recorded command, reduced to what a generated source needs anywhere.
function ConvertTo-KitCompileFlags {
    param([string[]]$Arguments, [string]$RuntimeInclude, [string]$Workspace)
    $flags = New-Object System.Collections.Generic.List[string]
    for ($i = 1; $i -lt $Arguments.Length; $i++) {
        $a = $Arguments[$i]
        switch -Regex -CaseSensitive ($a) {
            '^(-o|-c|-MT|-MF)$' { $i++; continue }
            '^(-MD|-MMD|-g|-Winvalid-pch)$' { continue }
            '^-isystem$' { $i++; continue }
            '^--sysroot=' { $flags.Add('--sysroot={sysroot}'); continue }
            '^-Xclang$' {
                if ($i + 3 -lt $Arguments.Length -and $Arguments[$i + 1] -eq '-include-pch') { $i += 3; continue }
                if ($i + 3 -lt $Arguments.Length -and $Arguments[$i + 1] -eq '-include') {
                    # CMake's cmake_pch.hxx only includes the precompiled header; builders parse it
                    # directly (a PCH is only valid for the compiler that wrote it).
                    $i += 3; $flags.Add('-include'); $flags.Add('mkw_pch.h'); continue
                }
                $flags.Add($a); continue
            }
            '^-I' {
                $path = ConvertTo-ForwardPath $a.Substring(2)
                if ($path -eq $RuntimeInclude) { $flags.Add('-I{kit}/include') }
                elseif ($path -eq $Workspace) { $flags.Add('-I{workspace}') }
                continue
            }
            default { $flags.Add($a) }
        }
    }
    return ,$flags.ToArray()
}

# The arguments of a `clang -###` command line: double-quoted, with backslash escapes.
function Split-DriverCommand([string]$Line) {
    $arguments = New-Object System.Collections.Generic.List[string]
    $current = New-Object System.Text.StringBuilder
    $inQuotes = $false
    for ($i = 0; $i -lt $Line.Length; $i++) {
        $c = $Line[$i]
        if ($inQuotes) {
            if ($c -eq '\' -and $i + 1 -lt $Line.Length) { [void]$current.Append($Line[++$i]) }
            elseif ($c -eq '"') { $arguments.Add($current.ToString()); [void]$current.Clear(); $inQuotes = $false }
            else { [void]$current.Append($c) }
        } elseif ($c -eq '"') { $inQuotes = $true }
    }
    return , $arguments.ToArray()
}

# The kit's link as a raw ld.lld command, for the headset. Android lets the app start its tools
# only through the system linker, so clang there cannot start lld itself and the builder runs lld
# directly. The NDK's own driver expands the link here, with the kit and marker objects standing in
# as real files. Placeholders: {kit}, {ndk} (the NDK's prebuilt toolchain directory, which holds
# sysroot/ and lib/clang/), {output}, and {game:runtime}, {game:product}, {game:translated}, each
# replaced by its object files (the translated ones already sit inside --start-lib/--end-lib).
function Get-KitLldArguments {
    param([string]$ClangCxx, [string]$KitDir, [string[]]$Flags, [string[]]$Inputs)
    $kit = ConvertTo-ForwardPath $KitDir
    $ndk = ConvertTo-ForwardPath (Join-Path (Split-Path -Parent $ClangCxx) '..')
    # Forward slashes throughout: clang reads response files with GNU quoting, where '\' escapes.
    $probe = ConvertTo-ForwardPath (Join-Path ([IO.Path]::GetTempPath()) ('mkw-kit-link-' + [Guid]::NewGuid().ToString('N')))
    New-Item -ItemType Directory $probe | Out-Null
    try {
        $markers = [ordered]@{}
        foreach ($slot in 'runtime', 'product', 'translated') {
            $markers[$slot] = ConvertTo-ForwardPath (Join-Path $probe "game_$slot.o")
            [IO.File]::WriteAllBytes($markers[$slot], [byte[]]@())
        }
        $real = { param([string]$s) $s.Replace('{kit}', $kit).Replace('{sysroot}', "$ndk/sysroot") }
        $arguments = New-Object System.Collections.Generic.List[string]
        foreach ($flag in $Flags) { $arguments.Add((& $real $flag)) }
        $arguments.Add('-o'); $arguments.Add("$probe/libmain.so")
        foreach ($linkInput in $Inputs) {
            switch ($linkInput) {
                '{game:runtime}' { $arguments.Add($markers.runtime) }
                '{game:product}' { $arguments.Add($markers.product) }
                '{game:translated}' { $arguments.Add('-Wl,--start-lib'); $arguments.Add($markers.translated); $arguments.Add('-Wl,--end-lib') }
                default { $arguments.Add((& $real $linkInput)) }
            }
        }
        $rsp = Join-Path $probe 'link.rsp'
        [IO.File]::WriteAllText($rsp, (($arguments | ForEach-Object { ConvertTo-QuotedArgument $_ }) -join "`n"), (New-Object Text.UTF8Encoding $false))

        # -### prints the command on stderr; Windows PowerShell turns redirected native stderr into errors.
        $start = New-Object Diagnostics.ProcessStartInfo $ClangCxx, "-### `"@$rsp`""
        $start.UseShellExecute = $false
        $start.RedirectStandardError = $true
        $start.RedirectStandardOutput = $true
        $process = [Diagnostics.Process]::Start($start)
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEnd()
        $process.WaitForExit()
        [void]$stdout.Result
        if ($process.ExitCode -ne 0) { throw "clang++ -### failed for the kit link:`n$stderr" }
        $line = @($stderr -split "`r?`n" | Where-Object { $_ -match '^\s*"[^"]*ld(\.lld)?(\.exe)?"' }) | Select-Object -Last 1
        if (-not $line) { throw "clang++ -### printed no linker command:`n$stderr" }

        $lld = New-Object System.Collections.Generic.List[string]
        $driverArguments = Split-DriverCommand $line
        for ($i = 1; $i -lt $driverArguments.Length; $i++) {
            $argument = $driverArguments[$i].Replace('\', '/')
            if ($argument -eq "$probe/libmain.so") { $lld.Add('{output}'); continue }
            $slot = @($markers.Keys | Where-Object { $markers[$_] -eq $argument })
            if ($slot.Count -eq 1) { $lld.Add("{game:$($slot[0])}"); continue }
            $argument = [regex]::Replace($argument, [regex]::Escape($kit), '{kit}', 'IgnoreCase')
            $argument = [regex]::Replace($argument, [regex]::Escape($ndk), '{ndk}', 'IgnoreCase')
            if ($argument -match '[A-Za-z]:/') { throw "The kit link names a path outside the kit and the NDK: $argument" }
            $lld.Add($argument)
        }
        foreach ($required in '{output}', '{game:runtime}', '{game:product}', '{game:translated}') {
            if (-not $lld.Contains($required)) { throw "The expanded kit link lacks $required" }
        }
        return , $lld.ToArray()
    } finally {
        Remove-Item -Recurse -Force $probe
    }
}

function Export-QuestGameKit {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string]$CMakeBinaryDir,
        [Parameter(Mandatory)] [string]$OutputDir,
        [Parameter(Mandatory)] [string]$RepoRoot,
        [Parameter(Mandatory)] [string]$LlvmStrip
    )
    $ErrorActionPreference = 'Stop'
    $binary = ConvertTo-ForwardPath $CMakeBinaryDir
    $runtimeInclude = ConvertTo-ForwardPath (Join-Path $RepoRoot 'runtime/include')

    # The probe's link edge: explicit inputs are the objects, LINK_LIBRARIES the archives.
    $ninja = [IO.File]::ReadAllText("$binary/build.ninja")
    $edge = [regex]::Match($ninja, '(?m)^build (\S*libmkw_quest_kit_probe\.so): (\S+) ([^\r\n]*)\r?\n((?:  [^\r\n]*\r?\n)+)')
    if (-not $edge.Success) { throw "No mkw_quest_kit_probe link edge in $binary/build.ninja" }
    $explicit = ($edge.Groups[3].Value.Replace('$ ', '<ninja-space>') -split ' \|')[0]
    $objects = @($explicit -split ' ' | Where-Object { $_ } | ForEach-Object { ConvertFrom-NinjaPath $_.Replace('<ninja-space>', '$ ') })
    $vars = @{}
    foreach ($line in ($edge.Groups[4].Value -split "\r?\n")) {
        if ($line -match '^  (\w+) = (.*)$') { $vars[$Matches[1]] = $Matches[2] }
    }
    $rule = [regex]::Match($ninja, "(?ms)^rule $([regex]::Escape($edge.Groups[2].Value))\r?\n.*?command = ([^\r\n]*)")
    if (-not $rule.Success) {
        $rules = [IO.File]::ReadAllText("$binary/CMakeFiles/rules.ninja")
        $rule = [regex]::Match($rules, "(?ms)^rule $([regex]::Escape($edge.Groups[2].Value))\r?\n.*?command = ([^\r\n]*)")
    }
    $target = [regex]::Match($rule.Groups[1].Value, '--target=(\S+)').Groups[1].Value
    if (-not $target) { throw "Cannot read the link target triple from rule $($edge.Groups[2].Value)" }

    # Compile flags per generated kind, from CMake's own commands.
    $commands = Read-CompileCommands "$binary/compile_commands.json"
    function Find-Command([string]$Pattern) {
        $entry = @($commands | Where-Object { $_.file.Replace('\', '/') -match $Pattern })
        if ($entry.Count -eq 0) { throw "compile_commands.json has no source matching $Pattern" }
        return $entry[0]
    }
    $translatedEntry = Find-Command '/build_shards/base_common/[^/]+\.cpp$'
    $workspace = ConvertTo-ForwardPath ($translatedEntry.file.Replace('\', '/') -replace '/generated/build_shards/.*$', '')
    $flagsFor = {
        param($Entry)
        ConvertTo-KitCompileFlags -Arguments (Split-CommandLine $Entry.command) -RuntimeInclude $runtimeInclude -Workspace $workspace
    }
    $compile = [ordered]@{
        translated = & $flagsFor $translatedEntry
        product = & $flagsFor (Find-Command '/build_shards/base_registration/[^/]+\.cpp$')
        runtime = & $flagsFor (Find-Command '/generated/data_sections_init\.cpp$')
        asm = & $flagsFor (Find-Command 'data_sections_init_blobs[^/]*\.S$')
    }

    if (Test-Path $OutputDir) { Remove-Item -Recurse -Force $OutputDir }
    $objectsDir = Join-Path $OutputDir 'objects'
    $libsDir = Join-Path $OutputDir 'libs'
    New-Item -ItemType Directory -Force $objectsDir, $libsDir | Out-Null
    Copy-Item -Recurse (Join-Path $RepoRoot 'runtime/include') (Join-Path $OutputDir 'include')

    $copied = @{}
    $copy = {
        param([string]$Source, [string]$Directory)
        $full = ConvertTo-ForwardPath $(if ([IO.Path]::IsPathRooted($Source)) { $Source } else { "$binary/$Source" })
        if ($copied.ContainsKey($full)) { return $copied[$full] }
        $script:kitIndex++
        $name = '{0:D3}_{1}' -f $script:kitIndex, [IO.Path]::GetFileName($full)
        $destination = Join-Path (Join-Path $OutputDir $Directory) $name
        & $LlvmStrip --strip-debug -o $destination $full
        if ($LASTEXITCODE -ne 0) { throw "llvm-strip failed on $full" }
        $copied[$full] = "{kit}/$Directory/$name"
        return $copied[$full]
    }
    $script:kitIndex = 0

    # Link inputs in the probe's order, with the game's slots marked where WiiCompiled has them.
    $inputs = New-Object System.Collections.Generic.List[string]
    $lastRuntime = -1
    for ($i = 0; $i -lt $objects.Count; $i++) {
        if ($objects[$i] -match '/mkw_runtime_common\.dir/') { $lastRuntime = $i }
    }
    for ($i = 0; $i -lt $objects.Count; $i++) {
        $inputs.Add((& $copy $objects[$i] 'objects'))
        if ($i -eq $lastRuntime) { $inputs.Add('{game:runtime}') }
        if ($objects[$i] -match '/base_product\.cpp\.o$') { $inputs.Add('{game:product}') }
    }
    if (-not $inputs.Contains('{game:runtime}') -or -not $inputs.Contains('{game:product}')) {
        throw 'The probe link line lacks the runtime or product objects the game slots follow'
    }
    foreach ($token in ((ConvertFrom-NinjaPath $vars['LINK_LIBRARIES']) -split ' ' | Where-Object { $_ })) {
        if ($token.StartsWith('-')) { $inputs.Add($token); continue }
        $path = $token.Replace('\', '/')
        if ($path -match '/sysroot/(.+)$') { $inputs.Add("{sysroot}/$($Matches[1])") }
        else { $inputs.Add((& $copy $token 'libs')) }
        if ($path -match '/libmkw_platform\.a$') { $inputs.Add('{game:translated}') }
    }
    if (-not $inputs.Contains('{game:translated}')) { throw 'The probe link line lacks libmkw_platform.a' }

    $linkFlags = New-Object System.Collections.Generic.List[string]
    $linkFlags.Add("--target=$target"); $linkFlags.Add('--sysroot={sysroot}'); $linkFlags.Add('-fPIC')
    foreach ($name in 'LANGUAGE_COMPILE_FLAGS', 'ARCH_FLAGS', 'LINK_FLAGS') {
        if (-not $vars.ContainsKey($name)) { continue }
        $tokens = Split-CommandLine (ConvertFrom-NinjaPath $vars[$name])
        for ($i = 0; $i -lt $tokens.Length; $i++) {
            if ($tokens[$i] -eq '-g' -or $tokens[$i] -eq '-Wl,--unresolved-symbols=ignore-all') { continue }
            if ($tokens[$i] -eq '-Xlinker' -and $i + 1 -lt $tokens.Length -and $tokens[$i + 1].StartsWith('--dependency-file')) { $i++; continue }
            $linkFlags.Add($tokens[$i])
        }
    }
    $linkFlags.Add('-Wl,-soname,libmain.so')
    $lld = Get-KitLldArguments -ClangCxx (Join-Path (Split-Path -Parent $LlvmStrip) 'clang++.exe') -KitDir $OutputDir `
        -Flags $linkFlags.ToArray() -Inputs $inputs.ToArray()

    # What the headset's translator needs besides the disc: the game manifest and symbol map, and
    # the runtime sources it indexes for native registrations, which must be the ones compiled into
    # this kit.
    $translation = Join-Path $OutputDir 'translation'
    New-Item -ItemType Directory -Force (Join-Path $translation 'projects/mkwii'), (Join-Path $translation 'runtime') | Out-Null
    Copy-Item (Join-Path $RepoRoot 'projects/mkwii/recomp.yml'), (Join-Path $RepoRoot 'projects/mkwii/MAP.txt') (Join-Path $translation 'projects/mkwii')
    Copy-Item -Recurse (Join-Path $RepoRoot 'runtime/src') (Join-Path $translation 'runtime/src')

    $recipe = [ordered]@{
        schema = $script:KitSchema
        product = 'base'
        output = 'libmain.so'
        target = $target
        runtimeIncludeFingerprint = Get-RuntimeIncludeFingerprint (Join-Path $OutputDir 'include')
        compile = $compile
        link = [ordered]@{ flags = $linkFlags.ToArray(); inputs = $inputs.ToArray(); lld = $lld }
    }
    $recipeJson = $recipe | ConvertTo-Json -Depth 8 -Compress
    $files = Get-RelativeFiles $OutputDir | Sort-Object FullName | ForEach-Object {
        "/$($_.Relative) $(Get-Sha256Hex $_.FullName)"
    }
    $recipe.fingerprint = Get-StringSha256Hex ($recipeJson + "`n" + ($files -join "`n"))
    [IO.File]::WriteAllText((Join-Path $OutputDir 'kit.json'), ($recipe | ConvertTo-Json -Depth 8), (New-Object Text.UTF8Encoding $false))
    return $recipe.fingerprint
}

# The generated source lists emit-build-shards wrote into shards.cmake.
function Read-ShardList {
    param([string]$ShardsCmake, [string]$Name)
    $text = [IO.File]::ReadAllText($ShardsCmake)
    $match = [regex]::Match($text, "(?s)set\($Name\s*(.*?)\)")
    if (-not $match.Success) { return @() }
    return @([regex]::Matches($match.Groups[1].Value, '"([^"]+)"') | ForEach-Object { $_.Groups[1].Value.Replace('\', '/') })
}

function Invoke-QuestGameBuild {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string]$KitDir,
        [Parameter(Mandatory)] [string]$GeneratedDir,
        [Parameter(Mandatory)] [string]$BuildDir,
        [Parameter(Mandatory)] [string]$ClangCxx,
        [Parameter(Mandatory)] [string]$ClangC,
        [Parameter(Mandatory)] [string]$Sysroot,
        [Parameter(Mandatory)] [string]$Ninja,
        [Parameter(Mandatory)] [int]$TranslatedJobs
    )
    $ErrorActionPreference = 'Stop'
    $kit = ConvertTo-ForwardPath $KitDir
    $generated = ConvertTo-ForwardPath $GeneratedDir
    $workspace = ConvertTo-ForwardPath (Split-Path -Parent $generated)
    $recipe = [IO.File]::ReadAllText("$kit/kit.json") | ConvertFrom-Json
    if ($recipe.schema -ne $script:KitSchema) { throw "Unsupported game kit schema $($recipe.schema)" }
    # The translation must come from the same release as the kit: its code is compiled against the
    # kit's runtime headers and linked with the kit's runtime objects.
    $workspaceInclude = Join-Path $workspace 'runtime/include'
    if (Test-Path $workspaceInclude) {
        if ((Get-RuntimeIncludeFingerprint $workspaceInclude) -ne $recipe.runtimeIncludeFingerprint) {
            throw ('The Quest app and the translation on this PC come from different WiiCompiled releases ' +
                '(their runtime headers differ). Update both to the same release, then build again.')
        }
    }
    New-Item -ItemType Directory -Force $BuildDir | Out-Null
    $build = ConvertTo-ForwardPath $BuildDir
    $expand = { param([string]$s) $s.Replace('{kit}', $kit).Replace('{sysroot}', (ConvertTo-ForwardPath $Sysroot)).Replace('{workspace}', $workspace) }

    foreach ($kind in 'translated', 'product', 'runtime', 'asm') {
        $lines = @($recipe.compile.$kind | ForEach-Object { ConvertTo-QuotedArgument (& $expand $_) })
        [IO.File]::WriteAllText("$build/$kind.rsp", ($lines -join "`n"), (New-Object Text.UTF8Encoding $false))
    }
    $linkLines = @($recipe.link.flags | ForEach-Object { ConvertTo-QuotedArgument (& $expand $_) })
    [IO.File]::WriteAllText("$build/link.rsp", ($linkLines -join "`n"), (New-Object Text.UTF8Encoding $false))

    # The Windows installer generates the blob assembly for PE/COFF; rewrite it for ELF exactly
    # as runtime/cmake/PublicProducts.cmake does.
    $blob = [IO.File]::ReadAllText("$generated/data_sections_init_blobs.S")
    $elf = $blob.Replace('.section .rdata,"dr"', '.section .rodata,"a",@progbits')
    if ($elf -ne $blob) { $elf += "`n.section .note.GNU-stack,`"`",@progbits`n" }
    [IO.File]::WriteAllText("$build/data_sections_init_blobs_android.S", $elf, (New-Object Text.UTF8Encoding $false))

    $shards = "$generated/build_shards/shards.cmake"
    $sources = [ordered]@{
        runtime = @("$generated/data_sections_init.cpp", "$generated/guest_symbol_table.cpp")
        runtimeAsm = @("$build/data_sections_init_blobs_android.S")
        product = @(Read-ShardList $shards 'MKW_BASE_REGISTRATION_SOURCES')
        translated = @((Read-ShardList $shards 'MKW_BASE_COMMON_SHARDS') + (Read-ShardList $shards 'MKW_BASE_PORTABLE_SENSITIVE_SHARDS'))
    }
    if ($sources.translated.Count -eq 0) { throw "No translated shards in $shards" }

    $ninjaText = New-Object System.Text.StringBuilder
    $escape = { param([string]$p) $p.Replace('$', '$$').Replace(':', '$:').Replace(' ', '$ ') }
    [void]$ninjaText.AppendLine('ninja_required_version = 1.10')
    [void]$ninjaText.AppendLine("pool translated`n  depth = $TranslatedJobs")
    [void]$ninjaText.AppendLine("rule cxx`n  command = $(ConvertTo-QuotedArgument $ClangCxx) @`$flags -c `$in -o `$out`n  description = Compiling `$in")
    [void]$ninjaText.AppendLine("rule asm`n  command = $(ConvertTo-QuotedArgument $ClangC) @`$flags -c `$in -o `$out`n  description = Assembling `$in")
    [void]$ninjaText.AppendLine("rule link`n  command = $(ConvertTo-QuotedArgument $ClangCxx) @`$flags -o `$out @`$out.rsp`n  rspfile = `$out.rsp`n  rspfile_content = `$inputs`n  description = Linking `$out")
    $objectsOf = @{ runtime = @(); product = @(); translated = @() }
    $seen = @{}
    foreach ($kind in 'runtime', 'runtimeAsm', 'product', 'translated') {
        foreach ($source in $sources[$kind]) {
            $stem = [IO.Path]::GetFileNameWithoutExtension($source)
            $object = "obj/$stem.o"
            if ($seen.ContainsKey($object)) { throw "Two generated sources share the object name $object" }
            $seen[$object] = $true
            $rule = if ($kind -eq 'runtimeAsm') { 'asm' } else { 'cxx' }
            $flags = switch ($kind) { 'runtimeAsm' { 'asm.rsp' } default { "$kind.rsp" } }
            [void]$ninjaText.AppendLine("build $(& $escape $object): $rule $(& $escape $source)`n  flags = $(& $escape "$build/$flags")")
            if ($kind -eq 'translated') { [void]$ninjaText.AppendLine('  pool = translated') }
            $slot = if ($kind -eq 'runtimeAsm') { 'runtime' } else { $kind }
            $objectsOf[$slot] += "$build/$object"
        }
    }
    $linkInputs = New-Object System.Collections.Generic.List[string]
    foreach ($linkInput in $recipe.link.inputs) {
        switch ($linkInput) {
            '{game:runtime}' { $objectsOf.runtime | ForEach-Object { $linkInputs.Add($_) } }
            '{game:product}' { $objectsOf.product | ForEach-Object { $linkInputs.Add($_) } }
            '{game:translated}' {
                # Archive semantics, as libmkw_base_shared.a has in a CMake build.
                $linkInputs.Add('-Wl,--start-lib')
                $objectsOf.translated | ForEach-Object { $linkInputs.Add($_) }
                $linkInputs.Add('-Wl,--end-lib')
            }
            default { $linkInputs.Add((& $expand $linkInput)) }
        }
    }
    $allObjects = @($objectsOf.runtime + $objectsOf.product + $objectsOf.translated | ForEach-Object { & $escape $_.Substring($build.Length + 1) })
    $rspInputs = ($linkInputs | ForEach-Object { ConvertTo-QuotedArgument $_ }) -join ' '
    [void]$ninjaText.AppendLine("build libmain.so: link $($allObjects -join ' ')`n  flags = $(& $escape "$build/link.rsp")`n  inputs = $($rspInputs.Replace('$', '$$'))")
    [IO.File]::WriteAllText("$build/build.ninja", $ninjaText.ToString(), (New-Object Text.UTF8Encoding $false))

    # Ninja's progress goes to the console, not into this function's return value; its [n/N]
    # lines are what WiiCompiled Setup turns into build progress.
    Write-Host 'MKWCBUILD:STEP:quest-compile Compiling the game for Quest'
    & $Ninja -C $build | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Game build failed (ninja exit $LASTEXITCODE)" }
    return "$build/libmain.so"
}

# A .wcgame: the built library and what it was built from, for the headset to import. With
# -DataDir it also carries the extracted disc (DATA/...), written without compression, so a player who
# builds on a PC copies one file to the headset and never needs the disc image there.
function New-QuestGamePackage {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string]$Library,
        [string]$DataDir = '',
        [Parameter(Mandatory)] [string]$KitDir,
        [Parameter(Mandatory)] [string]$GameId,
        [Parameter(Mandatory)] [string]$DolSha256,
        [Parameter(Mandatory)] [string]$RelSha256,
        [Parameter(Mandatory)] [string]$BuiltBy,
        [Parameter(Mandatory)] [string]$OutputPath
    )
    $ErrorActionPreference = 'Stop'
    $recipe = [IO.File]::ReadAllText((Join-Path $KitDir 'kit.json')) | ConvertFrom-Json
    $game = [ordered]@{
        schema = $script:GameSchema
        profile = $recipe.product
        gameId = $GameId
        dolSha256 = $DolSha256
        relSha256 = $RelSha256
        kitFingerprint = $recipe.fingerprint
        library = $recipe.output
        librarySha256 = Get-Sha256Hex $Library
        includesData = [bool]$DataDir
        builtBy = $BuiltBy
        builtAt = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    }
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    if (Test-Path $OutputPath) { Remove-Item -Force $OutputPath }
    New-Item -ItemType Directory -Force (Split-Path -Parent ([IO.Path]::GetFullPath($OutputPath))) | Out-Null
    $zip = [IO.Compression.ZipFile]::Open($OutputPath, 'Create')
    try {
        $entry = $zip.CreateEntry('game.json')
        $writer = New-Object IO.StreamWriter($entry.Open(), (New-Object Text.UTF8Encoding $false))
        try { $writer.Write(($game | ConvertTo-Json)) } finally { $writer.Dispose() }
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $Library, $recipe.output, 'Optimal')
        if ($DataDir) {
            foreach ($file in Get-RelativeFiles $DataDir) {
                [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $file.FullName, "DATA/$($file.Relative)", 'NoCompression')
            }
        }
    } finally { $zip.Dispose() }
    return $game
}

Export-ModuleMember -Function Export-QuestGameKit, Invoke-QuestGameBuild, New-QuestGamePackage
