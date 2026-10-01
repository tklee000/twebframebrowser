// Summarize observable failures and timing without treating HTTP 200 or hidden
// DOM text as a solved challenge. Query strings, tokens and script bodies are
// deliberately omitted from this report.
const fs = require('node:fs');
if (process.argv.length !== 4) {
  console.error('Usage: node summarize-page-diagnostic.cjs observation.log page-script-layout.json');
  process.exit(2);
}
const log = fs.readFileSync(process.argv[2], 'utf8');
const layout = JSON.parse(fs.readFileSync(process.argv[3], 'utf8'));
const messages = [...log.matchAll(/SCRIPT_MESSAGE ms=(\d+)([\s\S]*?)(?=SCRIPT_MESSAGE|OBSERVE|SCRIPT_CONSOLE|RUNTIME_ERROR|EXECUTION_TIME_LIMIT|$)/g)];
const events = messages.map((match) => ({
  ms: Number(match[1]),
  event: /\|event=([^|\r\n]*)/.exec(match[2])?.[1],
  code: /\|code=(\d+)/.exec(match[2])?.[1]
})).filter((event) => event.event && event.event !== 'food');
const testCallbacks = messages.map((match) => {
  const callback = /TEST_ONLY\|(pass|fail)\|(ready|success|error|timeout)\|([^\r\n]*)/.exec(match[2]);
  return callback && {ms:Number(match[1]),testCase:callback[1],state:callback[2],details:callback[3].split('|error=')[0]};
}).filter(Boolean);
const runtimes = [];
function visit(frame, framePath) {
  if (frame.runtime) {
    const runtime = frame.runtime;
    runtimes.push({
      frame: framePath, scriptJobs: runtime.scriptJobs,
      scriptCpuMs: runtime.scriptCpuMs, scriptWallMs: runtime.scriptJobMs,
      longestScriptCpuMs: runtime.longestScriptCpuMs,
      longestScriptWallMs: runtime.longestScriptJobMs,
      instructions: runtime.instructions,
      errors: [...new Set((runtime.createdErrors || []).map((error) => error.split('\n')[0]))],
      failedOperations: [...new Set((runtime.failedCalls || []).map((failure) => failure.split(' | ')[0]))],
      httpResults: [...new Set(runtime.requests || [])]
    });
  }
  (frame.frames || []).forEach((child, index) => visit(child, `${framePath}.${index}`));
}
visit(layout, 'top');
console.log(JSON.stringify({
  observationAbortedByHost: /EXECUTION_TIME_LIMIT 1\b/.test(log),
  uncaughtRuntimeError: /^RUNTIME_ERROR (.*)$/m.exec(log)?.[1]?.trim() || '',
  widgetReportedFailureCodes: [...new Set(events.filter((event) => event.event === 'fail' && event.code).map((event) => event.code))],
  widgetCompletionReported: events.some((event) => event.event === 'complete'),
  productionSuccessVerified: false,
  longCpuJobObserved: runtimes.some((runtime) => runtime.longestScriptCpuMs >= 10000),
  events, testCallbacks, runtimes,
  interpretation: 'Caught exceptions and optional feature probes alone do not establish a fatal failure. A host observation deadline is distinct from a server timeout. Official test-key callbacks do not establish production success.'
}, null, 2));
