/* First-use checks operate on received raw products, never configured targets. */
(function(root,factory){const api=factory();if(typeof module==='object'&&module.exports)module.exports=api;else root.NuvoFirstUse=api;})(globalThis,function(){
'use strict';
function calibration(metadata){
  const accepted=[1,2].includes(metadata?.calibration_authority)&&/^[a-f0-9]{64}$/.test(metadata?.calibration_sha256||'');
  return accepted?{label:'Accepted calibration',detail:'The active bundle passed its declared validation authority.'}:
    {label:'Preview · accuracy unverified',detail:'Depth values are engineering output; this preview does not establish metric accuracy.'};
}
function observe(state,frame,now){
  const m=frame.metadata,depth=m.products.find(p=>p.kind==='depth'),validity=m.products.find(p=>p.kind==='validity');
  if(!depth||!validity||depth.format!=='float32_le'||validity.format!=='uint8'||depth.width!==validity.width||depth.height!==validity.height)return;
  const values=new DataView(frame.buffer,frame.payloadOffset+depth.offset,depth.length);
  const mask=new Uint8Array(frame.buffer,frame.payloadOffset+validity.offset,validity.length);
  let validPixels=0;
  for(let index=0;index<mask.length;index++){const z=values.getFloat32(index*4,true);if(mask[index]===1&&Number.isFinite(z)&&z>0)validPixels++;}
  if(!validPixels){state.count=0;state.error='No valid depth pixels. Inspect scene texture, lighting, exposure and calibration alignment.';return;}
  state.error='';state.validPixels=validPixels;
  if(!/^[a-f0-9]{32}$/.test(m.device_uuid||'')||!/^[a-f0-9]{32}$/.test(m.run_uuid||'')||!Number.isSafeInteger(m.capture_sequence)||m.capture_sequence<=0||!Number.isSafeInteger(m.capture_epoch))return;
  const run=`${m.device_uuid}:${m.run_uuid}:${m.capture_epoch}`;
  if(run!==state.run){state.run=run;state.count=0;state.sequence=0;state.captureTime=-1;}
  if(m.capture_sequence<=state.sequence||!(m.capture_time_ns>state.captureTime))return;
  state.sequence=m.capture_sequence;state.captureTime=m.capture_time_ns;state.count++;state.at=now;state.frame=frame;
}
function ready(state,now){return state.count>=2&&now-state.at>=0&&now-state.at<=5000;}
return Object.freeze({calibration,observe,ready});
});
