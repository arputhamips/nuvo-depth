/* Shared bounded wire parsing and client-side product visualization. */
(function(root,factory){const codec=factory();if(typeof module==='object'&&module.exports)module.exports=codec;else root.NuvoFrameCodec=codec;})(globalThis,function(){
'use strict';
function parseFrame(buffer) {
  const view = new DataView(buffer);
  if (buffer.byteLength < 32 || String.fromCharCode(...new Uint8Array(buffer,0,4)) !== 'NUV1') throw new Error('Invalid Nuvo frame');
  const metadataLength = view.getUint32(12,false), payloadLength = Number(view.getBigUint64(16,false));
  const sequence = Number(view.getBigUint64(24,false));
  if (32 + metadataLength + payloadLength !== buffer.byteLength || metadataLength > 65536 || payloadLength > 67108864) throw new Error('Frame bounds rejected');
  const metadata = JSON.parse(new TextDecoder().decode(new Uint8Array(buffer,32,metadataLength)));
  if(!Array.isArray(metadata.products))throw new Error('Missing products');
  const names=new Set();
  for(const p of metadata.products){
    const channels=['rgb8','bgr8'].includes(p.format)?3:['gray8','uint8'].includes(p.format)?1:p.format==='float32_le'?4:0;
    if(!channels||names.has(p.kind)||![p.width,p.height,p.offset,p.length].every(Number.isSafeInteger)||
      p.width<=0||p.height<=0||p.offset<0||p.length!==p.width*p.height*channels||p.offset+p.length>payloadLength)throw new Error('Invalid product bounds or format');
    names.add(p.kind);
  }
  return {metadata, sequence, payloadOffset:32+metadataLength, buffer};
}
function parseEncoded(buffer) {
  const view=new DataView(buffer);
  if(buffer.byteLength<32||String.fromCharCode(...new Uint8Array(buffer,0,4))!=='NVE1'||view.getUint16(4,false)!==1)throw new Error('Invalid encoded frame');
  const metadataLength=view.getUint32(8,false),payloadLength=Number(view.getBigUint64(12,false));
  const streamSequence=Number(view.getBigUint64(20,false));
  if(metadataLength>65536||payloadLength>33554432||32+metadataLength+payloadLength!==buffer.byteLength)throw new Error('Encoded frame bounds rejected');
  const metadata=JSON.parse(new TextDecoder().decode(new Uint8Array(buffer,32,metadataLength)));
  return {metadata,streamSequence,payload:new Uint8Array(buffer,32+metadataLength,payloadLength)};
}
function drawGray(canvas, frame, descriptor) {
  if (!descriptor) return; canvas.width=descriptor.width; canvas.height=descriptor.height;
  canvas.style.aspectRatio=`${descriptor.width} / ${descriptor.height}`;
  const source = new Uint8Array(frame.buffer,frame.payloadOffset+descriptor.offset,descriptor.length);
  const image = canvas.getContext('2d').createImageData(descriptor.width,descriptor.height);
  const color=descriptor.format==='rgb8'||descriptor.format==='bgr8',bgr=descriptor.format==='bgr8';
  for(let pixel=0;pixel<descriptor.width*descriptor.height;pixel++){
    const i=pixel*(color?3:1),j=pixel*4;
    image.data[j]=source[i+(color&&bgr?2:0)];image.data[j+1]=source[i+(color?1:0)];
    image.data[j+2]=source[i+(color?(bgr?0:2):0)];image.data[j+3]=255;
  }
  canvas.getContext('2d').putImageData(image,0,0);
}
function drawDepth(canvas, frame, descriptor) {
  if (!descriptor) return; canvas.width=descriptor.width; canvas.height=descriptor.height;
  canvas.style.aspectRatio=`${descriptor.width} / ${descriptor.height}`;
  const values = new DataView(frame.buffer,frame.payloadOffset+descriptor.offset,descriptor.length);
  const image = canvas.getContext('2d').createImageData(descriptor.width,descriptor.height); let valid=0;
  for(let i=0,j=0;i<descriptor.length;i+=4,j+=4){const z=values.getFloat32(i,true);if(Number.isFinite(z)&&z>0){valid++;const t=Math.max(0,Math.min(1,(z-.2)/12));image.data[j]=Math.round(255*Math.max(0,1.5-Math.abs(4*t-3)));image.data[j+1]=Math.round(255*Math.max(0,1.5-Math.abs(4*t-2)));image.data[j+2]=Math.round(255*Math.max(0,1.5-Math.abs(4*t-1)));}image.data[j+3]=255}
  canvas.getContext('2d').putImageData(image,0,0); return 100*valid/(descriptor.width*descriptor.height);
}
function turbo(t) { return [Math.round(255*Math.max(0,Math.min(1,1.5-Math.abs(4*t-3)))),Math.round(255*Math.max(0,Math.min(1,1.5-Math.abs(4*t-2)))),Math.round(255*Math.max(0,Math.min(1,1.5-Math.abs(4*t-1))))]; }
function drawDisparity(canvas, frame, descriptor, maximum=128) {
  if (!descriptor) return; canvas.width=descriptor.width; canvas.height=descriptor.height;
  canvas.style.aspectRatio=`${descriptor.width} / ${descriptor.height}`;
  const values=new DataView(frame.buffer,frame.payloadOffset+descriptor.offset,descriptor.length);
  const image=canvas.getContext('2d').createImageData(descriptor.width,descriptor.height);
  const scale=maximum;
  for(let i=0,j=0;i<descriptor.length;i+=4,j+=4){const d=values.getFloat32(i,true);if(Number.isFinite(d)){const [r,g,b]=turbo(Math.max(0,Math.min(1,d/scale)));image.data[j]=r;image.data[j+1]=g;image.data[j+2]=b;}image.data[j+3]=255;}
  canvas.getContext('2d').putImageData(image,0,0);
}
return Object.freeze({parseFrame,parseEncoded,drawGray,drawDepth,drawDisparity});
});
