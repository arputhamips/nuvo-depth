const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const script = fs.readFileSync(path.join(__dirname, '../web/app.js'), 'utf8');
const start = script.indexOf('let sessionBootstrap =');
const end = script.indexOf('async function refreshSetupStatus', start);
assert(start >= 0 && end > start);
const calls = [], responses = [];
const deviceSettings=require('../web/device-settings.js');
const validStereoBody=fs.readFileSync(path.join(__dirname,'../config/stereo.json'),'utf8');
const context = {
  lastHealth:null,
  Headers, csrf: 'csrf',
  configurationRevisions: new Map(), latestConfigurationRevisions: new Map(),
  encodingFormDirty:false, algorithmFormDirty:false, exposureFormDirty:false,
  fetch: async (url, options) => {calls.push({url, options}); return responses.shift()},
};
context.window={NuvoDeviceSettings:{request:(route,options,state)=>deviceSettings.request(route,options,{...state,fetch:context.fetch})}};
vm.createContext(context);
vm.runInContext(script.slice(start, end), context);
const old = '"' + 'a'.repeat(64) + '"', newer = '"' + 'b'.repeat(64) + '"';
const response = (etag, status=200) => ({
  status, ok:status<400, statusText:'Conflict',
  headers:new Headers(etag ? {'ETag':etag} : {}), json:async()=>({error:'reload before saving'}),
});
(async()=>{
  responses.push(response(old)); await context.api('/api/v1/stereo');
  context.algorithmFormDirty=true;
  responses.push(response(newer)); await context.api('/api/v1/stereo');
  assert.equal(context.configurationRevisions.get('/api/v1/stereo'), old);
  assert.equal(context.latestConfigurationRevisions.get('/api/v1/stereo'), newer);
  responses.push(response(null,412));
  await assert.rejects(context.api('/api/v1/stereo',{method:'POST',body:validStereoBody}),/reload before saving/);
  assert.equal(calls.at(-1).options.headers.get('If-Match'),old);
  assert.equal(context.configurationRevisions.get('/api/v1/stereo'),old);
  assert.equal(calls.at(-1).options.headers.get('X-Nuvo-CSRF'),'csrf');
  context.algorithmFormDirty=false;
  responses.push(response(newer)); await context.api('/api/v1/stereo');
  responses.push(response(newer,202)); await context.api('/api/v1/stereo',{method:'POST',body:validStereoBody});
  assert.equal(calls.at(-1).options.headers.get('If-Match'),newer);
  await assert.rejects(context.api('/api/v1/exposure',{method:'POST',body:'{}'}),/Load device settings/);
  responses.push(response(null));
  await assert.rejects(context.api('/api/v1/encoding'),/revision is unavailable/);

  // Discard must restore the latest polled settings and their revision together.
  // Otherwise a subsequent edit starts from one value but submits another ETag.
  const handlers = new Map(), elements = new Map();
  context.$ = id => {
    if (!elements.has(id)) elements.set(id,{textContent:'old error',addEventListener:(_event,callback)=>handlers.set(id,callback)});
    return elements.get(id);
  };
  for (const [name, value, dirty, show] of [
    ['encoding',{cadence_hz:20},'encodingFormDirty','showEncodingConfig'],
    ['algorithm',{search_range:128},'algorithmFormDirty','showStereoConfig'],
  ]) {
    const route = name==='encoding'?'/api/v1/encoding':'/api/v1/stereo';
    context[name==='encoding'?'encoding':'stereo']=value;
    context[dirty]=true;
    context.configurationRevisions.set(route,old);
    context.latestConfigurationRevisions.set(route,newer);
    let rendered;
    context[show]=config=>{rendered=config};
    const handlerStart=script.indexOf(`$('${name}Reset').addEventListener`);
    const handlerEnd=script.indexOf('\n});',handlerStart)+4;
    assert(handlerStart>=0&&handlerEnd>handlerStart);
    vm.runInContext(script.slice(handlerStart,handlerEnd),context);
    handlers.get(`${name}Reset`)();
    assert.equal(rendered,value);
    assert.equal(context[dirty],false);
    assert.equal(context.configurationRevisions.get(route),newer);
    assert.equal(elements.get(`${name}Error`).textContent,'');
  }
  console.log('Web configuration edit-base revision tests passed');
})().catch(error=>{console.error(error); process.exitCode=1});
