& {
$ErrorActionPreference = 'Stop'

# the patterns of runtime/schema/catalog.schema.json, verbatim; install_script_test fails when the schema changes them
$FileKeyPattern = '^[^/\\:]*[^/\\:. ](?:/[^/\\:]*[^/\\:. ])*(?![\s\S])'
$VersionPattern = '^(?![\s\S]*\.\.)[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]*[0-9A-Za-z-])?(\+[0-9A-Za-z.-]*[0-9A-Za-z-])?(?![\s\S])'
$Sha256Pattern = '^[0-9a-f]{64}(?![\s\S])'

$HostId = 'forebay-host'
$InstallMarker = '.installed.json'

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

function Receive-File([string]$Url, [string]$Path) {
    $client = New-Object System.Net.WebClient
    try {
        $client.DownloadFile($Url, $Path)
    } catch {
        Stop-Install "could not fetch $Url" 'check the network connection, then run this again'
    } finally {
        $client.Dispose()
    }
}

function New-TemporaryPath([string]$Path) {
    return "$Path.$([guid]::NewGuid().ToString('N')).part"
}

function Remove-Temporary([string]$Part) {
    if (Test-Path -LiteralPath $Part) { Remove-Item -LiteralPath $Part -Force -ErrorAction SilentlyContinue }
}

function Receive-Verified([string]$Url, [string]$Path, [string]$Sha256, $Size) {
    $part = New-TemporaryPath $Path
    try {
        Receive-File $Url $part
        if ((Get-Item -LiteralPath $part).Length -ne [long]$Size -or -not (Test-Sha256 $part $Sha256)) {
            Stop-Install "$Url does not have the $Size bytes and SHA-256 $Sha256 it is pinned to" 'run this again; if it repeats, the download is being altered on the way'
        }
        Move-Item -LiteralPath $part -Destination $Path -Force
    } finally {
        Remove-Temporary $part
    }
}

function Read-CatalogText([string]$Catalog) {
    if ($Catalog -match '^https://') {
        $client = New-Object System.Net.WebClient
        try { return [System.Text.Encoding]::UTF8.GetString($client.DownloadData($Catalog)) } catch { Stop-Install "could not fetch the catalog $Catalog" 'check the network connection and the url, then run this again' } finally { $client.Dispose() }
    }
    return [System.IO.File]::ReadAllText($Catalog, [System.Text.Encoding]::UTF8)
}

<#
.NOTES
Parsed with JavaScriptSerializer, not ConvertFrom-Json, whose objects fold the case of member names
and so refuse two file keys that differ only by case as though the index were not JSON at all.
#>
function Read-CatalogIndex([string]$Catalog) {
    $text = (Read-CatalogText $Catalog).TrimStart([char]0xFEFF)
    Add-Type -AssemblyName System.Web.Extensions
    $serializer = New-Object System.Web.Script.Serialization.JavaScriptSerializer
    $serializer.MaxJsonLength = [int]::MaxValue
    try { $index = $serializer.DeserializeObject($text) } catch { $index = $null }
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

function Test-Inside($Origin, $Url) {
    if ($Origin.Scheme -eq 'https') { return $Url.Scheme -eq 'https' }
    $directory = $Origin.AbsolutePath.Substring(0, $Origin.AbsolutePath.LastIndexOf('/') + 1)
    return $Url.Scheme -eq 'file' -and $Url.Host -eq '' -and $Url.AbsolutePath.StartsWith($directory, [System.StringComparison]::Ordinal)
}

function Get-HostFiles([string]$Catalog, [string]$Platform) {
    $found = Get-VersionEntry (Read-CatalogIndex $Catalog) $Catalog
    $listed = Get-BuildFiles $found.Entry $found.Version $Platform
    $origin = New-Object System.Uri($Catalog)
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
        $url = $null
        if ((Get-Field $reference 'url') -is [string]) { $url = New-Object System.Uri($origin, (Get-Field $reference 'url')) }
        $usable = ($key -cmatch $FileKeyPattern) -and ($folded -cne $InstallMarker) -and ($null -ne $url) -and (Test-Inside $origin $url) -and ($sha256 -is [string]) -and ($sha256 -cmatch $Sha256Pattern) -and ($size -is [int] -or $size -is [long]) -and ($size -ge 0)
        if (-not $usable) {
            Stop-Install "the catalog lists the $HostId file `"$key`" with a name, url, sha256 or size the installer refuses" 'install from a catalog built by forebay-host catalog build'
        }
        $files += @{ Key = $key; Url = $url.AbsoluteUri; Sha256 = $sha256; Size = $size }
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
        Receive-Verified $file.Url $target $file.Sha256 $file.Size
    }
    return Join-Path $directory "$Platform\$HostId.exe"
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

function Complete-Install([int]$Code) {
    # run through irm | iex there is no script file, and exit would close the user's shell
    if ($MyInvocation.ScriptName) { exit $Code }
    $global:LASTEXITCODE = $Code
}

$prepared = $null
try {
    $prepared = Initialize-Install $args
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
}
if ($null -eq $prepared) {
    Complete-Install 1
} else {
    $ErrorActionPreference = 'Continue'
    & $prepared.Executable install "--catalog=$($prepared.Catalog)" @($prepared.Rest)
    Complete-Install $LASTEXITCODE
}
} @args
