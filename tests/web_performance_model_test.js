const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../web/app.js'), 'utf8');
const start = source.indexOf('// Performance sample model.');
const end = source.indexOf('// Performance monitor rendering:', start);
assert(start >= 0 && end > start);
const context = {};
vm.createContext(context);
vm.runInContext(source.slice(start, end), context);
const model = {run:null,published:null,samples:[],latest:null,stale:false};
const health = {
  run_uuid:'run-a',published_frames:1,processing_ms:309.5,
  rectification_ms:8.3,stereo_preprocess_ms:.2,
  stereo_match_ms:294.6,stereo_postprocess_ms:3.8,
};
context.recordPerformanceSample(model, health, 1000);
context.recordPerformanceSample(model, health, 2000);
assert.equal(model.samples.length, 1, 'repeated polls of one frame are not new samples');
context.recordPerformanceSample(model, {...health,published_frames:2,processing_ms:320}, 3000);
assert.equal(context.sampledP95(model.samples), 320);
const stages = context.performanceStages(health);
assert.equal(stages[0].name, 'Stereo match');
assert.equal(stages[0].share.toFixed(1), '95.2');
assert.equal(stages.find(s=>s.name==='Depth + overhead').value.toFixed(1), '2.6');
assert.equal(context.performanceStages({...health,stereo_preprocess_ms:null}).find(s=>s.name==='Depth + overhead').value, null,
  'missing timings must not be represented as measured zero or invented overhead');
assert.equal(context.performanceStages({...health,processing_ms:1}).find(s=>s.name==='Depth + overhead').value, null,
  'inconsistent stage totals must not produce a fabricated remainder');
context.recordPerformanceSample(model, {...health,published_frames:3}, 64000);
assert.equal(model.samples.length, 1, 'history is limited to the current 60-second window');
context.recordPerformanceSample(model, {...health,run_uuid:'run-b'}, 65000);
assert.equal(model.samples.length, 1, 'a restarted device must begin a new history');
context.recordPerformanceSample(model, {...health,run_uuid:'run-b',published_frames:0,processing_ms:null}, 66000);
assert.equal(model.samples.length, 0, 'counter rollback clears samples from the previous run');
assert.equal(context.sampledP95(model.samples), null);
context.recordPerformanceSample(model, {...health,run_uuid:'run-b',published_frames:4,processing_ms:NaN}, 67000);
assert.equal(model.samples.length, 0, 'invalid compute values are not observations');
context.recordPerformanceSample(model, {...health,run_uuid:'run-b',published_frames:4,processing_ms:315}, 68000);
assert.equal(model.samples.length, 1, 'a previously unavailable timing can be sampled once it becomes valid');

// The daemon reports stage activity at the instant of its status snapshot.
// A cadence-limited matcher is often idle between two successfully published frames.
const rateStart = source.indexOf('const now=performance.now();', source.indexOf('async function refreshHealth()'));
const rateEnd = source.indexOf('lastHealthRun=health.run_uuid', rateStart);
assert(rateStart > 0 && rateEnd > rateStart);
const hostSource = fs.readFileSync(path.join(__dirname, '../web/pipeline-host.js'), 'utf8');
const hostRateStart = hostSource.indexOf('depthOutputRate=lastRun===');
const hostRateEnd = hostSource.indexOf('lastPublished=health.matched_frames', hostRateStart);
assert(hostRateStart > 0 && hostRateEnd > hostRateStart);
for (const [published, expected] of [[44, 4], [40, 0]]) {
  const healthSample = {run_uuid:'run-a',published_frames:published,matcher_active:false};
  const pageContext = {health:healthSample,performance:{now:()=>2000},lastHealthRun:'run-a',lastHealthPublished:40,lastHealthAt:1000,depthOutputRate:null,setMeasurement:()=>{},$:()=>({}),depthPublicationCount:context.depthPublicationCount};
  vm.runInNewContext(source.slice(rateStart, rateEnd), pageContext);
  assert.equal(pageContext.depthOutputRate, expected, 'main page must report observed publications even between matcher work');
  const hostContext = {health:healthSample,now:2000,lastRun:'run-a',lastPublished:40,lastHealthAt:1000,depthOutputRate:null};
  vm.runInNewContext(hostSource.slice(hostRateStart, hostRateEnd), hostContext);
  assert.equal(hostContext.depthOutputRate, expected, 'pipeline must report observed publications even between matcher work');
}
const mixedHealth={run_uuid:'run-a',published_frames:50,matched_frames:41,matcher_active:false};
const mixedPage={health:mixedHealth,performance:{now:()=>2000},lastHealthRun:'run-a',lastHealthPublished:40,lastHealthAt:1000,
  depthOutputRate:null,setMeasurement:()=>{},$:()=>({}),depthPublicationCount:context.depthPublicationCount};
vm.runInNewContext(source.slice(rateStart,rateEnd),mixedPage);
assert.equal(mixedPage.depthOutputRate,1,'10 Hz raw publications must report the 1 Hz matcher rate');
const mixedHost={health:mixedHealth,now:2000,lastRun:'run-a',lastPublished:40,lastHealthAt:1000,depthOutputRate:null};
vm.runInNewContext(hostSource.slice(hostRateStart,hostRateEnd),mixedHost);
assert.equal(mixedHost.depthOutputRate,1);
const mixedModel={run:null,published:null,samples:[],latest:null,stale:false};
context.recordPerformanceSample(mixedModel,{run_uuid:'mixed',published_frames:10,matched_frames:1,processing_ms:1,matcher_statistics:{processing_ms:100}},1000);
context.recordPerformanceSample(mixedModel,{run_uuid:'mixed',published_frames:20,matched_frames:2,processing_ms:1,matcher_statistics:{processing_ms:110}},2000);
context.recordPerformanceSample(mixedModel,{run_uuid:'mixed',published_frames:30,matched_frames:2,processing_ms:1,matcher_statistics:{processing_ms:110}},3000);
assert.equal(mixedModel.samples.length,2,'mono completions cannot create stereo timing samples');
assert.equal(mixedModel.latest.processing_ms,110,'latest matcher timings survive intervening image-only completions');
console.log('Web performance sampling and timing-accounting tests passed');
