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
assert(source.includes("document.addEventListener('visibilitychange',restartStreams)"),'hidden tabs must release stream demand');
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
