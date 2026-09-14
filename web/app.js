const $ = id => document.getElementById(id);
const dialog = $('claimDialog');
let lastSequence = 0, lastIdentity = '', lastFrameAt = 0;
let lastHealthRun = '', lastHealthPublished = null, lastHealthAt = 0;
let rawPolling = false, rawSocket = null, encoding = null;
let encodedPolling = false, encodedSocket = null, encodedFrameAt = 0;
let decoder = null, decoderCodec = '', encodedLastSequence = 0;
let encodedWaitingForKeyframe = true;
let encodedGeneration = 0, encodedArrival = 0, encodedLastPresented = 0;
let encodingFormDirty = false, encodingPreviewPending = false;
let stereo = null, algorithmFormDirty = false, activeStereoBackend = '';
let csrf = '';
let selectedPage = '';

function openPage(id) {
  if (!document.getElementById(id)?.classList.contains('page')) id = 'overview';
  if (id === selectedPage) return;
  selectedPage = id;
  document.querySelectorAll('.page').forEach(x => x.classList.toggle('active', x.id === id));
  document.querySelectorAll('nav button').forEach(x => {
    const active = x.dataset.page === id;
    x.classList.toggle('active', active);
    if (active) x.setAttribute('aria-current', 'page');
    else x.removeAttribute('aria-current');
  });
  restartStreams();
}
function navigateToPage(id){
  if(location.hash!==`#${id}`)history.pushState(null,'',`#${id}`);
  openPage(id);
}
document.querySelectorAll('[data-page]').forEach(x => x.onclick = () => navigateToPage(x.dataset.page));
document.querySelectorAll('[data-open]').forEach(x => x.onclick = () => navigateToPage(x.dataset.open));
window.addEventListener('hashchange', () => openPage(location.hash.slice(1)));
window.addEventListener('popstate', () => openPage(location.hash.slice(1)));
openPage(location.hash.slice(1));
$('originLabel').textContent = location.origin;
$('encodedEndpoint').textContent = `${location.origin}/api/v1/encoded/latest · WebSocket /api/v1/encoded/stream`;

async function api(path, options = {}) {
  const method = options.method || 'GET';
  const headers = new Headers(options.headers || {});
  if (!['GET','HEAD'].includes(method) && path !== '/api/v1/session' && csrf) headers.set('X-Nuvo-CSRF',csrf);
  const response = await fetch(path, {...options, method, headers, credentials:'same-origin', cache:'no-store'});
  if (response.status === 401) { if (!dialog.open) dialog.showModal(); throw new Error('Authentication required'); }
  if (!response.ok) {
    let detail = `${response.status} ${response.statusText}`;
    try { const value = await response.json(); detail = value.error || detail; } catch (_) {}
    throw new Error(detail);
  }
  return response;
}

$('claimForm').addEventListener('submit', async event => {
  event.preventDefault(); $('claimError').textContent = '';
  const token = $('claimToken').value;
  try {
    const session = await (await api('/api/v1/session', {method:'POST', headers:{Authorization:`Bearer ${token}`}})).json();
    csrf = session.csrf; $('claimToken').value = ''; dialog.close();
    await Promise.all([refreshHealth(),refreshTransport(),refreshEncoding(),refreshStereo()]); restartStreams();
  } catch (error) { $('claimError').textContent = error.message; }
});

async function refreshTransport() {
  try {
    const state = await (await api('/api/v1/transport')).json();
    $('transportState').textContent = `Current preference: ${state.preference}`;
    $('transportError').textContent = '';
  } catch (error) { $('transportState').textContent = 'Connection preference unavailable'; $('transportError').textContent = error.message; }
}

async function transportAction(path, body) {
  $('transportError').textContent = '';
  try {
    await api(path,{method:'POST',headers:{'Content-Type':'application/json'},body:body ? JSON.stringify(body) : ''});
    await refreshTransport();
  } catch (error) { $('transportError').textContent = error.message; }
}
document.querySelectorAll('[data-transport]').forEach(button => {
  button.onclick = () => transportAction('/api/v1/transport/stage',{preference:button.dataset.transport});
});
$('confirmTransport').onclick = () => transportAction('/api/v1/transport/confirm');
$('rollbackTransport').onclick = () => transportAction('/api/v1/transport/rollback');

