const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const settings = require('../web/device-settings.js');
const script = fs.readFileSync(path.join(__dirname, '../web/app.js'), 'utf8');
const start = script.indexOf('let sessionBootstrap =');
const end = script.indexOf('async function refreshSetupStatus', start);
assert(start >= 0 && end > start);
const old = 'a'.repeat(64), fresh = 'b'.repeat(64), revision = '"' + 'c'.repeat(64) + '"';
const reply = (status, value = {}, headers = {}) => ({status, ok:status<400, statusText:'Unavailable', headers:new Headers(headers), json:async()=>value});
function fixture(handler) {
  const calls = [];
  const context = {Headers, csrf:'', lastHealth:null, configurationRevisions:new Map(), latestConfigurationRevisions:new Map(),
    encodingFormDirty:false, algorithmFormDirty:false, exposureFormDirty:false,
    fetch:async(route,options)=>{calls.push({route,options});return handler(route,options,calls)},
    window:{NuvoDeviceSettings:settings}};
  // The shared request helper accepts a fetch dependency; keep both paths on
  // this fixture without changing the browser's actual fetch behavior.
  context.window={NuvoDeviceSettings:{request:(route,options,state)=>settings.request(route,options,{...state,fetch:context.fetch})}};
  vm.createContext(context); vm.runInContext(script.slice(start,end),context);
  return {context,calls};
}
(async()=>{
  let release;
  const pending = new Promise(resolve=>{release=resolve});
  const f=fixture(async route=>route==='/api/v1/session'?(await pending,reply(200,{csrf:fresh,needs_owner_password:true})):reply(200));
  const first=f.context.api('/api/v1/health'), second=f.context.api('/api/v1/capabilities');
  assert.equal(f.calls.length,1);assert.equal(f.calls[0].route,'/api/v1/session');
  assert.equal(f.calls[0].options.credentials,'same-origin');assert.equal(f.calls[0].options.cache,'no-store');
  release();await Promise.all([first,second]);
  assert.equal(f.context.csrf,fresh);assert.equal(f.calls.filter(c=>c.route==='/api/v1/session').length,1);
  assert(f.calls.every(c=>!c.options.method||c.options.method==='GET'));

  // Expired/restarted gateway sessions recover automatically once. A settings
  // retry must use the new CSRF and the same edit-base ETag/body.
  const g=fixture((route,options)=>route==='/api/v1/session'?reply(200,{csrf:fresh}):
    reply(options.headers.get('X-Nuvo-CSRF')===old?401:202,{}, {'ETag':revision}));
  g.context.csrf=old;g.context.configurationRevisions.set('/api/v1/stereo',revision);
  const body=fs.readFileSync(path.join(__dirname,'../config/stereo.json'),'utf8');
  await g.context.api('/api/v1/stereo',{method:'POST',body});
  const attempts=g.calls.filter(c=>c.route==='/api/v1/stereo');
  assert.equal(attempts.length,2);
  assert.equal(attempts[1].options.headers.get('X-Nuvo-CSRF'),fresh);
  assert.equal(attempts[1].options.headers.get('If-Match'),revision);
  assert.equal(attempts[1].options.body,body);

  const denied=fixture(route=>route==='/api/v1/session'?reply(200,{csrf:fresh}):reply(401));
  denied.context.csrf=old;
  await assert.rejects(denied.context.api('/api/v1/health'),/session expired/);
  assert.equal(denied.calls.length,3,'one failed request, one bootstrap, one retry');
  let available=false;
  const retry=fixture(route=>route==='/api/v1/session'?reply(available?200:503,{csrf:fresh}):reply(200));
  await assert.rejects(retry.context.api('/api/v1/health'),/connection unavailable/);
  available=true;await retry.context.api('/api/v1/health');
  assert.equal(retry.calls.filter(c=>c.route==='/api/v1/session').length,2);
  // Periodic settings polling must start streams when the initial page-open
  // bootstrap failed before encoding was ever loaded.
  let online=false, started=0;
  const recovery=fixture(route=>route==='/api/v1/session'?reply(online?200:503,{csrf:fresh}):
    reply(200,{mode:'both',cadence_hz:10},{'ETag':revision}));
  Object.assign(recovery.context,{encoding:null,initialEncodingLoaded:false,encodingPreviewPending:false,showEncodingConfig(){},setFlowState(){},
    restartStreams(){started++},$:()=>({textContent:''})});
  const encodingStart=script.indexOf('async function refreshEncoding()');
  const encodingEnd=script.indexOf('function requestedEncoding()',encodingStart);
  vm.runInContext(script.slice(encodingStart,encodingEnd),recovery.context);
  await recovery.context.refreshEncoding();assert.equal(started,0);
  online=true;await recovery.context.refreshEncoding();assert.equal(started,1);
  await recovery.context.refreshEncoding();assert.equal(started,1,'unchanged polls retain existing streams');
  assert.equal(recovery.calls.filter(c=>c.route==='/api/v1/session').length,2);
  const malformed=fixture(()=>reply(200,{csrf:'invalid'}));
  await assert.rejects(malformed.context.api('/api/v1/health'),/session is invalid/);
  assert(!script.includes('needs_owner_password')&&!script.includes('claimDialog')&&!script.includes('ownerDialog'));
  console.log('Web automatic session bootstrap, recovery and CSRF tests passed');
})().catch(error=>{console.error(error);process.exitCode=1});
