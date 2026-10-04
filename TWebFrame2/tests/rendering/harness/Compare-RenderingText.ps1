function Compare-RenderingText($Reference, $Native) {
    $differences = [Collections.Generic.List[object]]::new()
    $referenceItems = @{}; $nativeItems = @{}
    foreach ($pair in @(@{items=$Reference; map=$referenceItems; engine='reference'}, @{items=$Native; map=$nativeItems; engine='native'})) {
        foreach ($item in $pair.items) {
            $key = $item.path + ':' + $item.start + ':' + $item.length
            if ($pair.map.ContainsKey($key)) {
                $differences.Add(@{kind='HARNESS_ERROR';message='Duplicate text source range';engine=$pair.engine;key=$key})
            }
            $pair.map[$key] = $item
            if (-not $item.mapped) {$differences.Add(@{kind='FAIL_TEXT';message='Text does not map to the authored source';engine=$pair.engine;key=$key})}
            if ($pair.engine -eq 'native' -and $null -ne $item.PSObject.Properties['glyph']) {
                if (-not $item.glyph.collected -or -not $item.glyph.fontResolved -or -not $item.glyph.family -or -not $item.glyph.postScript) {
                    $differences.Add(@{kind='FAIL_TEXT';message='Shaped glyph or resolved font metadata missing';key=$key})
                }
                if ($item.glyph.index -eq 0) {$differences.Add(@{kind='FAIL_TEXT';message='Missing glyph (.notdef)';key=$key})}
            }
        }
    }
    foreach ($key in $referenceItems.Keys) {
        $r = $referenceItems[$key]; $n = $nativeItems[$key]
        if ($null -eq $n) {$differences.Add(@{kind='FAIL_TEXT';message='Missing character range';key=$key;reference=$r});continue}
        if ($r.text -cne $n.text) {$differences.Add(@{kind='FAIL_TEXT';message='Character content differs';key=$key;reference=$r.text;native=$n.text})}
        if ($r.rect.Count -ne 4 -or $n.rect.Count -ne 4) {$differences.Add(@{kind='HARNESS_ERROR';message='Invalid character rect';key=$key});continue}
        for ($axis=0; $axis -lt 4; $axis++) {
            if ($null -eq $r.rect[$axis] -or $null -eq $n.rect[$axis] -or
                [double]::IsNaN([double]$r.rect[$axis]) -or [double]::IsNaN([double]$n.rect[$axis]) -or
                [double]::IsInfinity([double]$r.rect[$axis]) -or [double]::IsInfinity([double]$n.rect[$axis])) {
                $differences.Add(@{kind='HARNESS_ERROR';message='Non-finite or null character coordinate';key=$key;axis=$axis});continue
            }
            if ([Math]::Abs($r.rect[$axis]-$n.rect[$axis]) -gt (1.0/64.0)) {
                $differences.Add(@{kind='FAIL_TEXT';key=$key;axis=$axis;reference=$r.rect[$axis];native=$n.rect[$axis]})
            }
        }
    }
    foreach ($key in $nativeItems.Keys) {
        if (-not $referenceItems.ContainsKey($key)) {$differences.Add(@{kind='FAIL_TEXT';message='Unexpected character range';key=$key;native=$nativeItems[$key]})}
    }
    return [pscustomobject]@{passed=($differences.Count -eq 0);referenceCharacters=$referenceItems.Count;nativeCharacters=$nativeItems.Count;differences=@($differences.ToArray())}
}