function showEncodingConfig(value) {
  $('streamMode').value=value.mode; $('streamCodec').value=value.codec;
  $('streamSource').value=value.source; $('streamWidth').value=value.width;
  $('streamHeight').value=value.height; $('streamFps').value=value.cadence_hz;
  $('streamBitrate').value=value.bitrate_bps; $('streamQuality').value=value.jpeg_quality;
  $('streamKeyframes').value=value.keyframe_interval; $('streamThreads').value=value.threads;
  $('streamLowLatency').checked=value.low_latency; $('streamPreset').value=value.preset;
  $('encodedFormat').textContent=value.mode==='raw' ? 'Disabled' : `${value.codec.toUpperCase()} · ${value.source.replaceAll('_',' ')}`;
  $('viewerModeNotice').hidden=value.mode!=='encoded';
  updateEncodingFields();
}

function updateEncodingFields(){
  const h264=$('streamCodec').value==='h264';
  for(const id of ['streamBitrate','streamKeyframes','streamThreads','streamPreset','streamLowLatency']){
    $(id).disabled=!h264;$(id).closest('label').classList.toggle('inactive',!h264);
  }
  $('streamQuality').disabled=h264;$('streamQuality').closest('label').classList.toggle('inactive',h264);
}
$('streamCodec').addEventListener('change',updateEncodingFields);

function encodedFrameMatchesConfiguration(metadata) {
  return encoding && encoding.mode !== 'raw' && metadata.codec === encoding.codec &&
    metadata.source === encoding.source &&
    (!encoding.width || metadata.width === encoding.width) &&
    (!encoding.height || metadata.height === encoding.height);
}

$('encodingForm').addEventListener('input', () => {
  encodingFormDirty=true;
  $('encodingSaveState').textContent='Not applied';
});

async function refreshEncoding() {
  try {
    const updated=await (await api('/api/v1/encoding')).json();
    const changed=encoding&&JSON.stringify(updated)!==JSON.stringify(encoding);
    encoding=updated;
    if(!encodingFormDirty)showEncodingConfig(encoding);
    else if(changed)$('encodingSaveState').textContent='Device changed · review and apply';
    if(changed){encodingPreviewPending=encoding.mode!=='raw';restartStreams();}
    $('encodingError').textContent='';
  } catch (error) { $('encodingError').textContent=error.message; }
}

function requestedEncoding() {
  return {schema_version:1,mode:$('streamMode').value,codec:$('streamCodec').value,
    source:$('streamSource').value,width:Number($('streamWidth').value),height:Number($('streamHeight').value),
    cadence_hz:Number($('streamFps').value),bitrate_bps:Number($('streamBitrate').value),
    jpeg_quality:Number($('streamQuality').value),keyframe_interval:Number($('streamKeyframes').value),
    threads:Number($('streamThreads').value),low_latency:$('streamLowLatency').checked,preset:$('streamPreset').value};
}

$('encodingForm').addEventListener('submit', async event => {
  event.preventDefault(); $('encodingSaveState').textContent='Applying…'; $('encodingError').textContent='';
  try {
    const requested=requestedEncoding();
    if ((requested.width===0)!==(requested.height===0)) throw new Error('Set both width and height, or leave both at 0 for native resolution');
    encoding=await (await api('/api/v1/encoding',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(requested)})).json();
    encodingFormDirty=false; encodingPreviewPending=encoding.mode!=='raw';
    showEncodingConfig(encoding); restartStreams();
    $('encodingSaveState').textContent=encodingPreviewPending?'Applied · preview syncing…':'Applied';
  } catch (error) { $('encodingSaveState').textContent=''; $('encodingError').textContent=error.message; }
});

