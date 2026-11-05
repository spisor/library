& {
$ErrorActionPreference = 'Stop'

# the patterns of runtime/schema/catalog.schema.json, verbatim; install_script_test fails when the schema changes them
$FileKeyPattern = '^[^/\\:]*[^/\\:. ](?:/[^/\\:]*[^/\\:. ])*(?![\s\S])'
$VersionPattern = '^(?![\s\S]*\.\.)[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]*[0-9A-Za-z-])?(\+[0-9A-Za-z.-]*[0-9A-Za-z-])?(?![\s\S])'
$Sha256Pattern = '^[0-9a-f]{64}(?![\s\S])'

$HostId = 'forebay-host'
$InstallMarker = '.installed.json'

<#
.NOTES
SPISOR_INSTALL_TRACE names a file the install test reads when a run hangs; nothing else sets it.
#>
function Write-InstallTrace([string]$Line) {
    if ($env:SPISOR_INSTALL_TRACE) { [System.IO.File]::AppendAllText($env:SPISOR_INSTALL_TRACE, "$([DateTime]::UtcNow.ToString('o')) $Line`n") }
}

function Test-WindowsImage([string]$Path) {
    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $first = $stream.ReadByte()
        $second = $stream.ReadByte()
    } finally {
        $stream.Dispose()
    }
    return $first -eq 0x4D -and $second -eq 0x5A
}

function Stop-Install([string]$Detail, [string]$Fix) {
    throw "$Detail`nfix: $Fix"
}

function Write-Fetching([string]$Line) {
    [Console]::Error.WriteLine($Line)
}

function Test-Absolute([string]$Path) {
    return $Path -match '^([\\/]|[A-Za-z]:[\\/])'
}

