/* Shared bounded wire parsing and client-side product visualization. */
(function(root,factory){const codec=factory();if(typeof module==='object'&&module.exports)module.exports=codec;else root.NuvoFrameCodec=codec;})(globalThis,function(){
'use strict';
const PRODUCT_FORMATS={
  left_rectified:['gray8','bgr8','rgb8'],right_rectified:['gray8','bgr8','rgb8'],
  disparity:['float32_le'],depth:['float32_le'],confidence:['uint8'],validity:['uint8'],
  left_mono:['gray8'],right_mono:['gray8'],left_rgb:['rgb8'],right_rgb:['rgb8'],
  left_rectified_rgb:['rgb8'],right_rectified_rgb:['rgb8'],rectified_stereo:['gray8'],rectified_stereo_rgb:['rgb8']
};
function parseMetadata(buffer,offset,length){
  const metadata=JSON.parse(new TextDecoder('utf-8',{fatal:true}).decode(new Uint8Array(buffer,offset,length)));
  if(!metadata||Array.isArray(metadata)||metadata.schema_version!==1)throw new Error('Invalid frame metadata schema');
  return metadata;
}
function parseFrame(buffer) {
  const view = new DataView(buffer);
  if (buffer.byteLength < 32 || String.fromCharCode(...new Uint8Array(buffer,0,4)) !== 'NUV1'||
      view.getUint16(4,false)!==1||view.getUint16(6,false)!==0||view.getUint16(8,false)!==5||view.getUint16(10,false)!==0) throw new Error('Invalid Nuvo frame header');
  const metadataLength = view.getUint32(12,false), payloadLength = Number(view.getBigUint64(16,false));
  const sequence = Number(view.getBigUint64(24,false));
  if (32 + metadataLength + payloadLength !== buffer.byteLength || !metadataLength || metadataLength > 65536 || !payloadLength || payloadLength > 67108864||!Number.isSafeInteger(sequence)||sequence<=0) throw new Error('Frame bounds rejected');
  const metadata = parseMetadata(buffer,32,metadataLength);
  if(!Array.isArray(metadata.products)||!metadata.products.length||metadata.products.length>14)throw new Error('Missing or excessive products');
  for(const name of ['capture_epoch','capture_sequence','capture_time_ns','configuration_epoch','calibration_sha256','calibration_authority']){
    if(!(name in metadata))throw new Error('Incomplete frame identity');
  }
  for(const name of ['device_uuid','run_uuid'])if(name in metadata&&(typeof metadata[name]!=='string'||! /^[0-9a-f]{32}$/.test(metadata[name])))throw new Error('Invalid frame UUID');
  const names=new Set(),ranges=[];
  let mask=0;
  for(const p of metadata.products){
    if(!p||typeof p!=='object'||!Object.hasOwn(PRODUCT_FORMATS,p.kind)||!PRODUCT_FORMATS[p.kind].includes(p.format))throw new Error('Invalid product kind or format');
    const channels=['rgb8','bgr8'].includes(p.format)?3:['gray8','uint8'].includes(p.format)?1:p.format==='float32_le'?4:0;
    if(!channels||names.has(p.kind)||![p.width,p.height,p.offset,p.length].every(Number.isSafeInteger)||
      p.width<=0||p.height<=0||p.offset<0||p.length!==p.width*p.height*channels||p.offset+p.length>payloadLength)throw new Error('Invalid product bounds or format');
    names.add(p.kind);
    if(ranges.some(([start,end])=>p.offset<end&&p.offset+p.length>start))throw new Error('Product payload ranges overlap');
    ranges.push([p.offset,p.offset+p.length]);
    mask|=1<<Object.keys(PRODUCT_FORMATS).indexOf(p.kind);
  }
  for(const name of ['available_products','selected_products'])if(name in metadata){
    const value=metadata[name];
    if(!Number.isInteger(value)||value<=0||value>=16384||(name==='selected_products'?value!==mask:!!(mask&~value)))throw new Error('Product mask disagrees with descriptors');
  }
  return {metadata, sequence, payloadOffset:32+metadataLength, buffer};
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
const canvasStates = new WeakMap(), imageBuffers = new Map();
let renderedFrame = null, renderedProducts = new Map();
function prepareCanvas(canvas, width, height) {
  let state=canvasStates.get(canvas);
  if(!state){state={context:canvas.getContext('2d')};canvasStates.set(canvas,state);}
  if(canvas.width!==width)canvas.width=width;
  if(canvas.height!==height)canvas.height=height;
  canvas.style.aspectRatio=`${width} / ${height}`;
  return state;
}
function drawProduct(canvas, frame, descriptor, maximum=128) {
  if(!descriptor)return;
  const state=prepareCanvas(canvas,descriptor.width,descriptor.height);
  const key=`${descriptor.kind}:${descriptor.format}:${descriptor.width}:${descriptor.height}:${maximum}`;
  if(renderedFrame!==frame){renderedFrame=frame;renderedProducts.clear();}
  let rendered=renderedProducts.get(descriptor.kind);
  if(!rendered||rendered.key!==key){
    let buffer=imageBuffers.get(descriptor.kind);
    if(!buffer||buffer.width!==descriptor.width||buffer.height!==descriptor.height){
      buffer={image:state.context.createImageData(descriptor.width,descriptor.height),width:descriptor.width,height:descriptor.height};
      if(!imageBuffers.has(descriptor.kind)&&imageBuffers.size>=14)imageBuffers.delete(imageBuffers.keys().next().value);
      imageBuffers.set(descriptor.kind,buffer);
    }
    const image=buffer.image;
    const valid=fillProduct(image,frame,descriptor,maximum);
    rendered={image,valid,key};renderedProducts.set(descriptor.kind,rendered);
  }
  state.context.putImageData(rendered.image,0,0);
  return rendered.valid;
}
function fillProduct(image, frame, descriptor, maximum) {
  const numeric=descriptor.format==='float32_le';
  if(numeric){
    const values=new DataView(frame.buffer,frame.payloadOffset+descriptor.offset,descriptor.length);
    let valid=0;
    for(let i=0,j=0;i<descriptor.length;i+=4,j+=4){
      const value=values.getFloat32(i,true),accepted=Number.isFinite(value)&&(descriptor.kind!=='depth'||value>0);
      if(accepted){
        valid++;
        const t=Math.max(0,Math.min(1,descriptor.kind==='depth'?(value-.2)/12:value/maximum));
        image.data[j]=Math.round(255*Math.max(0,Math.min(1,1.5-Math.abs(4*t-3))));
        image.data[j+1]=Math.round(255*Math.max(0,Math.min(1,1.5-Math.abs(4*t-2))));
        image.data[j+2]=Math.round(255*Math.max(0,Math.min(1,1.5-Math.abs(4*t-1))));
      }else image.data[j]=image.data[j+1]=image.data[j+2]=0;
      image.data[j+3]=255;
    }
    return 100*valid/(descriptor.width*descriptor.height);
  }
  const source = new Uint8Array(frame.buffer,frame.payloadOffset+descriptor.offset,descriptor.length);
  const color=descriptor.format==='rgb8'||descriptor.format==='bgr8',bgr=descriptor.format==='bgr8';
  for(let pixel=0;pixel<descriptor.width*descriptor.height;pixel++){
    const i=pixel*(color?3:1),j=pixel*4;
    if(descriptor.kind==='validity'){const value=source[i]===1?255:0;image.data[j]=image.data[j+1]=image.data[j+2]=value;image.data[j+3]=255;continue;}
    image.data[j]=source[i+(color&&bgr?2:0)];image.data[j+1]=source[i+(color?1:0)];
    image.data[j+2]=source[i+(color?(bgr?0:2):0)];image.data[j+3]=255;
  }
}
function drawGray(canvas,frame,descriptor){return drawProduct(canvas,frame,descriptor);}
function drawDepth(canvas, frame, descriptor) {
  return drawProduct(canvas,frame,descriptor);
}
function drawDisparity(canvas, frame, descriptor, maximum=128) {
  return drawProduct(canvas,frame,descriptor,maximum);
}
return Object.freeze({parseFrame,parseEncoded,drawGray,drawDepth,drawDisparity,prepareCanvas});
});