function showStereoConfig(value) {
  const fields={algorithm:'algorithmType',min_disparity:'minDisparity',
    search_range:'searchRange',max_disparity:'maxDisparity',block_size:'blockSize',
    uniqueness_ratio:'uniquenessRatio',speckle_window_size:'speckleWindow',
    speckle_range:'speckleRange',disparity_12_max_diff:'disparity12Diff',
    pre_filter_cap:'preFilterCap',bm_pre_filter_size:'bmPreFilterSize',
    bm_texture_threshold:'bmTextureThreshold',specular_threshold:'specularThreshold',
    opencv_threads:'algorithmThreads'};
  for(const [key,id] of Object.entries(fields))$(id).value=value[key];
  updateAlgorithmFields();
}
function updateAlgorithmFields(){
  const bm=$('algorithmType').value==='bm';
  document.querySelectorAll('.bm-only').forEach(label=>{
    label.classList.toggle('inactive',!bm);
    label.querySelector('input').disabled=!bm;
  });
}
function requestedStereo(){
  return {schema_version:1,algorithm:$('algorithmType').value,
    min_disparity:Number($('minDisparity').value),search_range:Number($('searchRange').value),
    max_disparity:Number($('maxDisparity').value),block_size:Number($('blockSize').value),
    uniqueness_ratio:Number($('uniquenessRatio').value),
    speckle_window_size:Number($('speckleWindow').value),speckle_range:Number($('speckleRange').value),
    disparity_12_max_diff:Number($('disparity12Diff').value),pre_filter_cap:Number($('preFilterCap').value),
    bm_pre_filter_size:Number($('bmPreFilterSize').value),
    bm_texture_threshold:Number($('bmTextureThreshold').value),
    specular_threshold:Number($('specularThreshold').value),
    opencv_threads:Number($('algorithmThreads').value)};
}
async function refreshStereo(){
  try{
    const updated=await (await api('/api/v1/stereo')).json();
    const changed=stereo&&JSON.stringify(updated)!==JSON.stringify(stereo);
    stereo=updated;
    if(!algorithmFormDirty)showStereoConfig(stereo);
    else if(changed)$('algorithmSaveState').textContent='Device changed · review and apply';
    $('algorithmError').textContent='';
  }catch(error){$('algorithmError').textContent=error.message;}
}
$('algorithmType').addEventListener('change',updateAlgorithmFields);
$('algorithmForm').addEventListener('input',()=>{
  algorithmFormDirty=true;$('algorithmSaveState').textContent='Not applied';
});
$('algorithmForm').addEventListener('submit',async event=>{
  event.preventDefault();$('algorithmSaveState').textContent='Applying…';$('algorithmError').textContent='';
  try{
    const requested=requestedStereo();
    if(requested.search_range%16!==0||
       requested.min_disparity>=requested.max_disparity||
       requested.max_disparity>requested.min_disparity+requested.search_range){
      throw new Error('Maximum disparity must lie inside the 16-pixel-step search range');
    }
    stereo=await (await api('/api/v1/stereo',{method:'POST',
      headers:{'Content-Type':'application/json'},body:JSON.stringify(requested)})).json();
    algorithmFormDirty=false;showStereoConfig(stereo);
    $('algorithmSaveState').textContent='Saved · applies on next depth frame';
  }catch(error){$('algorithmSaveState').textContent='';$('algorithmError').textContent=error.message;}
});

async function refreshHealth() {
  try {
    const health = await (await api('/api/v1/health')).json();
    const now=performance.now();
    if (health.run_uuid===lastHealthRun && lastHealthPublished!==null && health.published_frames>=lastHealthPublished && now>lastHealthAt) {
      const fps=1000*(health.published_frames-lastHealthPublished)/(now-lastHealthAt);
      $('frameRate').textContent=`${fps.toFixed(1)} fps`;
    }
    lastHealthRun=health.run_uuid||'';lastHealthPublished=health.published_frames;lastHealthAt=now;
    $('connection').textContent = 'Securely connected'; $('connection').className = 'pill online';
    $('cameraState').textContent = health.status === 'running' ? 'Streaming' : health.status;
    $('cameraDetail').textContent = `Capture ${health.capture_epoch ?? '—'} · frame ${health.capture_sequence ?? '—'}`;
    $('frameDetail').textContent=`${health.depth_width??'—'} × ${health.depth_height??'—'} native depth grid`;
    $('processingTime').textContent = health.processing_ms == null ? '—' : `${health.processing_ms.toFixed(1)} ms`;
    const total=Number(health.processing_ms)||0;
    const stages=[['rectification',health.rectification_ms],['preprocess',health.stereo_preprocess_ms],['match',health.stereo_match_ms],['postprocess',health.stereo_postprocess_ms]];
    const accounted=stages.reduce((sum,[,value])=>sum+(Number(value)||0),0);
    stages.push(['other',Math.max(0,total-accounted)]);
    for(const [id,value] of stages){
      const timing=Number(value);
      $(id+'Time').textContent=Number.isFinite(timing)?`${timing.toFixed(1)} ms`:'—';
      $(id+'Bar').style.width=total>0&&Number.isFinite(timing)?`${Math.min(100,100*timing/total)}%`:'0%';
    }
    activeStereoBackend=health.stereo_backend||'';
    $('algorithmActive').textContent=`Active matcher: ${activeStereoBackend||'waiting for depth frame'}`;
    const provisional = health.calibration_authority === 0;
    $('calibrationState').textContent = provisional ? 'Engineering only' : 'Accepted';
    $('calibrationHash').textContent = health.calibration_sha256 ? health.calibration_sha256.slice(0,16)+'…' : 'Not loaded';
    $('calibrationNotice').textContent = provisional ? 'This unit is using a provisional calibration. Metric depth is visibly marked engineering-only until per-unit validation succeeds.' : 'The active calibration passed its declared validation authority.';
    $('diagnosticJson').textContent = JSON.stringify(health,null,2);
  } catch (_) { $('connection').textContent = 'Not connected'; $('connection').className = 'pill offline'; $('frameRate').textContent='—'; lastHealthPublished=null; }
}

