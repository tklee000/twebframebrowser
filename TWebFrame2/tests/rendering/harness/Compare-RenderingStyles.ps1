function Get-RenderingStyleProperties {
    @('display','position','visibility','box-sizing','white-space','direction','text-align','font-style','font-weight','font-kerning','pointer-events','overflow-x','overflow-y','flex-direction','flex-wrap','flex-grow','flex-shrink','list-style-position','list-style-type','opacity','letter-spacing','line-height','font-size','color','background-color')
}
function Convert-RenderingStyleColor([string]$Value) {
    $v=$Value.Trim().ToLowerInvariant()
    if($v -match '^#([0-9a-f]{6})([0-9a-f]{2})?$'){
        return $Matches[1]+$(if($Matches[2]){$Matches[2]}else{'ff'})
    }
    if($v -eq 'transparent'){return '00000000'}
    if($v -notmatch '^rgba?\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)(?:\s*,\s*(\d*\.?\d+))?\s*\)$'){throw 'Unsupported color serialization'}
    $components=@([int]$Matches[1],[int]$Matches[2],[int]$Matches[3])
    $alpha=if($Matches[4]){[double]::Parse($Matches[4],[Globalization.CultureInfo]::InvariantCulture)}else{1.0}
    if(@($components|Where-Object {$_ -lt 0 -or $_ -gt 255}).Count -or $alpha -lt 0 -or $alpha -gt 1){throw 'Invalid color component'}
    return ('{0:x2}{1:x2}{2:x2}{3:x2}' -f $components[0],$components[1],$components[2],[int][Math]::Floor($alpha*255+0.5))
}
function Convert-RenderingStyleNumber([string]$Value,[bool]$Length) {
    $v=$Value.Trim().ToLowerInvariant()
    $pattern=if($Length){'^([-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:e[-+]?\d+)?)px$'}else{'^([-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:e[-+]?\d+)?)$'}
    if($v -notmatch $pattern){throw 'Invalid numeric style serialization'}
    $number=[double]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture)
    if([double]::IsNaN($number) -or [double]::IsInfinity($number)){throw 'Non-finite style number'}
    return $number
}
function Compare-RenderingStyles($Reference,$Native) {
    $differences=[Collections.Generic.List[object]]::new();$checked=0;$missing=0
    foreach($property in Get-RenderingStyleProperties){
        $r=if($null -ne $Reference){$Reference.PSObject.Properties[$property]}else{$null}
        $n=if($null -ne $Native){$Native.PSObject.Properties[$property]}else{$null}
        $rv=if($null -ne $r){[string]$r.Value}else{''};$nv=if($null -ne $n){[string]$n.Value}else{''}
        if([string]::IsNullOrWhiteSpace($rv) -or [string]::IsNullOrWhiteSpace($nv)){
            $missing++;$differences.Add(@{property=$property;message='Required computed style is missing';reference=$rv;native=$nv});continue
        }
        $checked++;$rv=$rv.Trim().ToLowerInvariant();$nv=$nv.Trim().ToLowerInvariant();$same=$false
        try {
            if($property -in @('color','background-color')){
                $same=(Convert-RenderingStyleColor $rv) -ceq (Convert-RenderingStyleColor $nv)
            }elseif($property -in @('opacity','flex-grow','flex-shrink','font-weight')){
                if($property -eq 'font-weight'){
                    if($rv -eq 'normal'){$rv='400'}elseif($rv -eq 'bold'){$rv='700'}
                    if($nv -eq 'normal'){$nv='400'}elseif($nv -eq 'bold'){$nv='700'}
                }
                $same=[Math]::Abs((Convert-RenderingStyleNumber $rv $false)-(Convert-RenderingStyleNumber $nv $false)) -le 0.000001
            }elseif($property -in @('font-size','line-height','letter-spacing') -and ($rv -ne 'normal' -or $nv -ne 'normal')){
                $same=[Math]::Abs((Convert-RenderingStyleNumber $rv $true)-(Convert-RenderingStyleNumber $nv $true)) -le (1.0/64.0)
            }else{
                if($property -eq 'text-align'){
                    if($rv -eq 'start'){$rv=if($Reference.direction -eq 'rtl'){'right'}else{'left'}}
                    elseif($rv -eq 'end'){$rv=if($Reference.direction -eq 'rtl'){'left'}else{'right'}}
                    if($nv -eq 'start'){$nv=if($Native.direction -eq 'rtl'){'right'}else{'left'}}
                    elseif($nv -eq 'end'){$nv=if($Native.direction -eq 'rtl'){'left'}else{'right'}}
                }
                $same=$rv -ceq $nv
            }
            if(-not $same){$differences.Add(@{property=$property;message='Computed style differs';reference=$rv;native=$nv})}
        }catch{$differences.Add(@{property=$property;message=$_.Exception.Message;reference=$rv;native=$nv})}
    }
    [pscustomobject]@{passed=($differences.Count -eq 0);checked=$checked;missing=$missing;differences=@($differences.ToArray())}
}
