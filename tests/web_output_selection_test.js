const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(require('node:path').join(__dirname, '../web/app.js'),'utf8');
const context = {TextDecoder, Uint8Array, DataView, Number, Set};
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
console.log('Web output selection, bandwidth, RGB/BGR, and demand lifecycle tests passed');