function parseFrame(buffer) {
  const view = new DataView(buffer);
  if (buffer.byteLength < 32 || String.fromCharCode(...new Uint8Array(buffer,0,4)) !== 'NUV1') throw new Error('Invalid Nuvo frame');
  const metadataLength = view.getUint32(12,false), payloadLength = Number(view.getBigUint64(16,false));
  const sequence = Number(view.getBigUint64(24,false));
  if (32 + metadataLength + payloadLength !== buffer.byteLength || metadataLength > 65536 || payloadLength > 67108864) throw new Error('Frame bounds rejected');
  const metadata = JSON.parse(new TextDecoder().decode(new Uint8Array(buffer,32,metadataLength)));
  return {metadata, sequence, payloadOffset:32+metadataLength, buffer};
}
function product(frame, name) { return frame.metadata.products.find(x => x.kind === name); }
function drawGray(canvas, frame, descriptor) {
  if (!descriptor) return; canvas.width=descriptor.width; canvas.height=descriptor.height;
  const source = new Uint8Array(frame.buffer,frame.payloadOffset+descriptor.offset,descriptor.length);
  const image = canvas.getContext('2d').createImageData(descriptor.width,descriptor.height);
  for(let i=0,j=0;i<source.length;i++,j+=4){const v=source[i];image.data[j]=v;image.data[j+1]=v;image.data[j+2]=v;image.data[j+3]=255}
  canvas.getContext('2d').putImageData(image,0,0);
}
function drawDepth(canvas, frame, descriptor) {
  if (!descriptor) return; canvas.width=descriptor.width; canvas.height=descriptor.height;
  const values = new DataView(frame.buffer,frame.payloadOffset+descriptor.offset,descriptor.length);
  const image = canvas.getContext('2d').createImageData(descriptor.width,descriptor.height); let valid=0;
  for(let i=0,j=0;i<descriptor.length;i+=4,j+=4){const z=values.getFloat32(i,true);if(Number.isFinite(z)&&z>0){valid++;const t=Math.max(0,Math.min(1,(z-.2)/12));image.data[j]=Math.round(255*Math.max(0,1.5-Math.abs(4*t-3)));image.data[j+1]=Math.round(255*Math.max(0,1.5-Math.abs(4*t-2)));image.data[j+2]=Math.round(255*Math.max(0,1.5-Math.abs(4*t-1)));}image.data[j+3]=255}
  canvas.getContext('2d').putImageData(image,0,0); $('validity').textContent=`Valid ${(100*valid/(descriptor.width*descriptor.height)).toFixed(1)}%`;
}
function turbo(t) { return [Math.round(255*Math.max(0,Math.min(1,1.5-Math.abs(4*t-3)))),Math.round(255*Math.max(0,Math.min(1,1.5-Math.abs(4*t-2)))),Math.round(255*Math.max(0,Math.min(1,1.5-Math.abs(4*t-1))))]; }
function drawDisparity(canvas, frame, descriptor) {
  if (!descriptor) return; canvas.width=descriptor.width; canvas.height=descriptor.height;
  const values=new DataView(frame.buffer,frame.payloadOffset+descriptor.offset,descriptor.length);
  const image=canvas.getContext('2d').createImageData(descriptor.width,descriptor.height);
  const scale=stereo?.max_disparity||128;
  for(let i=0,j=0;i<descriptor.length;i+=4,j+=4){const d=values.getFloat32(i,true);if(Number.isFinite(d)){const [r,g,b]=turbo(Math.max(0,Math.min(1,d/scale)));image.data[j]=r;image.data[j+1]=g;image.data[j+2]=b;}image.data[j+3]=255;}
  canvas.getContext('2d').putImageData(image,0,0);
}
function renderFrame(frame) {
  const identity=`${frame.metadata.run_uuid}:${frame.metadata.capture_epoch}:${frame.metadata.capture_sequence}`;
  if(identity===lastIdentity)return;
  drawGray($('leftCanvas'),frame,product(frame,'left_rectified')); drawGray($('rightCanvas'),frame,product(frame,'right_rectified'));
  drawDisparity($('disparityCanvas'),frame,product(frame,'disparity')); drawDepth($('depthCanvas'),frame,product(frame,'depth'));
  lastFrameAt=Date.now();lastSequence=frame.sequence;lastIdentity=identity;
  $('sequence').textContent=`Frame ${frame.metadata.capture_sequence}`; $('age').textContent='Updated now';$('frameError').textContent='';
}
async function pollFrame() {
  if (!rawPolling) return;
  try { renderFrame(parseFrame(await (await api('/api/v1/frame/latest')).arrayBuffer())); }
  catch (error) { $('frameError').textContent=error.message; }
  setTimeout(pollFrame,200);
}
function socketUrl(path) { return `${location.protocol==='https:'?'wss':'ws'}://${location.host}${path}`; }
function startRawViewer(){
  if(rawSocket||rawPolling||!$('viewer').classList.contains('active')||!encoding||encoding.mode==='encoded')return;
  rawSocket=new WebSocket(socketUrl('/api/v1/stream'));rawSocket.binaryType='arraybuffer';
  rawSocket.onmessage=event=>{try{renderFrame(parseFrame(event.data))}catch(_){rawSocket.close()}};
  rawSocket.onclose=()=>{rawSocket=null;if(encoding&&encoding.mode!=='encoded'&&!rawPolling){rawPolling=true;pollFrame()}};
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
function resetEncodedDecoder() {
  if(decoder){try{decoder.close()}catch(_){}}
  decoder=null;decoderCodec='';encodedLastSequence=0;encodedWaitingForKeyframe=true;
}
function ensureDecoder(codec,width,height,generation) {
  if(!('VideoDecoder' in window))throw new Error('This browser cannot preview H.264; the network stream remains available');
  const key=`${codec}:${width}x${height}`;
  if(decoder&&decoderCodec===key)return decoder;
  if(decoder){try{decoder.close()}catch(_){}}
  decoder=null;decoderCodec=key;encodedWaitingForKeyframe=true;
  decoder=new VideoDecoder({output:video=>{if(generation!==encodedGeneration){video.close();return}const canvas=$('encodedCanvas');canvas.width=video.displayWidth;canvas.height=video.displayHeight;canvas.getContext('2d').drawImage(video,0,0);video.close();},error:error=>{encodedWaitingForKeyframe=true;$('encodingError').textContent=`Decoder resynchronizing: ${error.message}`}});
  decoder.configure({codec,codedWidth:width,codedHeight:height,optimizeForLatency:true,hardwareAcceleration:'prefer-hardware'});
  return decoder;
}
async function renderEncoded(frame,generation,arrival) {
  const {metadata,payload}=frame, canvas=$('encodedCanvas');
  if(generation!==encodedGeneration||!encodedFrameMatchesConfiguration(metadata))return;
  if(metadata.codec==='jpeg'){
    const bitmap=await createImageBitmap(new Blob([payload],{type:'image/jpeg'}));
    if(generation!==encodedGeneration||!encodedFrameMatchesConfiguration(metadata)||arrival<encodedLastPresented){bitmap.close();return}
    encodedLastPresented=arrival;canvas.width=bitmap.width;canvas.height=bitmap.height;canvas.getContext('2d').drawImage(bitmap,0,0);bitmap.close();
  }else{
    if(encodedLastSequence&&frame.streamSequence!==encodedLastSequence+1)resetEncodedDecoder();
    encodedLastSequence=frame.streamSequence;
    const activeDecoder=ensureDecoder(metadata.codec_string,metadata.width,metadata.height,generation);
    if(encodedWaitingForKeyframe&&!metadata.keyframe){
      $('encodingError').textContent='Stream resynchronizing at the next keyframe';
      return;
    }
    try{
      activeDecoder.decode(new EncodedVideoChunk({type:metadata.keyframe?'key':'delta',timestamp:Math.max(0,Math.floor(metadata.capture_time_ns/1000)),data:payload}));
      encodedWaitingForKeyframe=false;
    }catch(error){resetEncodedDecoder();throw error}
  }
  encodedFrameAt=Date.now();$('encodedSequence').textContent=`Frame ${metadata.capture_sequence}`;
  $('encodedSize').textContent=`${(payload.byteLength/1024).toFixed(1)} KiB`;
  $('encodedLatency').textContent=`Encoded ${metadata.encode_ms.toFixed(1)} ms`;
  $('encodedFormat').textContent=`${metadata.codec.toUpperCase()} · ${metadata.source.replaceAll('_',' ')} · ${metadata.width} × ${metadata.height}`;
  $('encodedAge').textContent='Updated now';$('encodingError').textContent='';
  if(encodingPreviewPending){encodingPreviewPending=false;if(!encodingFormDirty)$('encodingSaveState').textContent='Applied';}
}
async function pollEncoded(generation) {
  if(!encodedPolling||generation!==encodedGeneration)return;
  try{await renderEncoded(parseEncoded(await (await api('/api/v1/encoded/latest')).arrayBuffer()),generation,++encodedArrival);}
  catch(error){$('encodingError').textContent=error.message;}
  setTimeout(()=>pollEncoded(generation),200);
}
function startEncodedViewer(){
  if(encodedSocket||encodedPolling||!$('streaming').classList.contains('active')||!encoding||encoding.mode==='raw')return;
  const generation=encodedGeneration, socket=new WebSocket(socketUrl('/api/v1/encoded/stream'));
  encodedSocket=socket;socket.binaryType='arraybuffer';
  socket.onmessage=event=>{if(generation!==encodedGeneration||socket!==encodedSocket)return;try{renderEncoded(parseEncoded(event.data),generation,++encodedArrival).catch(error=>{$('encodingError').textContent=error.message})}catch(error){$('encodingError').textContent=error.message}};
  socket.onclose=()=>{if(generation!==encodedGeneration||socket!==encodedSocket)return;encodedSocket=null;if(encoding&&encoding.mode!=='raw'&&!encodedPolling){encodedPolling=true;pollEncoded(generation)}};
}
function restartStreams(){
  ++encodedGeneration;encodedArrival=0;encodedLastPresented=0;
  rawPolling=false;encodedPolling=false;
  if(rawSocket){const value=rawSocket;rawSocket=null;value.onclose=null;value.close()}
  if(encodedSocket){const value=encodedSocket;encodedSocket=null;value.onmessage=null;value.onclose=null;value.onerror=null;value.close()}
  resetEncodedDecoder();
  for(const id of ['leftCanvas','rightCanvas','disparityCanvas','depthCanvas','encodedCanvas']){
    const canvas=$(id);canvas.width=728;canvas.height=544;
  }
  $('sequence').textContent='Frame —';$('validity').textContent='Valid —';$('age').textContent='Waiting for products';lastFrameAt=0;lastIdentity='';
  $('encodedSequence').textContent='Frame —';$('encodedSize').textContent='—';$('encodedLatency').textContent='—';$('encodedAge').textContent='Waiting';encodedFrameAt=0;
  startRawViewer();startEncodedViewer();
}

$('copyDiagnostics').addEventListener('click',async()=>{
  try{await navigator.clipboard.writeText($('diagnosticJson').textContent);$('copyDiagnosticsState').textContent='Copied';}
  catch(error){$('copyDiagnosticsState').textContent=`Copy unavailable: ${error.message}`;}
});

async function restoreSession() {
  try {
    const session = await (await api('/api/v1/session')).json(); csrf = session.csrf;
    await Promise.all([refreshHealth(),refreshTransport(),refreshEncoding(),refreshStereo()]); restartStreams();
  } catch (_) {}
}
restoreSession();
setInterval(refreshHealth,2000);setInterval(refreshTransport,5000);setInterval(refreshEncoding,5000);setInterval(refreshStereo,5000);
setInterval(()=>{if(lastFrameAt){const age=(Date.now()-lastFrameAt)/1000;$('age').textContent=age<1?'Updated now':`${age.toFixed(1)} s old`;$('age').classList.toggle('error',age>2)}if(encodedFrameAt){const age=(Date.now()-encodedFrameAt)/1000;$('encodedAge').textContent=age<1?'Updated now':`${age.toFixed(1)} s old`;$('encodedAge').classList.toggle('error',age>2)}},250);
