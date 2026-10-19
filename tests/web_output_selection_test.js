const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(require('node:path').join(__dirname, '../web/app.js'),'utf8');
const context = {TextDecoder, Uint8Array, DataView, Number, Set};
context.window={NuvoFrameCodec:require('../web/frame-codec.js')};
vm.createContext(context);
vm.runInContext(source.slice(source.indexOf('const OUTPUT_PRODUCTS'),source.indexOf('let capabilities')),context);
vm.runInContext(source.slice(source.indexOf('function outputSubscriptionPath'),source.indexOf('function updateOutputBudget')),context);
vm.runInContext(source.slice(source.indexOf('function parseFrame('),source.indexOf('function product(')),context);
vm.runInContext(source.slice(source.indexOf('function drawGray('),source.indexOf('function drawDepth(')),context);
const settings={products:['left_rgb','validity'],cadence_hz:5,delivery:'best_effort'};
assert.equal(context.outputSubscriptionPath('/api/v1/stream',settings),'/api/v1/stream?cadence_hz=5&products=left_rgb,validity&delivery=best_effort');
const image=context.outputBandwidth(['left_mono'],728,544,5);
assert.equal(image.frame_bytes,728*544+65568);
assert.equal(context.outputBandwidth(['rectified_stereo_rgb'],728,544,5).bytes_per_second,(728*544*6+65568)*5);
assert(context.outputBandwidth(['left_rgb'],728,544,5).bytes_per_second<8*1024*1024);
assert(context.outputBandwidth(['left_rgb','right_rgb'],728,544,5).bytes_per_second>8*1024*1024);
function frame(format,data,kind='left_rgb'){
  const metadata=Buffer.from(JSON.stringify({products:[{kind,format,width:2,height:1,offset:0,length:data.length}]}));
  const wire=Buffer.alloc(32+metadata.length+data.length);wire.write('NUV1');wire.writeUInt32BE(metadata.length,12);wire.writeBigUInt64BE(BigInt(data.length),16);wire.writeBigUInt64BE(7n,24);metadata.copy(wire,32);Buffer.from(data).copy(wire,32+metadata.length);
  return wire.buffer.slice(wire.byteOffset,wire.byteOffset+wire.byteLength);
}
let drawn;
const canvas={style:{},getContext:()=>({createImageData:(w,h)=>({data:new Uint8Array(w*h*4)}),putImageData:image=>drawn=image.data})};
for(const [format,expected] of [['rgb8',[11,22,33,255,44,55,66,255]],['bgr8',[33,22,11,255,66,55,44,255]]]){
  const parsed=context.parseFrame(frame(format,[11,22,33,44,55,66]));context.drawGray(canvas,parsed,parsed.metadata.products[0]);
  assert.deepEqual(Array.from(drawn),expected);
}
assert.throws(()=>context.parseFrame(frame('rgb8',[11,22])),/Invalid product/,'short RGB payload must reject instead of showing partial pixels');
let allocations=0,resizes=0,writes=0;
function trackedCanvas(){
  let width=0,height=0;
  return {style:{},get width(){return width},set width(value){width=value;resizes++},
    get height(){return height},set height(value){height=value;resizes++},
    getContext:()=>({createImageData:(w,h)=>{allocations++;return {data:new Uint8Array(w*h*4)}},
      putImageData:image=>{drawn=Array.from(image.data);writes++}})};
}
const codec=context.window.NuvoFrameCodec,firstCanvas=trackedCanvas(),secondCanvas=trackedCanvas();
function depthFrame(values){const bytes=Buffer.alloc(values.length*4);values.forEach((z,i)=>bytes.writeFloatLE(z,i*4));return context.parseFrame(frame('float32_le',bytes,'depth'));}
const numeric=depthFrame([1,NaN]);
assert.equal(codec.drawDepth(firstCanvas,numeric,numeric.metadata.products[0]),50);
const numericPixels=drawn;
codec.drawDepth(secondCanvas,numeric,numeric.metadata.products[0]);
assert.equal(allocations,1,'two visible destinations share one colour map and ImageData');
assert.deepEqual(drawn,numericPixels);
codec.drawDepth(firstCanvas,numeric,numeric.metadata.products[0]);
assert.equal(writes,3,'canvas pixels are restored even if a caller reset its backing store');
const invalid=depthFrame([NaN,-1]);codec.drawDepth(firstCanvas,invalid,invalid.metadata.products[0]);
assert.equal(allocations,1,'same-size frames reuse image storage');
assert.equal(resizes,4,'steady frame dimensions do not reset canvas backing stores');
assert.deepEqual(drawn,[0,0,0,255,0,0,0,255],'invalid depth clears pixels from the prior frame');
const disparity=context.parseFrame(frame('float32_le',Buffer.from(new Float32Array([32,64]).buffer),'disparity'));
codec.drawDisparity(firstCanvas,disparity,disparity.metadata.products[0],128);const wide=drawn;
codec.drawDisparity(secondCanvas,disparity,disparity.metadata.products[0],64);assert.notDeepEqual(drawn,wide);
codec.drawDisparity(trackedCanvas(),disparity,disparity.metadata.products[0],128);assert.deepEqual(drawn,wide,'alternate disparity scales cannot poison cached colour maps');
const visibleContext={document:{hidden:false},window:{NuvoFrameCodec:{drawGray:()=>writes++}},stereo:null,$:()=>({})};
vm.createContext(visibleContext);
vm.runInContext(source.slice(source.indexOf('function drawGray('),source.indexOf('function renderFrame(')),visibleContext);
const visible={closest:selector=>selector==='.page.active'?{}:null};
const hidden={closest:selector=>selector==='[hidden]'?{}:selector==='.page.active'?{}:null};
const beforeVisible=writes;
visibleContext.drawVisibleProduct(hidden,numeric,{kind:'left_mono'});
visibleContext.document.hidden=true;visibleContext.drawVisibleProduct(visible,numeric,{kind:'left_mono'});
assert.equal(writes,beforeVisible,'hidden destinations and hidden tabs do no pixel work');
visibleContext.document.hidden=false;visibleContext.drawVisibleProduct(visible,numeric,{kind:'left_mono'});
assert.equal(writes,beforeVisible+1,'newly visible destination renders the current frame');
assert(source.includes("document.addEventListener('visibilitychange',restartStreams)"),'hidden tabs must release stream demand');
const consoleTimers=[],consoleListeners={},consoleCalls={health:0,settings:0};
const consolePolling={setInterval:fn=>consoleTimers.push(fn),document:{hidden:false,addEventListener:(name,fn)=>consoleListeners[name]=fn},
  refreshHealth:()=>consoleCalls.health++,refreshTransport:()=>consoleCalls.settings++,refreshEncoding:()=>consoleCalls.settings++,
  refreshStereo:()=>consoleCalls.settings++,refreshExposure:()=>consoleCalls.settings++,refreshCapabilities:()=>consoleCalls.settings++};
