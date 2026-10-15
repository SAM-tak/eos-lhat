# Resolve published dependency commits before updating any revision files.
[CmdletBinding()]
param(
    [ValidateSet('all', 'lhat', 'eos-sdk')]
    [string]$Dependency = 'all',
    [string]$LhatRef = 'refs/heads/main',
    [string]$SdkRef = 'refs/heads/main'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dependencies = @(
    @{ Name = 'lhat'; Repository = 'git@github.com:SAM-tak/lhat.git'; Ref = $LhatRef },
    @{ Name = 'eos-sdk'; Repository = 'git@github.com:SAM-tak/eos-sdk-private.git'; Ref = $SdkRef }
)
$updates = @()

foreach ($item in $dependencies) {
    if ($Dependency -ne 'all' -and $Dependency -ne $item.Name) { continue }
    if ($item.Ref -notmatch '^refs/(heads|tags)/.+') {
        throw 'Specify a full branch or tag ref (refs/heads/main or refs/tags/v1.0).'
    }
    # Query peeled tags as well so annotated tags resolve to a commit ID.
    $lines = @(git ls-remote --exit-code $item.Repository $item.Ref "$($item.Ref)^{}")
    if ($LASTEXITCODE -ne 0) {
        throw "Cannot resolve $($item.Name) ref $($item.Ref). Check the ref and Git SSH access."
    }
    $refs = @{}
    foreach ($line in $lines) {
        $parts = $line -split '\s+', 2
        if ($parts.Count -eq 2) { $refs[$parts[1]] = $parts[0] }
    }
    $revision = $refs["$($item.Ref)^{}"]
    if (-not $revision) { $revision = $refs[$item.Ref] }
    if ($revision -cnotmatch '^[0-9a-f]{40}$') {
        throw "Invalid or missing revision for $($item.Name): $($item.Ref)"
    }
    $updates += @{ Name = $item.Name; Revision = $revision }
}

foreach ($update in $updates) {
    $path = Join-Path $root "$($update.Name).rev"
    $current = if (Test-Path -LiteralPath $path) { (Get-Content -Raw -LiteralPath $path).Trim() } else { '' }
    if ($current -ceq $update.Revision) {
        Write-Host "$($update.Name).rev: unchanged ($current)"
        continue
    }
    [System.IO.File]::WriteAllText($path, "$($update.Revision)`n", [System.Text.UTF8Encoding]::new($false))
    Write-Host "$($update.Name).rev: $current -> $($update.Revision)"
}
