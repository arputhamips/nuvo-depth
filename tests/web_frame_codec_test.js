const assert=require('node:assert/strict');
const codec=require('../web/frame-codec.js');
function encoded(flags=1,overrides={},reserved=0){
  const metadata={schema_version:1,capture_epoch:1,capture_sequence:2,capture_time_ns:1000,source:'left_mono',codec:'jpeg',mime_type:'image/jpeg',width:728,height:544,keyframe:true,...overrides};
  const json=Buffer.from(JSON.stringify(metadata)),bytes=Buffer.alloc(32+json.length+1);
  bytes.write('NVE1');bytes.writeUInt16BE(1,4);bytes.writeUInt16BE(flags,6);bytes.writeUInt32BE(json.length,8);bytes.writeBigUInt64BE(1n,12);bytes.writeBigUInt64BE(1n,20);bytes.writeUInt32BE(reserved,28);json.copy(bytes,32);
  return bytes.buffer.slice(bytes.byteOffset,bytes.byteOffset+bytes.byteLength);
}
assert.equal(codec.parseEncoded(encoded()).metadata.codec,'jpeg');
for(const input of [encoded(0),encoded(3),encoded(1,{},1),encoded(1,{width:0}),encoded(1,{height:1081}),encoded(1,{codec:'other'}),encoded(1,{keyframe:'true'})])assert.throws(()=>codec.parseEncoded(input));
const invalidUtf8=encoded();new Uint8Array(invalidUtf8)[33]=255;assert.throws(()=>codec.parseEncoded(invalidUtf8));
for(const field of ['schema_version','capture_epoch','capture_sequence','capture_time_ns','source','codec','mime_type','width','height','keyframe'])assert.throws(()=>codec.parseEncoded(encoded(1,{[field]:undefined})));
let contexts=0,resizes=0,width=0,height=0;
const canvas={style:{},getContext:()=>{contexts++;return {}},get width(){return width},set width(value){width=value;resizes++},get height(){return height},set height(value){height=value;resizes++}};
codec.prepareCanvas(canvas,728,544);codec.prepareCanvas(canvas,728,544);assert.equal(contexts,1);assert.equal(resizes,2,'steady preview frames reuse the canvas backing store');
codec.prepareCanvas(canvas,640,480);assert.equal(resizes,4);assert.equal(contexts,1);
assert.equal(codec.parseFrame,undefined);assert.equal(codec.inspectDepth,undefined);
console.log('Browser compressed frame schema/bounds and reusable canvas checks passed');