const consoleStart=source.indexOf('setInterval(()=>{if(!document.hidden)refreshHealth();}');
vm.runInNewContext(source.slice(consoleStart,source.indexOf('\nsetInterval(()=>{if(lastFrameAt)',consoleStart)),consolePolling);
consoleTimers.forEach(fn=>fn());assert.deepEqual(consoleCalls,{health:1,settings:5});
consolePolling.document.hidden=true;consoleTimers.forEach(fn=>fn());consoleListeners.visibilitychange();
assert.deepEqual(consoleCalls,{health:1,settings:5},'hidden console must stop all routine endpoint polling');
consolePolling.document.hidden=false;consoleListeners.visibilitychange();assert.deepEqual(consoleCalls,{health:2,settings:10});
assert(!source.includes("socketUrl('/api/v1/stream')"),'browser must not request implicit legacy-all depth products');
const pipelineHost = fs.readFileSync(require('node:path').join(__dirname, '../web/pipeline-host.js'), 'utf8');
const pollingStart = pipelineHost.indexOf('  setInterval(()=>{if(!document.hidden)refreshHealth();}');
assert(pollingStart > 0);
const timers=[],listeners={},calls={health:0,capabilities:0,configuration:0,restart:0,refresh:0};
const pollingContext={
  setInterval:fn=>timers.push(fn),document:{hidden:false,addEventListener:(name,fn)=>listeners[name]=fn},
  window:{addEventListener:()=>{}},names:{exposure:[],algorithm:[],encoding:[]},
  refreshHealth:()=>calls.health++,refreshCapabilities:()=>calls.capabilities++,refreshConfiguration:()=>calls.configuration++,
  restartStreams:()=>calls.restart++,refresh:()=>calls.refresh++,
};
vm.runInNewContext(pipelineHost.slice(pollingStart,pipelineHost.lastIndexOf('\n})();')),pollingContext);
timers.forEach(fn=>fn());
assert.deepEqual(calls,{health:1,capabilities:1,configuration:3,restart:0,refresh:0});
pollingContext.document.hidden=true;listeners.visibilitychange();timers.forEach(fn=>fn());
assert.deepEqual(calls,{health:1,capabilities:1,configuration:3,restart:1,refresh:0},'hidden pipeline must release subscriptions and skip all polling');
pollingContext.document.hidden=false;listeners.visibilitychange();
assert.equal(calls.refresh,1,'returning to the pipeline must refresh settings before restarting streams');
const pipelineController=fs.readFileSync(require('node:path').join(__dirname,'../web/pipeline.js'),'utf8');
const encodedDemand=pipelineController.match(/wantsEncoded:\(\)=>([^,\n]+)/);
assert(encodedDemand,'pipeline must declare its preview demand');
const previewBody={hidden:false};
const demandContext={$:()=>previewBody,selected:'monitor',applied:{preview:true}};
vm.createContext(demandContext);
assert.equal(vm.runInContext(encodedDemand[1],demandContext),true);
previewBody.hidden=true;
assert.equal(vm.runInContext(encodedDemand[1],demandContext),false,'a minimized viewer must release encoded processing demand');
previewBody.hidden=false;demandContext.selected='out-1';
assert.equal(vm.runInContext(encodedDemand[1],demandContext),false,'a raw output must not silently keep the encoder active');
console.log('Web output selection, bandwidth, RGB/BGR, and demand lifecycle tests passed');
