param([string]$CorpusRoot='')
$ErrorActionPreference='Stop'
if(-not $CorpusRoot){$CorpusRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))}
$manifest=Get-Content -LiteralPath (Join-Path $CorpusRoot 'corpus-manifest.json') -Raw -Encoding UTF8|ConvertFrom-Json
$features=@($manifest.cases|ForEach-Object {$_.features}|Sort-Object -Unique)
$pairs=[Collections.Generic.List[object]]::new()
for($i=0;$i -lt $features.Count;$i++){for($j=$i+1;$j -lt $features.Count;$j++){
    $ids=@($manifest.cases|Where-Object {$features[$i] -in $_.features -and $features[$j] -in $_.features}|ForEach-Object {$_.id})
    if($ids.Count){$pairs.Add(@{features=@($features[$i],$features[$j]);caseIds=$ids})}
}}
$quotas=@{cascade=100;block=120;text=110;flex=130;grid=140;table=80;position=110;paint=90;responsive=70;integration=50}
$families=@($quotas.Keys|Sort-Object|ForEach-Object {@{family=$_;current=@($manifest.cases|Where-Object family -eq $_).Count;target=$quotas[$_]}})
$inventory=[ordered]@{schemaVersion=1;corpusCount=$manifest.count;featureTags=$features;referenceAcceptance='Each capture saves reference-features.json from read-only CSS.supports; acceptance does not imply rendering correctness';nativeSupport='Per-feature conformance remains unknown until the full comparison contract passes';deferred=@('JavaScript DOM changes and events','animation/timeline sampling');notYetCovered=@('subgrid','writing-mode vertical layout','MathML','iframe child document comparison','local images and custom fonts','advanced transforms and perspective','filter and backdrop-filter','CSS masks and clip-path','container and style queries','scroll states','anchor positioning','advanced color spaces','text fragment and baseline matching')}
$coverage=[ordered]@{schemaVersion=1;generatorVersion=$manifest.generatorVersion;seed=$manifest.seed;documentCount=$manifest.count;targetDocumentCount=1000;requiredBaseComparisons=$manifest.count*6;familyQuotas=$families;authoredFeaturePairCount=$pairs.Count;authoredFeaturePairs=@($pairs.ToArray());pairwiseComplete=$false;threeWayComplete=$false;complexityQuotasVerified=$false;fullStandardCoverage=$false;note='Pairs describe authored feature tags only. Runtime acceptance, semantic coverage, complex nesting quotas and passing renders require separate evidence.'}
$encoding=[Text.UTF8Encoding]::new($false)
[IO.File]::WriteAllText((Join-Path $CorpusRoot 'feature-inventory.json'),($inventory|ConvertTo-Json -Depth 8),$encoding)
[IO.File]::WriteAllText((Join-Path $CorpusRoot 'coverage.json'),($coverage|ConvertTo-Json -Depth 10),$encoding)
Write-Output ('Coverage recorded for '+$manifest.count+' preserved fixtures and '+$pairs.Count+' authored feature pairs')
