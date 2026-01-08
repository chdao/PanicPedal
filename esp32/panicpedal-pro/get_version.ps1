# PowerShell script to get git commit hash and create version header
# This can be run before building to inject the git commit hash

$gitHash = git rev-parse --short HEAD
if ($LASTEXITCODE -ne 0) {
    $gitHash = "unknown"
}

$versionHeader = @"
// Auto-generated version header - do not edit manually
// Generated from git commit hash: $gitHash
#ifndef FIRMWARE_VERSION_H
#define FIRMWARE_VERSION_H
#define FIRMWARE_VERSION "$gitHash"
#endif
"@

$versionHeader | Out-File -FilePath "version.h" -Encoding ASCII -NoNewline

Write-Host "Generated version.h with commit hash: $gitHash"