function Resolve-Cache {
    if (Test-Absolute $env:SPISOR_CACHE) { return $env:SPISOR_CACHE.TrimEnd('\', '/') }
    if (Test-Absolute $env:LOCALAPPDATA) { return Join-Path (Join-Path $env:LOCALAPPDATA.TrimEnd('\', '/') 'spisor') 'cache' }
    Stop-Install 'LOCALAPPDATA is absent, empty or not an absolute path, so the cache place cannot be found' 'set LOCALAPPDATA, or SPISOR_CACHE, to an absolute directory path'
}

function Get-PlatformKey {
    $architecture = $env:PROCESSOR_ARCHITEW6432
    if (-not $architecture) { $architecture = $env:PROCESSOR_ARCHITECTURE }
    if (-not $architecture) {
        try { $architecture = "$([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture)" } catch { $architecture = '' }
    }
    switch ($architecture.ToUpperInvariant()) {
        'AMD64' { return 'win32-x64' }
        'X64' { return 'win32-x64' }
        'ARM64' { return 'win32-arm64' }
    }
    return "win32-$($architecture.ToLowerInvariant())"
}

function Get-RequiredFiles([string]$Platform) {
    return @("$Platform/$HostId.exe", "$Platform/forebay-boot.exe", "$Platform/runtime-launcher.exe")
}

function Resolve-Catalog([string]$Catalog) {
    if ($Catalog -match '^https://') { return $Catalog }
    if ($Catalog -match '^[A-Za-z][A-Za-z0-9+.-]*://') {
        Stop-Install "the catalog $Catalog is neither an https url nor a local path" 'pass the path of a catalog.json, or its https url'
    }
    $path = [System.IO.Path]::GetFullPath([System.IO.Path]::Combine((Get-Location).ProviderPath, $Catalog))
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        Stop-Install "the catalog $Catalog does not exist" 'pass the path of a catalog.json, or its https url'
    }
    return $path
}

function Get-FileSha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Test-Sha256([string]$Path, [string]$Sha256) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    return (Get-FileSha256 $Path) -ceq $Sha256
}

<#
.NOTES
Redirects are followed by hand so that every hop stays on https, which the web client's own
redirects would not guarantee.
#>
function Get-HttpsResponse([string]$Url) {
    for ($hop = 0; $hop -lt 10; $hop++) {
        $request = [System.Net.WebRequest]::Create($Url)
        $request.AllowAutoRedirect = $false
        try { $response = $request.GetResponse() } catch [System.Net.WebException] {
            $response = $_.Exception.InnerException.Response
            if ($null -eq $response) { $response = $_.Exception.Response }
            if ($null -eq $response) { throw }
        }
        $status = [int]$response.StatusCode
        $location = $response.Headers['Location']
        if ($status -ge 300 -and $status -lt 400 -and $location) {
            $response.Close()
            $Url = (New-Object System.Uri((New-Object System.Uri($Url)), $location)).AbsoluteUri
            if (-not $Url.StartsWith('https://', [System.StringComparison]::OrdinalIgnoreCase)) { throw "the redirect to $Url leaves https" }
            continue
        }
        if ($status -ne 200) {
            $response.Close()
            throw "$Url answered $status"
        }
        return $response
    }
    throw "$Url redirects more than 10 times"
}

function Copy-Response([string]$Url, [System.IO.Stream]$Destination) {
    $response = Get-HttpsResponse $Url
    try {
        $stream = $response.GetResponseStream()
        try { $stream.CopyTo($Destination) } finally { $stream.Dispose() }
    } finally {
        $response.Close()
    }
}

function Receive-File($File, [string]$Path) {
    try {
        if ($File.Local) {
            [System.IO.File]::Copy($File.Source, $Path, $true)
        } else {
            $stream = [System.IO.File]::Create($Path)
            try { Copy-Response $File.Source $stream } finally { $stream.Dispose() }
        }
    } catch {
        Stop-Install "could not fetch $($File.Source)" 'check the network connection, then run this again'
    }
}

function New-TemporaryPath([string]$Path) {
    return "$Path.$([guid]::NewGuid().ToString('N')).part"
}

function Remove-Temporary([string]$Part) {
    if (Test-Path -LiteralPath $Part) { Remove-Item -LiteralPath $Part -Force -ErrorAction SilentlyContinue }
}

function Receive-Verified($File, [string]$Path) {
    $part = New-TemporaryPath $Path
    try {
        Receive-File $File $part
        if ((Get-Item -LiteralPath $part).Length -ne [long]$File.Size -or -not (Test-Sha256 $part $File.Sha256)) {
            Stop-Install "$($File.Source) does not have the $($File.Size) bytes and SHA-256 $($File.Sha256) it is pinned to" 'run this again; if it repeats, the download is being altered on the way'
        }
        Move-Item -LiteralPath $part -Destination $Path -Force
    } finally {
        Remove-Temporary $part
    }
}

function Read-CatalogText([string]$Catalog) {
    if ($Catalog -match '^https://') {
        $buffer = New-Object System.IO.MemoryStream
        try { Copy-Response $Catalog $buffer } catch { Stop-Install "could not fetch the catalog $Catalog" 'check the network connection and the url, then run this again' }
        return [System.Text.Encoding]::UTF8.GetString($buffer.ToArray())
    }
    return [System.IO.File]::ReadAllText($Catalog, [System.Text.Encoding]::UTF8)
}

function ConvertFrom-JsonElement($Element) {
    switch ($Element.ValueKind.ToString()) {
        'Object' {
            $map = New-Object 'System.Collections.Generic.Dictionary[string,object]' ([System.StringComparer]::Ordinal)
            foreach ($property in $Element.EnumerateObject()) { $map[$property.Name] = ConvertFrom-JsonElement $property.Value }
            return $map
        }
        'Array' { return ,@($Element.EnumerateArray() | ForEach-Object { ConvertFrom-JsonElement $_ }) }
        'String' { return $Element.GetString() }
        'Number' {
            $whole = [long]0
            if ($Element.TryGetInt64([ref]$whole)) { return $whole }
            return $Element.GetDouble()
        }
        'True' { return $true }
        'False' { return $false }
    }
    return $null
}

<#
.NOTES
Parsed with JavaScriptSerializer on Windows PowerShell and System.Text.Json on PowerShell 7, never
ConvertFrom-Json, whose objects fold the case of member names and so refuse two file keys that differ
only by case as though the index were not JSON at all. Each answers dictionaries with ordinal keys.
#>
function ConvertFrom-CatalogText([string]$Text) {
    if ($PSVersionTable.PSEdition -eq 'Core') {
        $document = [System.Text.Json.JsonDocument]::Parse($Text)
        try { return ConvertFrom-JsonElement $document.RootElement } finally { $document.Dispose() }
    }
    Add-Type -AssemblyName System.Web.Extensions
    $serializer = New-Object System.Web.Script.Serialization.JavaScriptSerializer
    $serializer.MaxJsonLength = [int]::MaxValue
    return $serializer.DeserializeObject($Text)
}

function Read-CatalogIndex([string]$Catalog) {
    $text = (Read-CatalogText $Catalog).TrimStart([char]0xFEFF)
    try { $index = ConvertFrom-CatalogText $text } catch { $index = $null }
    if (-not (Test-Map $index)) { Stop-Install "the catalog $Catalog is not a JSON catalog index" 'pass the catalog.json of a catalog, or its https url' }
    return $index
}

function Test-Map($Value) {
    return $Value -is [System.Collections.Generic.IDictionary[string, object]]
}

function Get-Field($Map, [string]$Name) {
    if ((Test-Map $Map) -and $Map.ContainsKey($Name)) { return $Map[$Name] }
    return $null
}

function Get-VersionEntry($Index, [string]$Catalog) {
    $item = Get-Field (Get-Field $Index 'items') $HostId
    if (-not (Test-Map $item) -or (Get-Field $item 'kind') -cne 'host') { Stop-Install "the catalog $Catalog lists no $HostId" 'install from a catalog that lists the host' }
    $version = "$(Get-Field $item 'latest')"
    $entry = $null
    if ($version -cmatch $VersionPattern) { $entry = Get-Field (Get-Field $item 'versions') $version }
    if (-not (Test-Map $entry)) { Stop-Install "the catalog lists no files for $HostId $version" "install from a catalog whose latest $HostId lists its files" }
    return @{ Version = $version; Entry = $entry }
}

function Get-BuildFiles($Entry, [string]$Version, [string]$Platform) {
    $listed = New-Object System.Collections.Specialized.OrderedDictionary ([System.StringComparer]::Ordinal)
    $own = Get-Field (Get-Field (Get-Field $Entry 'platforms') $Platform) 'files'
    foreach ($files in @((Get-Field $Entry 'files'), $own)) {
        if (-not (Test-Map $files)) { continue }
        foreach ($key in $files.Keys) {
            if ($listed.Contains($key)) {
                Stop-Install "the catalog lists the $HostId file `"$key`" both for every platform and for $Platform" 'install from a catalog built by forebay-host catalog build'
            }
            $listed[$key] = $files[$key]
        }
    }
    if ($listed.Count -eq 0) { Stop-Install "the catalog lists no $HostId $Version build for $Platform" "install from a catalog that lists a $HostId build for $Platform" }
    $required = Get-RequiredFiles $Platform
    $missing = @($required | Where-Object { -not $listed.Contains($_) })
    if ($missing.Count -gt 0) { Stop-Install "the catalog lists $HostId $Version without $($missing -join ', ')" "install from a catalog whose $HostId lists $($required -join ' ') for $Platform" }
    return $listed
}

<#
.NOTES
A file url is accepted only when it names no host and decodes to a path inside the catalog's folder,
the rule catalog.read applies to a catalog read from a file.
#>
function Resolve-FileUrl([string]$Directory, [string]$Url) {
    if (-not $Url.StartsWith('file:///') -or $Url -match '[?#\\\x00-\x1f]') { return $null }
    $segments = $Url.Substring(8).Split('/')
    $first = [System.Uri]::UnescapeDataString($segments[0])
    if ($first -match '^[A-Za-z]:$') { $path = $first + [System.IO.Path]::DirectorySeparatorChar; $rest = 1 } else { $path = [string][System.IO.Path]::DirectorySeparatorChar; $rest = 0 }
    for ($position = $rest; $position -lt $segments.Count; $position++) {
        $decoded = [System.Uri]::UnescapeDataString($segments[$position])
        if ($decoded -eq '' -or $decoded -eq '.' -or $decoded -eq '..' -or $decoded -match '[/\\:\x00-\x1f]') { return $null }
        $path = Join-Path $path $decoded
    }
    $root = [System.IO.Path]::GetFullPath($Directory).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    $comparison = if ([System.IO.Path]::DirectorySeparatorChar -eq '\') { [System.StringComparison]::OrdinalIgnoreCase } else { [System.StringComparison]::Ordinal }
    if (-not $path.StartsWith($root, $comparison) -or $path.Length -eq $root.Length) { return $null }
    return $path
}

function Resolve-LocalFile([string]$Directory, [string]$Url) {
    if ($Url -cmatch '^file:') { return Resolve-FileUrl $Directory $Url }
    if ($Url -eq '' -or $Url -match '^[A-Za-z][A-Za-z0-9+.-]*:' -or $Url.StartsWith('/') -or $Url -match '[?#\\\x00-\x1f]') { return $null }
    $path = $Directory
    foreach ($segment in $Url.Split('/')) {
        $decoded = [System.Uri]::UnescapeDataString($segment)
        if ($decoded -eq '' -or $decoded -eq '.' -or $decoded -eq '..' -or $decoded -match '[/\\:\x00-\x1f]') { return $null }
        $path = Join-Path $path $decoded
    }
    return $path
}

function Resolve-RemoteUrl([string]$Origin, [string]$Url) {
    if ($Url -match '[\x00-\x20]') { return $null }
    if ($Url -cmatch '^https://') { return $Url }
    if ($Url -eq '' -or $Url -match '^[A-Za-z][A-Za-z0-9+.-]*:' -or $Url.StartsWith('//')) { return $null }
    $base = $Origin -replace '[?#].*$', ''
    if ($Url.StartsWith('/')) { return [regex]::Match($base, '^https://[^/]*').Value + $Url }
    return $base.Substring(0, $base.LastIndexOf('/') + 1) + $Url
}

<#
.NOTES
A local url is decoded segment by segment into a path inside the catalog's folder rather than read
through System.Uri, which encodes its percent signs a second time.
#>
function Resolve-Source([string]$Catalog, $Url) {
    if (-not ($Url -is [string])) { return $null }
    if ($Catalog -match '^https://') {
        $remote = Resolve-RemoteUrl $Catalog $Url
        if ($null -eq $remote) { return $null }
        return @{ Local = $false; Source = $remote }
    }
    $local = Resolve-LocalFile (Split-Path -Parent $Catalog) $Url
    if ($null -eq $local) { return $null }
    return @{ Local = $true; Source = $local }
}

function Get-HostFiles([string]$Catalog, [string]$Platform) {
    $found = Get-VersionEntry (Read-CatalogIndex $Catalog) $Catalog
    $listed = Get-BuildFiles $found.Entry $found.Version $Platform
    $files = @()
    $seen = New-Object 'System.Collections.Generic.Dictionary[string,string]' ([System.StringComparer]::Ordinal)
    foreach ($key in $listed.Keys) {
        $folded = $key.Normalize().ToLowerInvariant()
        if ($seen.ContainsKey($folded)) {
            Stop-Install "the catalog lists the $HostId files $($seen[$folded]) and $key, which differ only by case and would install into one file" 'install from a catalog built by forebay-host catalog build'
        }
        $seen[$folded] = $key
        $reference = $listed[$key]
        $sha256 = Get-Field $reference 'sha256'
        $size = Get-Field $reference 'size'
        $source = Resolve-Source $Catalog (Get-Field $reference 'url')
        $usable = ($key -cmatch $FileKeyPattern) -and ($folded -cne $InstallMarker) -and ($null -ne $source) -and ($sha256 -is [string]) -and ($sha256 -cmatch $Sha256Pattern) -and ($size -is [int] -or $size -is [long]) -and ($size -ge 0)
        if (-not $usable) {
            Stop-Install "the catalog lists the $HostId file `"$key`" with a name, url, sha256 or size the installer refuses" 'install from a catalog built by forebay-host catalog build'
        }
        $files += @{ Key = $key; Local = $source.Local; Source = $source.Source; Sha256 = $sha256; Size = $size }
    }
    return @{ Version = $found.Version; Files = $files }
}

function Install-HostBuild([string]$Cache, [string]$Catalog, [string]$Platform) {
    $listing = Get-HostFiles $Catalog $Platform
    $directory = Join-Path $Cache "bundles\$HostId\$($listing.Version)"
    foreach ($file in $listing.Files) {
        $target = Join-Path $directory ($file.Key -replace '/', '\')
        if (Test-Sha256 $target $file.Sha256) { continue }
        Write-Fetching "fetching $HostId $($listing.Version) $($file.Key)"
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
        Receive-Verified $file $target
    }
    $executable = Join-Path $directory "$Platform\$HostId.exe"
    Write-InstallTrace "fetched $HostId $($listing.Version)"
    if ([System.Environment]::OSVersion.Platform -eq 'Win32NT' -and -not (Test-WindowsImage $executable)) {
        Stop-Install "$executable is not a Windows program" 'run this again; if it repeats, report the forebay-host build for this platform'
    }
    return $executable
}

function Initialize-Install([object[]]$Arguments) {
    if ($Arguments.Count -lt 1 -or -not "$($Arguments[0])") {
        Stop-Install 'no catalog was given, and none is published yet' 'pass the catalog to install from: install.ps1 <path or https url of catalog.json>'
    }
    $catalog = Resolve-Catalog "$($Arguments[0])"
    $rest = @()
    if ($Arguments.Count -gt 1) { $rest = @($Arguments[1..($Arguments.Count - 1)]) }
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    $executable = Install-HostBuild (Resolve-Cache) $catalog (Get-PlatformKey)
    return @{ Executable = $executable; Catalog = $catalog; Rest = $rest }
}

function ConvertTo-ArgumentText([string]$Argument) {
    if ($Argument -ne '' -and $Argument -notmatch '[\s"]') { return $Argument }
    $backslash = [char]92
    $builder = New-Object System.Text.StringBuilder
    [void]$builder.Append('"')
    $backslashes = 0
    foreach ($character in $Argument.ToCharArray()) {
        if ($character -eq $backslash) {
            $backslashes++
            continue
        }
        $count = if ($character -eq '"') { 2 * $backslashes + 1 } else { $backslashes }
        [void]$builder.Append($backslash, $count)
        [void]$builder.Append($character)
        $backslashes = 0
    }
    [void]$builder.Append($backslash, 2 * $backslashes)
    [void]$builder.Append('"')
    return $builder.ToString()
}

<#
.NOTES
Started through Process with UseShellExecute off rather than with &, which on PowerShell 7 hands a
file that is no program to ShellExecute, whose "Open with" dialog blocks a headless machine forever.
ArgumentList exists only on .NET Core; Windows PowerShell gets one command line quoted as
CommandLineToArgvW reads it.
#>
function Invoke-ForebayHost([string]$Executable, [string[]]$Arguments) {
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = $Executable
    $start.UseShellExecute = $false
    if ($null -ne $start.PSObject.Properties['ArgumentList']) {
        foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
    } else {
        $start.Arguments = ($Arguments | ForEach-Object { ConvertTo-ArgumentText $_ }) -join ' '
    }
    Write-InstallTrace "starting $Executable"
    $process = [System.Diagnostics.Process]::Start($start)
    Write-InstallTrace "started $($process.Id)"
    try {
        $process.WaitForExit()
        Write-InstallTrace "exited $($process.ExitCode)"
        return $process.ExitCode
    } finally {
        $process.Dispose()
    }
}

function Complete-Install([int]$Code) {
    # run through irm | iex there is no script file, and exit would close the user's shell
    if ($MyInvocation.ScriptName) { exit $Code }
    $global:LASTEXITCODE = $Code
}

if ($env:SPISOR_INSTALL_TRACE) { Set-PSDebug -Trace 1 }
Write-InstallTrace "running on $($PSVersionTable.PSEdition) $($PSVersionTable.PSVersion)"
$prepared = $null
try {
    $prepared = Initialize-Install $args
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
}
if ($null -eq $prepared) {
    Complete-Install 1
} else {
    $code = 1
    try {
        $code = Invoke-ForebayHost $prepared.Executable (@('install', "--catalog=$($prepared.Catalog)") + @($prepared.Rest))
    } catch {
        [Console]::Error.WriteLine("$($prepared.Executable) could not be started: $($_.Exception.Message)`nfix: run this again; if it repeats, report the forebay-host build for this platform")
    }
    Complete-Install $code
}
} @args
