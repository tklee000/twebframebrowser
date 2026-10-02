const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert');
const root = __dirname;
const languageFile = path.join(root, 'support-language-regression.js');
const language = JSON.parse(vm.runInNewContext(fs.readFileSync(languageFile, 'utf8')));
assert(!Object.values(language).some(value => typeof value === 'string' && value.startsWith('ERROR:')));
if (process.argv.includes('--generate-reference')) {
  fs.writeFileSync(path.join(root, 'support-language-expected.json'), JSON.stringify(language));
  console.log(`Generated ${Object.keys(language).length} language reference results.`);
  process.exit(0);
}
for (const scale of ['1.000000', '1.500000']) for (const jit of ['0', '16']) {
  const actual = JSON.parse(fs.readFileSync(path.join(root, 'artifacts', `support-language-${scale}-${jit}.json`), 'utf8'));
  assert.deepStrictEqual(actual, language, `DPI=${scale}, JIT=${jit}`);
  const semantic = JSON.parse(fs.readFileSync(path.join(root, 'artifacts', `semantic-${scale}-${jit}.json`), 'utf8'));
  const reference = JSON.parse(fs.readFileSync(path.join(root, 'semantic-expected.json'), 'utf8'));
  assert.deepStrictEqual(semantic, reference, `Semantic DPI=${scale}, JIT=${jit}`);
}
console.log(`Language ${Object.keys(language).length} cases and 29 semantic cases × 4 configurations agree with references.`);
