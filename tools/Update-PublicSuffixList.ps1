param(
    [string]$OutputFile = (Join-Path $PSScriptRoot '..\TWebFrame2\src\PublicSuffixRules.inc')
)

$ErrorActionPreference = 'Stop'
$sourceUrl = 'https://publicsuffix.org/list/public_suffix_list.dat'
$response = Invoke-WebRequest -Uri $sourceUrl -TimeoutSec 30 -UseBasicParsing
$buffer = [System.IO.MemoryStream]::new()
try {
    $response.RawContentStream.Position = 0
    $response.RawContentStream.CopyTo($buffer)
    $sourceBytes = $buffer.ToArray()
} finally {
    $buffer.Dispose()
}
$sourceText = [System.Text.Encoding]::UTF8.GetString($sourceBytes)
$idn = [System.Globalization.IdnMapping]::new()
$rules = [System.Collections.Generic.List[string]]::new()
foreach ($line in ($sourceText -split '\r?\n')) {
    $rule = $line.Trim()
    if (!$rule -or $rule.StartsWith('//')) { continue }
    $prefix = ''
    if ($rule.StartsWith('!')) { $prefix = '!' }
    elseif ($rule.StartsWith('*.')) { $prefix = '*.' }
    $domainName = $idn.GetAscii($rule.Substring($prefix.Length)).ToLowerInvariant()
    $rules.Add('L"' + $prefix + $domainName + '",')
}
$sha = [System.Security.Cryptography.SHA256]::Create()
try { $digest = [BitConverter]::ToString($sha.ComputeHash($sourceBytes)).Replace('-', '').ToLowerInvariant() }
finally { $sha.Dispose() }
$header = @(
    '// Generated from https://publicsuffix.org/list/public_suffix_list.dat'
    '// Mozilla Public Suffix List, including ICANN and PRIVATE sections.'
    '// This Source Code Form is subject to the terms of the Mozilla Public'
    '// License, v. 2.0. https://mozilla.org/MPL/2.0/'
    ('// Source SHA256: ' + $digest)
)
[System.IO.File]::WriteAllText(
    [System.IO.Path]::GetFullPath($OutputFile),
    (($header + $rules.ToArray()) -join [char]10) + [char]10,
    [System.Text.Encoding]::ASCII
)
Write-Output ('Wrote ' + $rules.Count + ' public suffix rules')
