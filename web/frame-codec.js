/* Bounded compressed-preview parsing and reusable canvas context. */
(function(root,factory){const codec=factory();if(typeof module==='object'&&module.exports)module.exports=codec;else root.NuvoFrameCodec=codec;})(globalThis,function(){
'use strict';
function parseMetadata(buffer,offset,length){
  const metadata=JSON.parse(new TextDecoder('utf-8',{fatal:true}).decode(new Uint8Array(buffer,offset,length)));
  if(!metadata||Array.isArray(metadata)||metadata.schema_version!==1)throw new Error('Invalid frame metadata schema');
  return metadata;
}
function parseEncoded(buffer) {
  const view=new DataView(buffer);
  if(buffer.byteLength<32||String.fromCharCode(...new Uint8Array(buffer,0,4))!=='NVE1'||view.getUint16(4,false)!==1||view.getUint16(6,false)&~1||view.getUint32(28,false)!==0)throw new Error('Invalid encoded frame');
  const metadataLength=view.getUint32(8,false),payloadLength=Number(view.getBigUint64(12,false));
  const streamSequence=Number(view.getBigUint64(20,false));
  if(!metadataLength||metadataLength>65536||!payloadLength||payloadLength>33554432||!Number.isSafeInteger(streamSequence)||streamSequence<=0||32+metadataLength+payloadLength!==buffer.byteLength)throw new Error('Encoded frame bounds rejected');
  const metadata=parseMetadata(buffer,32,metadataLength);
  for(const name of ['capture_epoch','capture_sequence','capture_time_ns','source','codec','mime_type','width','height','keyframe'])if(!(name in metadata))throw new Error('Incomplete encoded metadata');
  if(typeof metadata.keyframe!=='boolean'||!!(view.getUint16(6,false)&1)!==metadata.keyframe)throw new Error('Encoded keyframe metadata disagrees with header');
  if(!['jpeg','h264'].includes(metadata.codec)||!Number.isSafeInteger(metadata.width)||!Number.isSafeInteger(metadata.height)||metadata.width<=0||metadata.height<=0||metadata.width>1920||metadata.height>1080)throw new Error('Invalid encoded dimensions or codec');
  return {metadata,streamSequence,payload:new Uint8Array(buffer,32+metadataLength,payloadLength)};
}
const canvasStates = new WeakMap();
function prepareCanvas(canvas, width, height) {
  let state=canvasStates.get(canvas);
  if(!state){state={context:canvas.getContext('2d')};canvasStates.set(canvas,state);}
  if(canvas.width!==width)canvas.width=width;
  if(canvas.height!==height)canvas.height=height;
  canvas.style.aspectRatio=`${width} / ${height}`;
  return state;
}
return Object.freeze({parseEncoded,prepareCanvas});
});
