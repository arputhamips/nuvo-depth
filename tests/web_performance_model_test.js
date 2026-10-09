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
console.log('Web performance sampling and timing-accounting tests passed');
