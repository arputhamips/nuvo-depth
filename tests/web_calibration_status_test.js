const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../web/app.js'), 'utf8');
const start = source.indexOf('function calibrationCoverageCells(');
const end = source.indexOf('function renderCalibration(', start);
assert(start >= 0 && end > start);
const context = vm.createContext({});
vm.runInContext(source.slice(start, end), context);
const cells = status => Array.from(context.calibrationCoverageCells(status)).sort();
assert.deepEqual(cells({coverage:{occupied_cell_indices:[0,1,2,3,4,5,6,7,8]},
  captures:[{accepted:false,common_corners:73}]}), [0,1,2,3,4,5,6,7,8],
  'a rejected newest capture must not erase prior field coverage');
assert.deepEqual(cells({captures:[
  {accepted:true,quality:{left:{center_x:.1,center_y:.1}}},
  {accepted:true,quality:{left:{center_x:.8,center_y:.8}}},
  {accepted:false,quality:{left:{center_x:.5,center_y:.5}}}
]}), [0,8], 'older device responses retain their full-history coverage fallback');
assert.deepEqual(cells({coverage:{occupied_cell_indices:[]},
  captures:[{accepted:true,quality:{left:{center_x:.5,center_y:.5}}}]}), [],
  'an explicit empty server grid is authoritative');
assert.deepEqual(cells({coverage:{occupied_cell_indices:[-1,0,8,9,1.5,'2',0]}}), [0,8]);
assert.deepEqual(cells({}), []);
console.log('Calibration status coverage contract OK');
const profileStart=source.indexOf('function calibrationProfileId(');
const profileEnd=source.indexOf('function calibrationConfiguration(',profileStart);
vm.runInContext(source.slice(profileStart,profileEnd),context);
assert.equal(context.calibrationProfileId({profile_id:'balanced_728x544_10'}),'balanced_728x544_10');
assert.throws(()=>context.calibrationProfileId({}),/active capture profile/);
assert.throws(()=>context.calibrationProfileId({profile_id:'../other'}),/active capture profile/);
const html=fs.readFileSync(path.join(__dirname,'../web/index.html'),'utf8');
assert.match(html, /id="calibrationPresetId"[^>]*readonly/);
assert.match(source,/preset_id:calibrationProfileId\(lastHealth\)/);
