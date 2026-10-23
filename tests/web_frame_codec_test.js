const assert=require('node:assert/strict');
const codec=require('../web/frame-codec.js');
function wire(overrides={},header={}){
  const metadata={schema_version:1,capture_epoch:1,capture_sequence:1,capture_time_ns:1000000000,configuration_epoch:1,
    calibration_sha256:'a'.repeat(64),calibration_authority:0,selected_products:12,available_products:63,
    products:[{kind:'depth',format:'float32_le',width:2,height:1,offset:0,length:8},{kind:'disparity',format:'float32_le',width:2,height:1,offset:8,length:8}],...overrides};
  const json=Buffer.from(JSON.stringify(metadata)),bytes=Buffer.alloc(32+json.length+16);
  bytes.write('NUV1');bytes.writeUInt16BE(header.major??1,4);bytes.writeUInt16BE(header.minor??0,6);
  bytes.writeUInt16BE(header.kind??5,8);bytes.writeUInt16BE(header.flags??0,10);bytes.writeUInt32BE(json.length,12);
  bytes.writeBigUInt64BE(16n,16);bytes.writeBigUInt64BE(header.sequence??1n,24);json.copy(bytes,32);
  return bytes.buffer.slice(bytes.byteOffset,bytes.byteOffset+bytes.byteLength);
}
assert.equal(codec.parseFrame(wire()).metadata.products.length,2);
for(const header of [{major:2},{minor:1},{kind:4},{flags:1},{sequence:0n}])assert.throws(()=>codec.parseFrame(wire({},header)));
for(const metadata of [{schema_version:2},{run_uuid:'bad'},{selected_products:4},{available_products:8},
  {products:[]},{products:[{kind:'unknown',format:'gray8',width:16,height:1,offset:0,length:16}]},
  {products:[{kind:'depth',format:'gray8',width:16,height:1,offset:0,length:16}]},
  {products:[{kind:'depth',format:'float32_le',width:2,height:1,offset:0,length:8},{kind:'disparity',format:'float32_le',width:2,height:1,offset:4,length:8}]}])assert.throws(()=>codec.parseFrame(wire(metadata)));
const utf8=wire(),bytes=new Uint8Array(utf8);bytes[33]=255;assert.throws(()=>codec.parseFrame(utf8));
function encoded(flags=1,overrides={},reserved=0){
  const metadata={schema_version:1,capture_epoch:1,capture_sequence:2,capture_time_ns:1000,source:'left_mono',codec:'jpeg',mime_type:'image/jpeg',width:728,height:544,keyframe:true,...overrides};
  const json=Buffer.from(JSON.stringify(metadata)),bytes=Buffer.alloc(32+json.length+1);
  bytes.write('NVE1');bytes.writeUInt16BE(1,4);bytes.writeUInt16BE(flags,6);bytes.writeUInt32BE(json.length,8);bytes.writeBigUInt64BE(1n,12);bytes.writeBigUInt64BE(1n,20);bytes.writeUInt32BE(reserved,28);json.copy(bytes,32);
  return bytes.buffer.slice(bytes.byteOffset,bytes.byteOffset+bytes.byteLength);
}
assert.equal(codec.parseEncoded(encoded()).metadata.codec,'jpeg');
for(const input of [encoded(0),encoded(3),encoded(1,{},1),encoded(1,{width:0}),encoded(1,{height:1081}),encoded(1,{codec:'other'}),encoded(1,{keyframe:'true'})])assert.throws(()=>codec.parseEncoded(input));
console.log('Browser frame headers, schemas, product masks, overlaps and encoded bounds passed');
