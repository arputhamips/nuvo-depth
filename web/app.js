const THEME_STORAGE_KEY = 'nuvo-console-theme';

function savedTheme() {
  try {
    const value = localStorage.getItem(THEME_STORAGE_KEY);
    return value === 'light' || value === 'dark' ? value : null;
  } catch (_) { return null; }
}

function systemTheme() {
  return window.matchMedia?.('(prefers-color-scheme: light)').matches ? 'light' : 'dark';
}

function applyTheme(theme, persist = false) {
  const selected = theme === 'light' ? 'light' : 'dark';
  document.documentElement.dataset.theme = selected;
  document.documentElement.style.colorScheme = selected;
  document.querySelector('meta[name="theme-color"]')?.setAttribute('content', selected === 'light' ? '#f1f3f6' : '#171a1f');
  const toggle = document.getElementById('themeToggle');
  if (toggle) {
    const next = selected === 'light' ? 'dark' : 'light';
    toggle.setAttribute('aria-label', `Use ${next} theme`);
    toggle.setAttribute('title', `Use ${next} theme`);
    toggle.setAttribute('aria-pressed', String(selected === 'light'));
  }
  if (persist) {
    try { localStorage.setItem(THEME_STORAGE_KEY, selected); } catch (_) {}
  }
}

applyTheme(savedTheme() || systemTheme());

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
let exposure = null, exposureFormDirty = false, exposurePending = null;
let csrf = '';
const configurationRevisions = new Map(), latestConfigurationRevisions = new Map();
let selectedPage = '';
let lastHealth = null, calibrationSession = null, calibrationOriginalEncoding = null;
let depthOutputRate = null, rawGeneration = 0;
const formControllers = new Map();
const restoredDraftForms = new Set();
const performanceModel = {run:null, published:null, samples:[], latest:null, stale:false};
let analysisTarget = 10;
const OUTPUT_PRODUCTS = [
  ['left_mono','Left mono · unrectified',1], ['right_mono','Right mono · unrectified',1],
  ['left_rgb','Left RGB · unrectified',3], ['right_rgb','Right RGB · unrectified',3],
  ['left_rectified','Left mono · rectified',1], ['right_rectified','Right mono · rectified',1],
  ['left_rectified_rgb','Left RGB · rectified',3], ['right_rectified_rgb','Right RGB · rectified',3],
  ['rectified_stereo','Stereo side by side · rectified mono',2],
  ['rectified_stereo_rgb','Stereo side by side · rectified RGB',6],
  ['disparity','Disparity · float32 pixels',4], ['depth','Depth · float32 metres',4],
  ['confidence','Confidence · uint8',1], ['validity','Validity · uint8',1]
];
let capabilities = null;
let outputSettings = {enabled:true, products:['left_mono'], cadence_hz:5, delivery:'auto', preview:true, preview_cadence_hz:5};
try {
  const saved=JSON.parse(localStorage.getItem('nuvo-output-selection')||'null');
  if(saved&&Array.isArray(saved.products)&&(saved.products.length||saved.enabled===false)&&saved.products.length<=14&&saved.products.every(p=>OUTPUT_PRODUCTS.some(x=>x[0]===p))&&
    saved.cadence_hz>0&&saved.cadence_hz<=60&&saved.preview_cadence_hz>0&&saved.preview_cadence_hz<=60&&
    ['auto','best_effort','exact'].includes(saved.delivery))outputSettings={...outputSettings,...saved};
} catch (_) {}

function outputSubscriptionPath(path, settings=outputSettings) {
  return `${path}?cadence_hz=${settings.cadence_hz}&products=${settings.products.join(',')}&delivery=${settings.delivery}`;
}
function outputBandwidth(products,width,height,cadence) {
  const bytes=products.reduce((total,name)=>total+(OUTPUT_PRODUCTS.find(p=>p[0]===name)?.[2]||0)*width*height,65568);
  return {frame_bytes:bytes, bytes_per_second:bytes*cadence};
}
function updateOutputBudget() {
  const products=[...document.querySelectorAll('[name="outputProduct"]:checked')].map(x=>x.value);
  const cadence=Number($('outputCadence').value), connection=capabilities?.connection;
  const width=lastHealth?.image_width||728,height=lastHealth?.image_height||544;
  const estimate=outputBandwidth(products,width,height,cadence);
  const budget=connection?.resource_budget_bytes_per_second;
  $('outputBandwidth').textContent=products.length?`${(estimate.bytes_per_second/1048576).toFixed(2)} MiB/s raw estimate · ${width} × ${height} · metadata allowance included`:'Select at least one raw product';
  const over=budget&&estimate.bytes_per_second>budget;
  const exactUnavailable=$('outputDelivery').value==='exact'&&connection&&!connection.qualified_link;
  $('outputWarning').textContent=exactUnavailable?'This connection has no measured exact qualification. Choose best effort or connect through a qualified wired link.':over?`Selection exceeds this connection’s ${(budget/1048576).toFixed(1)} MiB/s resource budget. Reduce cadence or choose fewer products; JPEG monitoring is available on Streaming.`:products.some(p=>['depth','disparity','confidence','validity'].includes(p))?'Selected matcher products start depth processing while this output is open.':'Image-only selection runs without the depth matcher.';
  $('outputWarning').classList.toggle('error',!!(over||exactUnavailable));
}
function outputLimits(){return {width:lastHealth?.image_width||lastHealth?.depth_width||728,height:lastHealth?.image_height||lastHealth?.depth_height||544,
  supportedProducts:capabilities?.products,qualified:!!capabilities?.connection?.qualified_link,bytesPerSecond:capabilities?.connection?.resource_budget_bytes_per_second,
  maxFrameBytes:capabilities?.stream_admission?.raw_maximum_wire_bytes,rawEnabled:encoding?.mode!=='encoded'};}
function commitOutputSelection(candidate){
  if(candidate.enabled&&!candidate.products.length)throw new Error('Select at least one raw product.');
  const checked=window.NuvoDeviceSettings.validateOutputSelection(candidate,outputLimits(),OUTPUT_PRODUCTS);
  const next={...candidate,enabled:checked.enabled,products:checked.products};
  let persisted=true;try{localStorage.setItem('nuvo-output-selection',JSON.stringify(next));}catch(_){persisted=false;}
  outputSettings=next;
  document.querySelectorAll('[name="outputProduct"]').forEach(input=>input.checked=next.products.includes(input.value));
  $('outputEnabled').checked=next.enabled;$('outputCadence').value=next.cadence_hz;$('outputDelivery').value=next.delivery;
  $('outputPreviewEnabled').checked=next.preview;$('outputPreviewCadence').value=next.preview_cadence_hz;
  $('outputSaveState').textContent=persisted?'Applied to this browser':'Applied for this session';restartStreams();updateOutputBudget();
  return persisted;
}
function initializeOutputControls() {
  for(const [name,label] of OUTPUT_PRODUCTS){
    const item=document.createElement('label'),input=document.createElement('input');
    input.type='checkbox';input.name='outputProduct';input.value=name;input.checked=outputSettings.products.includes(name);
    item.append(input,document.createTextNode(label));$('outputProductChoices').append(item);
  }
  $('outputEnabled').checked=outputSettings.enabled;$('outputCadence').value=outputSettings.cadence_hz;
  $('outputDelivery').value=outputSettings.delivery;$('outputPreviewEnabled').checked=outputSettings.preview;
  $('outputPreviewCadence').value=outputSettings.preview_cadence_hz;
  $('outputForm').addEventListener('input',updateOutputBudget);
  $('outputForm').addEventListener('submit',event=>{
    event.preventDefault();
    const products=[...document.querySelectorAll('[name="outputProduct"]:checked')].map(x=>x.value);
    if(!products.length&&$('outputEnabled').checked){$('outputWarning').textContent='Select at least one raw product';return;}
    const candidate={enabled:$('outputEnabled').checked,products,
      cadence_hz:Number($('outputCadence').value),delivery:$('outputDelivery').value,
      preview:$('outputPreviewEnabled').checked,preview_cadence_hz:Number($('outputPreviewCadence').value)};
    try{commitOutputSelection(candidate);}catch(error){$('outputWarning').textContent=error.message;$('outputWarning').classList.add('error');}
  });
  updateOutputBudget();
}
async function refreshCapabilities() {
  try {
    capabilities=await (await api('/api/v1/capabilities')).json();
    const c=capabilities.connection;
    $('outputConnection').textContent=`${c.kind.replaceAll('_',' ')} · ${c.interface} · ${(c.resource_budget_bytes_per_second/1048576).toFixed(1)} MiB/s ${c.budget_is_measured_capacity?'measured capacity':'resource limit'}`;
    $('outputRecommendation').textContent=`Recommended preview: ${c.recommended_preview.toUpperCase()} at ${c.recommended_cadence_hz} Hz. Exact delivery requires current measured source and wired-link qualification. Products and formats remain your selection.`;
    updateOutputBudget();
  } catch(error){$('outputConnection').textContent='Connection information unavailable';}
}

$('themeToggle').addEventListener('click', () => {
  applyTheme(document.documentElement.dataset.theme === 'light' ? 'dark' : 'light', true);
});
const themePreference = window.matchMedia?.('(prefers-color-scheme: light)');
themePreference?.addEventListener?.('change', event => {
  if (!savedTheme()) applyTheme(event.matches ? 'light' : 'dark');
});

function setFlowState(id, text, tone = 'waiting') {
  const status = $(id);
  status.textContent = text;
  status.className = `flow-state ${tone}`;
}

function setMeasurement(id, value, unit) {
  const element = $(id);
  element.textContent = value == null || !Number.isFinite(Number(value)) ? '—' : String(value);
  if (element.textContent === '—') return;
  const suffix = document.createElement('span');
  suffix.className = 'metric-unit';
  suffix.textContent = ` ${unit}`;
  element.append(suffix);
}

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
  placePerformanceMonitor(id);
  window.scrollTo(0, 0);
  requestAnimationFrame(() => window.scrollTo(0, 0));
  restartStreams();
}
function navigateToPage(id){
  if(location.hash!==`#${id}`)history.pushState(null,'',`#${id}`);
  openPage(id);
}
document.querySelectorAll('[data-page]').forEach(x => x.onclick = () => navigateToPage(x.dataset.page));
document.querySelectorAll('[data-open]').forEach(x => x.onclick = () => navigateToPage(x.dataset.open));
document.querySelector('.skip-link').addEventListener('click', event => {
  event.preventDefault();
  $('consoleMain').focus();
});
window.addEventListener('hashchange', () => openPage(location.hash.slice(1)));
window.addEventListener('popstate', () => openPage(location.hash.slice(1)));
openPage(location.hash.slice(1));
$('originLabel').textContent = location.origin;
$('encodedEndpoint').textContent = `${location.origin}/api/v1/encoded/latest · WebSocket /api/v1/encoded/stream`;

async function api(path,options={}) {
  return window.NuvoDeviceSettings.request(path,options,{csrf,configurationRevisions,latestConfigurationRevisions,
    encodingFormDirty,algorithmFormDirty,exposureFormDirty,safeExposureUs:Number(lastHealth?.exposure_safe_max_us)||8000,
    onUnauthorized(){if(!dialog.open)dialog.showModal();}});
}

async function refreshSetupStatus(){
  try {
    const payload=await(await api('/api/v1/setup/status')).json(),state=payload.result;
    if(!state?.schema_version)return;
    const note=$('setupState');note.hidden=false;
    note.textContent=`${state.display_name||'Nuvo'} · ${state.phase||'Setup pending'}`+
      (state.reboot_required?' · Camera changes require a reboot':'')+
      (state.calibration_state==='pending'?' · Calibration required':'')+
      (state.capture_error?` · ${state.capture_error}`:'')+
      (state.error?` · ${state.error}`:'');
  }catch(_){}
}

$('claimForm').addEventListener('submit', async event => {
  event.preventDefault(); $('claimError').textContent = '';
  const token = $('claimToken').value;
  try {
    const session = await (await api('/api/v1/session', {method:'POST', headers:{'Content-Type':'application/json'},body:JSON.stringify({secret:token})})).json();
    csrf = session.csrf; $('claimToken').value = ''; dialog.close();
    if(session.needs_owner_password)$('ownerDialog').showModal();
    await refreshSetupStatus();
    await Promise.all([refreshHealth(),refreshTransport(),refreshEncoding(),refreshStereo(),refreshExposure(),refreshCapabilities()]); restartStreams();
  } catch (error) { $('claimError').textContent = error.message; }
});

$('ownerForm').addEventListener('submit', async event => {
  event.preventDefault();$('ownerError').textContent='';
  if($('ownerPassword').value!==$('ownerRepeat').value){$('ownerError').textContent='Passwords must match.';return;}
  try {
    await api('/api/v1/setup/owner',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({password:$('ownerPassword').value})});
    $('ownerPassword').value='';$('ownerRepeat').value='';$('ownerDialog').close();
  } catch(error){$('ownerError').textContent=error.message;}
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
  $('streamEncoderEnabled').checked=value.mode!=='raw';
  $('streamRawEnabled').checked=value.mode!=='encoded';
  $('streamSource').value=value.source; $('streamWidth').value=value.width;
  $('streamHeight').value=value.height; $('streamFps').value=value.cadence_hz;
  $('streamBitrate').value=value.bitrate_bps; $('streamQuality').value=value.jpeg_quality;
  $('streamKeyframes').value=1; $('streamThreads').value=value.threads;
  $('streamLowLatency').checked=value.low_latency; $('streamPreset').value=value.preset;
  $('encodedFormat').textContent=value.mode==='raw' ? 'Disabled' : `${value.codec.toUpperCase()} · ${value.source.replaceAll('_',' ')}`;
  $('viewerModeNotice').hidden=value.mode!=='encoded';
  updateEncodingFields();
  setFormBaseline('encodingForm');
}

function updateEncodingFields(){
  const enabled=$('streamEncoderEnabled').checked;
  const h264=$('streamCodec').value==='h264';
  for(const id of ['streamCodec','streamSource','streamWidth','streamHeight','streamFps']){
    $(id).disabled=!enabled;$(id).closest('label').classList.toggle('inactive',!enabled);
  }
  for(const id of ['streamBitrate','streamKeyframes','streamThreads','streamPreset','streamLowLatency']){
    $(id).disabled=!enabled||!h264;$(id).closest('label').classList.toggle('inactive',!enabled||!h264);
  }
  $('streamQuality').disabled=!enabled||h264;$('streamQuality').closest('label').classList.toggle('inactive',!enabled||h264);
  $('encoderSettings').classList.toggle('inactive',!enabled);
}
$('streamCodec').addEventListener('change',updateEncodingFields);
$('streamEncoderEnabled').addEventListener('change',updateEncodingFields);

function encodedFrameMatchesConfiguration(metadata) {
  const revision=configurationRevisions.get('/api/v1/encoding')?.replaceAll('"','');
  if(metadata.encoding_applied_revision&&revision&&metadata.encoding_applied_revision!==revision)return false;
  return encoding && encoding.mode !== 'raw' && metadata.codec === encoding.codec &&
    metadata.source === encoding.source &&
    (!encoding.width || metadata.width === encoding.width) &&
    (!encoding.height || metadata.height === encoding.height);
}

$('encodingForm').addEventListener('input', () => {
  encodingFormDirty=true;
  $('encodingSaveState').textContent='Unsaved changes';
});

$('encodingReset').addEventListener('click', () => {
  if (!encoding) return;
  encodingFormDirty=false;
  showEncodingConfig(encoding);
  configurationRevisions.set('/api/v1/encoding',latestConfigurationRevisions.get('/api/v1/encoding'));
  $('encodingError').textContent='';
  $('encodingSaveState').textContent='Reset to saved device settings';
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
    const delivery = encoding.mode === 'both' ? 'Raw + monitor' : encoding.mode === 'raw' ? 'Raw products' : 'Monitor only';
    setFlowState('flowDeliveryState', delivery, encoding.mode === 'encoded' ? 'attention' : 'complete');
  } catch (error) { $('encodingError').textContent=error.message; setFlowState('flowDeliveryState','Waiting'); }
}

function requestedEncoding() {
  const monitor=$('streamEncoderEnabled').checked, raw=$('streamRawEnabled').checked;
  const mode=monitor&&raw?'both':monitor?'encoded':raw?'raw':'';
  return {schema_version:1,mode,codec:$('streamCodec').value,
    source:$('streamSource').value,width:Number($('streamWidth').value),height:Number($('streamHeight').value),
    cadence_hz:Number($('streamFps').value),bitrate_bps:Number($('streamBitrate').value),
    jpeg_quality:Number($('streamQuality').value),keyframe_interval:1,
    threads:Number($('streamThreads').value),low_latency:$('streamLowLatency').checked,preset:$('streamPreset').value};
}

$('encodingForm').addEventListener('submit', async event => {
  event.preventDefault(); $('encodingSaveState').textContent='Applying…'; $('encodingError').textContent='';
  setFormBusy('encodingForm',true);
  try {
    const requested=requestedEncoding();
    if(!requested.mode)throw new Error('Keep raw product delivery or the monitoring encoder enabled');
    if ((requested.width===0)!==(requested.height===0)) throw new Error('Set both width and height, or leave both at 0 for native resolution');
    encoding=await (await api('/api/v1/encoding',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(requested)})).json();
    encodingFormDirty=false; encodingPreviewPending=encoding.mode!=='raw';
    showEncodingConfig(encoding); restartStreams();
    $('encodingSaveState').textContent=encodingPreviewPending?'Saved · preview syncing…':'Saved · pending encoder confirmation';
  } catch (error) { $('encodingSaveState').textContent=''; $('encodingError').textContent=error.message; }
  finally { setFormBusy('encodingForm',false); }
});

function showStereoConfig(value) {
  const compatible={sgbm_mode:'3way',median_filter_size:0,...value};
  const fields={algorithm:'algorithmType',sgbm_mode:'sgbmMode',min_disparity:'minDisparity',
    search_range:'searchRange',max_disparity:'maxDisparity',block_size:'blockSize',
    uniqueness_ratio:'uniquenessRatio',speckle_window_size:'speckleWindow',
    speckle_range:'speckleRange',disparity_12_max_diff:'disparity12Diff',
    pre_filter_cap:'preFilterCap',bm_pre_filter_size:'bmPreFilterSize',
    bm_texture_threshold:'bmTextureThreshold',specular_threshold:'specularThreshold',
    median_filter_size:'medianFilterSize',
    opencv_threads:'algorithmThreads'};
  for(const [key,id] of Object.entries(fields))$(id).value=compatible[key];
  updateAlgorithmFields();
  updateActiveStereoLabel();
  setFormBaseline('algorithmForm');
  updateAppliedSummaries();
}
function updateActiveStereoLabel(){
  if(!stereo){$('algorithmActive').textContent=`Active matcher: ${activeStereoBackend||'waiting for depth frame'}`;return;}
  const mode=stereo.sgbm_mode||'3way';
  const detail=stereo.algorithm==='sgbm'?`${mode.toUpperCase()} · median ${stereo.median_filter_size||'off'}`:'StereoBM';
  $('algorithmActive').textContent=`Active matcher: ${activeStereoBackend||'waiting'} · requested ${detail}`;
}
function updateAlgorithmFields(){
  const bm=$('algorithmType').value==='bm';
  document.querySelectorAll('.bm-only').forEach(label=>{
    label.classList.toggle('inactive',!bm);
    label.hidden=!bm;
    label.querySelector('input').disabled=!bm;
  });
  document.querySelectorAll('.sgbm-only').forEach(label=>{
    label.classList.toggle('inactive',bm);
    label.hidden=bm;
    label.querySelector('select').disabled=bm;
  });
}
function requestedStereo(){
  return {schema_version:1,algorithm:$('algorithmType').value,sgbm_mode:$('sgbmMode').value,
    min_disparity:Number($('minDisparity').value),search_range:Number($('searchRange').value),
    max_disparity:Number($('maxDisparity').value),block_size:Number($('blockSize').value),
    uniqueness_ratio:Number($('uniquenessRatio').value),
    speckle_window_size:Number($('speckleWindow').value),speckle_range:Number($('speckleRange').value),
    disparity_12_max_diff:Number($('disparity12Diff').value),pre_filter_cap:Number($('preFilterCap').value),
    bm_pre_filter_size:Number($('bmPreFilterSize').value),
    bm_texture_threshold:Number($('bmTextureThreshold').value),
    specular_threshold:Number($('specularThreshold').value),
    median_filter_size:Number($('medianFilterSize').value),
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
    const mode=stereo.sgbm_mode||'3way';
    const profile = stereo.algorithm === 'bm' ? 'StereoBM' : mode === 'hh' ? 'Quality · HH' : `SGBM · ${mode.toUpperCase()}`;
    setFlowState('flowQualityState', profile, stereo.algorithm === 'sgbm' && mode === '3way' ? 'attention' : 'complete');
  }catch(error){$('algorithmError').textContent=error.message;setFlowState('flowQualityState','Waiting');}
}
$('algorithmType').addEventListener('change',updateAlgorithmFields);
$('algorithmForm').addEventListener('input',()=>{
  algorithmFormDirty=true;$('algorithmSaveState').textContent='Unsaved changes';
});
$('algorithmReset').addEventListener('click', () => {
  if (!stereo) return;
  algorithmFormDirty=false;
  showStereoConfig(stereo);
  configurationRevisions.set('/api/v1/stereo',latestConfigurationRevisions.get('/api/v1/stereo'));
  $('algorithmError').textContent='';
  $('algorithmSaveState').textContent='Reset to saved device settings';
});
$('algorithmForm').addEventListener('submit',async event=>{
  event.preventDefault();$('algorithmSaveState').textContent='Applying…';$('algorithmError').textContent='';
  setFormBusy('algorithmForm',true);
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
  finally { setFormBusy('algorithmForm',false); }
});

let depthProfiles=null;
async function loadDepthProfiles(){
  const buttons=[...document.querySelectorAll('[data-depth-preset]')];
  buttons.forEach(button=>{button.disabled=true});
  try{
    const response=await fetch('/depth-profiles.json',{cache:'no-store'});
    if(!response.ok)throw new Error('Depth profile catalog unavailable');
    const catalog=await response.json();
    if(catalog.schema_version!==1||catalog.capture?.width!==728||catalog.capture?.height!==544||
       catalog.base_controls?.schema_version!==1||!catalog.profiles)throw new Error('Unsupported depth profile catalog');
    for(const button of buttons){
      const profile=catalog.profiles[button.dataset.depthPreset];
      if(!profile||profile.qualification!=='unqualified_candidate'||profile.depth_rate_guarantee_hz!==null||
         profile.matching?.width!==catalog.capture.width||profile.matching?.height!==catalog.capture.height||
         !profile.controls||typeof profile.label!=='string'||typeof profile.description!=='string'){
        throw new Error('Invalid depth profile catalog');
      }
    }
    depthProfiles=catalog;
    for(const button of buttons){
      const profile=catalog.profiles[button.dataset.depthPreset];
      button.closest('article').querySelector('h2').textContent=profile.label;
      button.closest('article').querySelector('p').textContent=profile.description;
      button.disabled=false;
    }
  }catch(error){depthProfiles=null;$('presetError').textContent=error.message;}
}
loadDepthProfiles();
document.querySelectorAll('[data-depth-preset]').forEach(button=>{
  button.onclick=async()=>{
    $('presetSaveState').textContent='Applying…';$('presetError').textContent='';
    try{
      if(!depthProfiles)throw new Error('Depth profile catalog unavailable');
      const profile=depthProfiles.profiles[button.dataset.depthPreset];
      const requested={...depthProfiles.base_controls,...profile.controls};
      stereo=await (await api('/api/v1/stereo',{method:'POST',
        headers:{'Content-Type':'application/json'},body:JSON.stringify(requested)})).json();
      algorithmFormDirty=false;showStereoConfig(stereo);
      $('presetSaveState').textContent=`Saved ${profile.label} · throughput unqualified · waiting for depth frame`;
    }catch(error){$('presetSaveState').textContent='';$('presetError').textContent=error.message;}
  };
});

function showExposureConfig(value){
  const fields={mode:'exposureMode',manual_exposure_us:'manualExposure',
    manual_analogue_gain:'manualGain',red_gain:'redGain',blue_gain:'blueGain',
    auto_target_median:'autoTargetMedian',auto_min_median:'autoMinMedian',
    auto_max_median:'autoMaxMedian',auto_min_exposure_us:'autoMinExposure',
    auto_max_exposure_us:'autoMaxExposure',auto_min_analogue_gain:'autoMinGain',
    auto_max_analogue_gain:'autoMaxGain',auto_update_interval_frames:'autoUpdateInterval',
    auto_settle_frames_after_update:'autoSettleFrames'};
  for(const [key,id] of Object.entries(fields))$(id).value=value[key];
  updateExposureFields();
  updateExposureMilliseconds();
  setFormBaseline('exposureForm');
  updateAppliedSummaries();
}
function updateExposureMilliseconds(){
  for(const [input,output] of [['manualExposure','manualExposureMs'],['autoMinExposure','autoMinExposureMs'],['autoMaxExposure','autoMaxExposureMs']]){
    const value=Number($(input).value);
    $(output).textContent=Number.isFinite(value)?(value/1000).toFixed(value%1000===0?0:2):'—';
  }
}
function updateExposureFields(){
  const automatic=$('exposureMode').value==='auto';
  document.querySelectorAll('.manual-exposure').forEach(label=>{
    label.classList.toggle('inactive',automatic);
    label.querySelector('input').disabled=automatic;
  });
  document.querySelectorAll('.auto-exposure').forEach(label=>{
    label.classList.toggle('inactive',!automatic);
    label.querySelector('input').disabled=!automatic;
  });
  document.querySelectorAll('.auto-advanced').forEach(section=>section.classList.toggle('inactive',!automatic));
}
function requestedExposure(){
  return {schema_version:1,mode:$('exposureMode').value,
    manual_exposure_us:Number($('manualExposure').value),
    manual_analogue_gain:Number($('manualGain').value),
    red_gain:Number($('redGain').value),blue_gain:Number($('blueGain').value),
    auto_target_median:Number($('autoTargetMedian').value),
    auto_min_median:Number($('autoMinMedian').value),
    auto_max_median:Number($('autoMaxMedian').value),
    auto_min_exposure_us:Number($('autoMinExposure').value),
    auto_max_exposure_us:Number($('autoMaxExposure').value),
    auto_min_analogue_gain:Number($('autoMinGain').value),
    auto_max_analogue_gain:Number($('autoMaxGain').value),
    auto_update_interval_frames:Number($('autoUpdateInterval').value),
    auto_settle_frames_after_update:Number($('autoSettleFrames').value)};
}
async function refreshExposure(){
  try{
    const updated=await (await api('/api/v1/exposure')).json();
    const changed=exposure&&JSON.stringify(updated)!==JSON.stringify(exposure);
    exposure=updated;
    if(!exposureFormDirty)showExposureConfig(exposure);
    else if(changed)$('exposureSaveState').textContent='Device changed · review and apply';
    $('exposureError').textContent='';
  }catch(error){$('exposureError').textContent=error.message;}
}
$('exposureMode').addEventListener('change',updateExposureFields);
$('exposureForm').addEventListener('input',()=>{
  exposureFormDirty=true;exposurePending=null;updateExposureMilliseconds();
  $('exposureSaveState').textContent='Unsaved changes';
});
$('exposureReset').addEventListener('click',()=>{
  if(exposure)showExposureConfig(exposure);
  exposureFormDirty=false;exposurePending=null;
  configurationRevisions.set('/api/v1/exposure',latestConfigurationRevisions.get('/api/v1/exposure'));
  $('exposureError').textContent='';$('exposureSaveState').textContent='Reset to saved device settings';
});
$('exposureForm').addEventListener('submit',async event=>{
  event.preventDefault();$('exposureSaveState').textContent='Applying…';$('exposureError').textContent='';
  setFormBusy('exposureForm',true);
  try{
    const requested=requestedExposure();
    const safeMax=Number(lastHealth?.exposure_safe_max_us)||8000;
    if(requested.manual_exposure_us>safeMax||requested.auto_max_exposure_us>safeMax)
      throw new Error(`Exposure must not exceed the live ${safeMax} µs trigger-period limit`);
    if(requested.auto_min_median>requested.auto_target_median||
       requested.auto_target_median>requested.auto_max_median)
      throw new Error('Auto target must be inside the median deadband');
    if(requested.auto_min_exposure_us>requested.auto_max_exposure_us||
       requested.auto_min_analogue_gain>requested.auto_max_analogue_gain)
      throw new Error('Auto minimums must not exceed maximums');
    const changed=!exposure||Object.entries(requested).some(([key,value])=>exposure[key]!==value);
    const baselineReloads=Number(lastHealth?.exposure_configuration_reloads)||0;
    exposure=await (await api('/api/v1/exposure',{method:'POST',
      headers:{'Content-Type':'application/json'},body:JSON.stringify(requested)})).json();
    exposureFormDirty=false;showExposureConfig(exposure);
    exposurePending=changed?{mode:requested.mode,baselineReloads}:null;
    $('exposureSaveState').textContent='Saved · waiting for camera confirmation';
  }catch(error){$('exposureSaveState').textContent='';$('exposureError').textContent=error.message;}
  finally { setFormBusy('exposureForm',false); }
});

async function refreshHealth() {
  try {
    const health = await (await api('/api/v1/health')).json();
    lastHealth = health;
    updateViewerAvailability();
    $('calibrationPresetId').value=health.profile_id||'';
    const now=performance.now();
    let depthActivity='Waiting for sample';
    const depthPublished=depthPublicationCount(health);
    if (health.run_uuid===lastHealthRun && lastHealthPublished!==null && depthPublished>=lastHealthPublished && now>lastHealthAt) {
      const fps=1000*(depthPublished-lastHealthPublished)/(now-lastHealthAt);
      depthOutputRate=fps;
      setMeasurement('frameRate',fps.toFixed(1),'fps');
      depthActivity=fps>0?'Active':'No new frames';
    } else { $('frameRate').textContent='—'; depthOutputRate=null; }
    // matcher_active is an instantaneous sample and can be false between frames.
    if(health.matcher_active===false&&!(depthOutputRate>0)){depthOutputRate=0;setMeasurement('frameRate','0.0','fps');depthActivity='Idle · no new depth frames';}
    lastHealthRun=health.run_uuid||'';lastHealthPublished=depthPublished;lastHealthAt=now;
    $('connection').textContent = 'Connected'; $('connection').className = 'pill online';
    document.querySelector('.status-strip').classList.remove('stale');
    setFlowState('flowConnectionState','Secure session','complete');
    setFlowState('flowCaptureState',health.status === 'running' ? 'Frames active' : String(health.status || 'Needs attention'),health.status === 'running' ? 'complete' : 'attention');
    $('cameraState').textContent = health.status === 'running' ? 'Streaming' : health.status;
    $('overviewCaptureState').textContent = health.status === 'running' ? 'Active' : String(health.status || 'Unknown');
    $('overviewDepthState').textContent = depthActivity;
    $('deviceResolution').textContent = `${health.depth_width ?? '—'} × ${health.depth_height ?? '—'}`;
    $('cameraDetail').textContent = `Capture ${health.capture_epoch ?? '—'} · frame ${health.capture_sequence ?? '—'}`;
    $('frameDetail').textContent=`${health.depth_width??'—'} × ${health.depth_height??'—'} native depth grid`;
    setMeasurement('processingTime',health.processing_ms == null ? null : health.processing_ms.toFixed(1),'ms');
    recordPerformanceSample(performanceModel,health,Date.now());
    renderPerformanceMonitor();
    updateLiveMetrics(health);
    const requestedStereoRevision=configurationRevisions.get('/api/v1/stereo')?.slice(1,-1);
    if(!algorithmFormDirty&&requestedStereoRevision){
      if(health.stereo_requested_revision===requestedStereoRevision&&health.stereo_configuration_error){
        $('algorithmSaveState').textContent='Saved · rejected by matcher';
        $('algorithmError').textContent=health.stereo_configuration_error;
      }else if(health.stereo_applied_revision===requestedStereoRevision){
        $('algorithmSaveState').textContent=`Applied · configuration epoch ${health.configuration_epoch}`;
      }else $('algorithmSaveState').textContent='Saved · waiting for depth processing';
      toneSaveStatus($('algorithmSaveState'));
    }
    activeStereoBackend=health.stereo_backend||'';
    updateActiveStereoLabel();
    $('exposureActive').textContent=health.exposure_mode==='auto'?'Auto':'Manual';
    const safeExposure=Number(health.exposure_safe_max_us)||8000;
    const periodUs=Number(health.exposure_frame_period_us);
    for(const id of ['manualExposure','autoMinExposure','autoMaxExposure'])$(id).max=String(safeExposure);
    $('exposureTiming').textContent=Number.isFinite(periodUs)&&periodUs>0
      ?`At ${health.capture_target_hz??'—'} fps the trigger period is ${(periodUs/1000).toFixed(2)} ms; the live shutter limit is ${(safeExposure/1000).toFixed(2)} ms, including a ${health.exposure_trigger_guard_us??'—'} µs guard.`
      :`The live shutter limit is ${(safeExposure/1000).toFixed(2)} ms.`;
    $('exposureRequested').textContent=health.requested_exposure_us==null?'—':`${health.requested_exposure_us} µs`;
    $('gainRequested').textContent=health.requested_analogue_gain==null?'—':`${health.requested_analogue_gain.toFixed(2)}× analogue gain`;
    const leftExposure=health.left_reported_exposure_us,rightExposure=health.right_reported_exposure_us;
    $('exposureReported').textContent=leftExposure==null?'—':`${leftExposure} / ${rightExposure} µs`;
    $('gainReported').textContent=health.left_reported_analogue_gain==null?'—':`${health.left_reported_analogue_gain.toFixed(2)}× / ${health.right_reported_analogue_gain.toFixed(2)}×`;
    const exposureAutomatic=health.exposure_mode==='auto';
    $('exposureMedian').textContent=!exposureAutomatic?'Off':health.metered_median==null?'—':health.metered_median.toFixed(0);
    const leftMedian=health.metered_left_median,rightMedian=health.metered_right_median;
    const stateLabels={within_deadband:'Inside target band',settling:'Waiting for camera controls',waiting_interval:'Waiting for next sample',confirming_dark:'Confirming darker scene',confirming_bright:'Confirming brighter scene',adjusted:'Adjustment sent',at_limit_or_quantized:'At limit or below one step',auto_ready:'Auto ready'};
    const stateText=health.exposure_last_error||stateLabels[health.exposure_controller_state]||health.exposure_last_reason||'No adjustment needed';
    $('exposureReason').textContent=!exposureAutomatic?'Manual values held constant':`${stateText}${leftMedian==null?'':` · L/R ${leftMedian.toFixed(0)}/${rightMedian.toFixed(0)}`}`;
    if(exposurePending){
      const reloaded=Number(health.exposure_configuration_reloads)>exposurePending.baselineReloads;
      if(reloaded&&health.exposure_mode===exposurePending.mode){
        $('exposureSaveState').textContent='Saved · exposure commands reloaded';exposurePending=null;
        toneSaveStatus($('exposureSaveState'));
      }
    }
    const provisional = health.calibration_authority === 0;
    const accepted = typeof health.calibration_authority === 'number' && health.calibration_authority > 0 && !!health.calibration_sha256;
    $('calibrationState').textContent = provisional ? 'Provisional' : accepted ? 'Accepted' : 'Unavailable';
    $('calibrationState').classList.toggle('attention',provisional);
    $('calibrationState').className='chip '+(provisional?'chip-amber':accepted?'chip-green':'chip-red');
    $('calibrationDetail').textContent = provisional ? 'Metric accuracy unverified' : accepted ? 'Declared validation passed' : 'No accepted calibration';
    $('calibrationHash').textContent = health.calibration_sha256 ? health.calibration_sha256.slice(0,16)+'…' : 'Not loaded';
    $('calibrationHash').title=health.calibration_sha256 || '';
    $('calibrationNotice').textContent = provisional ? 'Metric depth is unverified. Engineering use only.' : accepted ? 'The active calibration passed its declared validation authority.' : 'Depth requires an active calibration.';
    $('calibrationBanner').hidden=accepted;
    $('diagnosticJson').textContent = JSON.stringify(health,null,2);
  } catch (_) { updateViewerAvailability(true); $('connection').textContent = 'Not connected'; $('connection').className = 'pill offline'; $('frameRate').textContent='—'; depthOutputRate=null; lastHealthPublished=null; performanceModel.stale=true; performanceModel.latest=null; renderPerformanceMonitor(); updateLiveMetrics(null); document.querySelector('.status-strip').classList.add('stale'); $('overviewCaptureState').textContent='Disconnected'; $('overviewDepthState').textContent='Unknown'; $('deviceResolution').textContent='Last known device state'; setFlowState('flowConnectionState','Waiting'); setFlowState('flowCaptureState','Waiting'); }
}

function parseFrame(buffer) { return window.NuvoFrameCodec.parseFrame(buffer); }
function product(frame, name) { return frame.metadata.products.find(x => x.kind === name); }
function setSampleCanvasLive(id, live) {
  $(id).closest('.sample-canvas')?.classList.toggle('live', live);
}
function updateViewerAvailability(disconnected=false) {
  const empty=$('viewerEmptyState');
  empty.hidden=!disconnected&&!!lastFrameAt;
  empty.textContent=disconnected?(lastFrameAt?'Disconnected · showing last received products':'Disconnected · waiting for device'):
    !outputSettings.enabled?'Select raw outputs on the Outputs page.':encoding?.mode==='encoded'?'Raw delivery is off. Enable raw products on Streaming.':'Waiting for selected live products.';
  $('encodedEmptyState').textContent=disconnected?'Disconnected · waiting for encoded preview':encoding?.mode==='raw'?'Encoded preview is off':'Waiting for encoded preview';
  if(disconnected)setSampleCanvasLive('encodedCanvas',false);
}
function drawGray(canvas,frame,descriptor) { return window.NuvoFrameCodec.drawGray(canvas,frame,descriptor); }
function drawDepth(canvas,frame,descriptor) {
  const valid=window.NuvoFrameCodec.drawDepth(canvas,frame,descriptor);
  if(valid!==undefined)$('validity').textContent=`Valid ${valid.toFixed(1)}%`;
}
function drawDisparity(canvas, frame, descriptor) {
  return window.NuvoFrameCodec.drawDisparity(canvas,frame,descriptor,stereo?.max_disparity||128);
}
function canvasVisible(canvas) {
  return !document.hidden && !!canvas.closest('.page.active') && !canvas.closest('[hidden]');
}
function drawVisibleProduct(canvas,frame,descriptor) {
  if(!descriptor||!canvasVisible(canvas))return;
  if(descriptor.kind==='depth')drawDepth(canvas,frame,descriptor);
  else if(descriptor.kind==='disparity')drawDisparity(canvas,frame,descriptor);
  else drawGray(canvas,frame,descriptor);
}
function renderFrame(frame) {
  const identity=`${frame.metadata.run_uuid}:${frame.metadata.capture_epoch}:${frame.metadata.capture_sequence}:${frame.metadata.products.map(p=>p.kind).join(',')}`;
  if(identity===lastIdentity)return;
  const left=product(frame,'left_rectified'),right=product(frame,'right_rectified');
  const disparity=product(frame,'disparity'),depth=product(frame,'depth');
  const firstImage=left||frame.metadata.products.find(p=>['gray8','rgb8','bgr8'].includes(p.format));
  for(const [id,descriptor] of [['leftCanvas',firstImage],['rightCanvas',right],['disparityCanvas',disparity],['depthCanvas',depth]])$(id).closest('figure').hidden=!descriptor;
  if(firstImage)$('leftCanvas').closest('figure').querySelector('figcaption').textContent=OUTPUT_PRODUCTS.find(p=>p[0]===firstImage.kind)?.[1]||firstImage.kind;
  if(frame.metadata.products.length){
    $('viewerEmptyState').hidden=true;
    $('viewerLiveGrid').hidden=false;
    $('viewerTelemetry').hidden=false;
  }
  for(const [id,descriptor] of [['leftCanvas',firstImage],['rightCanvas',right],['disparityCanvas',disparity],['depthCanvas',depth]])drawVisibleProduct($(id),frame,descriptor);
  for(const grid of [$('outputPreviewGrid'),$('selectedProductGrid')]){
    if(!canvasVisible(grid))continue;
    for(const descriptor of frame.metadata.products){
      if(grid.id==='selectedProductGrid'&&[firstImage?.kind,'right_rectified','depth','disparity'].includes(descriptor.kind))continue;
      let canvas=grid.querySelector(`[data-output-product="${descriptor.kind}"]`);
      if(!canvas){const figure=document.createElement('figure'),caption=document.createElement('figcaption');canvas=document.createElement('canvas');canvas.dataset.outputProduct=descriptor.kind;caption.textContent=OUTPUT_PRODUCTS.find(p=>p[0]===descriptor.kind)?.[1]||descriptor.kind;figure.append(caption,canvas);grid.append(figure)}
      drawVisibleProduct(canvas,frame,descriptor);
    }
  }
  document.querySelectorAll('.page.active [data-tuning-product]').forEach(canvas=>{
    const isLeft=canvas.dataset.tuningProduct==='left_rectified',descriptor=isLeft?firstImage:depth;
    canvas.closest('.sample-canvas').classList.toggle('live',!!descriptor);
    const placeholder=canvas.closest('.sample-canvas').querySelector('.sample-overlay span');
    if(placeholder&&!descriptor)placeholder.textContent=isLeft?'No image output selected':'Depth not selected';
    if(isLeft&&descriptor)canvas.closest('figure').querySelector('figcaption').firstChild.textContent=OUTPUT_PRODUCTS.find(p=>p[0]===descriptor.kind)?.[1]||descriptor.kind;
    drawVisibleProduct(canvas,frame,descriptor);
  });
  document.querySelectorAll('.page.active [data-preview-state]').forEach(label=>{label.textContent='Selected products live';label.className='chip chip-green';});
  lastFrameAt=Date.now();lastSequence=frame.sequence;lastIdentity=identity;
  setFlowState('flowCaptureState','Products live','complete');
  $('sequence').textContent=`Frame ${frame.metadata.capture_sequence}`; $('age').textContent='Updated now';$('frameError').textContent='';
}
function rawPreviewPage(){return !document.hidden&&outputSettings.enabled&&['overview','viewer','exposure','algorithm','network'].includes(selectedPage);}
async function pollFrame(generation) {
  if (!rawPolling||generation!==rawGeneration||!rawPreviewPage()) return;
  try { const buffer=await (await api(outputSubscriptionPath('/api/v1/frame/latest'))).arrayBuffer();if(generation!==rawGeneration||!rawPolling)return;renderFrame(parseFrame(buffer)); }
  catch (error) { if(generation!==rawGeneration)return;$('frameError').textContent=error.message; }
  if(generation===rawGeneration&&rawPolling)setTimeout(()=>pollFrame(generation),1000/outputSettings.cadence_hz);
}
function socketUrl(path) { return `${location.protocol==='https:'?'wss':'ws'}://${location.host}${path}`; }
function startRawViewer(){
  if(rawSocket||rawPolling||!rawPreviewPage()||!encoding||encoding.mode==='encoded')return;
  const generation=rawGeneration,socket=new WebSocket(socketUrl(outputSubscriptionPath('/api/v1/stream')));rawSocket=socket;socket.binaryType='arraybuffer';
  socket.onmessage=event=>{if(generation!==rawGeneration||socket!==rawSocket)return;try{renderFrame(parseFrame(event.data))}catch(_){socket.close()}};
  socket.onclose=()=>{if(generation!==rawGeneration||socket!==rawSocket)return;rawSocket=null;if(rawPreviewPage()&&encoding&&encoding.mode!=='encoded'&&!rawPolling){rawPolling=true;pollFrame(generation)}};
}

function parseEncoded(buffer) { return window.NuvoFrameCodec.parseEncoded(buffer); }
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
  decoder=new VideoDecoder({output:video=>{if(generation!==encodedGeneration){video.close();return}drawEncodedSource(video,video.displayWidth,video.displayHeight);video.close();},error:error=>{encodedWaitingForKeyframe=true;$('encodingError').textContent=`Decoder resynchronizing: ${error.message}`}});
  decoder.configure({codec,codedWidth:width,codedHeight:height,optimizeForLatency:true,hardwareAcceleration:'prefer-hardware'});
  return decoder;
}
function drawEncodedSource(source,width,height){
  const canvases=[];
  if(selectedPage==='streaming')canvases.push($('encodedCanvas'));
  if(selectedPage==='network')canvases.push($('outputEncodedCanvas'));
  if($('calibration').classList.contains('active'))canvases.push($('calibrationCanvas'));
  for(const canvas of canvases){if(canvasVisible(canvas))window.NuvoFrameCodec.prepareCanvas(canvas,width,height).context.drawImage(source,0,0)}
  setSampleCanvasLive('encodedCanvas',true);
}
async function renderEncoded(frame,generation,arrival) {
  const {metadata,payload}=frame;
  if(generation!==encodedGeneration||!encodedFrameMatchesConfiguration(metadata))return;
  if(metadata.codec==='jpeg'){
    const bitmap=await createImageBitmap(new Blob([payload],{type:'image/jpeg'}));
    if(generation!==encodedGeneration||!encodedFrameMatchesConfiguration(metadata)||arrival<encodedLastPresented){bitmap.close();return}
    encodedLastPresented=arrival;drawEncodedSource(bitmap,bitmap.width,bitmap.height);bitmap.close();
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
  if(encodingPreviewPending){encodingPreviewPending=false;if(!encodingFormDirty)$('encodingSaveState').textContent='Saved · preview available';}
}
async function pollEncoded(generation) {
  if(!encodedPolling||generation!==encodedGeneration||!encodedPreviewPage())return;
  try{await renderEncoded(parseEncoded(await (await api(`/api/v1/encoded/latest?cadence_hz=${outputSettings.preview_cadence_hz}`)).arrayBuffer()),generation,++encodedArrival);}
  catch(error){if(generation===encodedGeneration){$('encodingError').textContent=error.message;}}
  if(generation===encodedGeneration&&encodedPolling)setTimeout(()=>pollEncoded(generation),1000/outputSettings.preview_cadence_hz);
}
function encodedPreviewPage(){return !document.hidden&&(selectedPage==='calibration'||outputSettings.preview&&['streaming','network'].includes(selectedPage));}
function startEncodedViewer(){
  if(encodedSocket||encodedPolling||!encodedPreviewPage()||!encoding||encoding.mode==='raw')return;
  const generation=encodedGeneration, socket=new WebSocket(socketUrl(`/api/v1/encoded/stream?cadence_hz=${outputSettings.preview_cadence_hz}`));
  encodedSocket=socket;socket.binaryType='arraybuffer';
  socket.onmessage=event=>{if(generation!==encodedGeneration||socket!==encodedSocket)return;try{renderEncoded(parseEncoded(event.data),generation,++encodedArrival).catch(error=>{$('encodingError').textContent=error.message})}catch(error){$('encodingError').textContent=error.message}};
  socket.onclose=()=>{if(generation!==encodedGeneration||socket!==encodedSocket)return;encodedSocket=null;if(encodedPreviewPage()&&encoding&&encoding.mode!=='raw'&&!encodedPolling){encodedPolling=true;pollEncoded(generation)}};
}
function restartStreams(){
  ++rawGeneration;
  ++encodedGeneration;encodedArrival=0;encodedLastPresented=0;
  rawPolling=false;encodedPolling=false;
  if(rawSocket){const value=rawSocket;rawSocket=null;value.onmessage=null;value.onclose=null;value.close()}
  if(encodedSocket){const value=encodedSocket;encodedSocket=null;value.onmessage=null;value.onclose=null;value.onerror=null;value.close()}
  resetEncodedDecoder();
  $('outputPreviewGrid').replaceChildren();$('selectedProductGrid').replaceChildren();
  for(const id of ['leftCanvas','rightCanvas','disparityCanvas','depthCanvas','encodedCanvas','calibrationCanvas']){
    const canvas=$(id);canvas.width=728;canvas.height=544;
  }
  setSampleCanvasLive('encodedCanvas',false);
  document.querySelectorAll('[data-tuning-product]').forEach(canvas=>{canvas.width=728;canvas.height=544;canvas.closest('.sample-canvas').classList.remove('live');});
  document.querySelectorAll('[data-preview-state]').forEach(label=>{label.textContent=encoding?.mode==='encoded'?'Raw delivery off':'Waiting for live frames';label.className='chip chip-neutral';});
  document.querySelectorAll('.preview-unavailable').forEach(note=>note.hidden=encoding?.mode!=='encoded');
  $('viewerEmptyState').hidden=false;
  $('viewerLiveGrid').hidden=true;
  $('viewerTelemetry').hidden=true;
  $('sequence').textContent='Frame —';$('validity').textContent='Valid —';$('age').textContent='Waiting for products';lastFrameAt=0;lastIdentity='';
  $('encodedSequence').textContent='Frame —';$('encodedSize').textContent='—';$('encodedLatency').textContent='—';$('encodedAge').textContent='Waiting';encodedFrameAt=0;
  updateViewerAvailability();
  startRawViewer();startEncodedViewer();
}

const CALIBRATION_SESSION_KEY='nuvo-calibration-session';

async function calibrationOperation(path,body={}){
  const envelope=await (await api(path,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})).json();
  if(envelope.status!=='ok'||!envelope.result)throw new Error(envelope.error||'Calibration operation failed');
  return envelope.result;
}

function calibrationProfileId(health){
  if(typeof health?.profile_id!=='string'||!/^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/.test(health.profile_id))
    throw new Error('The active capture profile is not available yet; wait for a live frame');
  return health.profile_id;
}

function calibrationConfiguration(){
  if(!lastHealth?.device_uuid||!lastHealth?.left_camera_id||!lastHealth?.right_camera_id)
    throw new Error('Camera identities are not available yet; wait for a live frame');
  const rates=$('calibrationRates').value.split(',').map(value=>Number(value.trim())).filter(value=>Number.isFinite(value));
  if(!rates.length||new Set(rates).size!==rates.length||rates.some(value=>value<=0||value>240))
    throw new Error('Enter unique frame rates between 0 and 240 Hz');
  const squareMm=Number($('charucoSquareMm').value),markerMm=Number($('charucoMarkerMm').value);
  if(markerMm>=squareMm)throw new Error('Marker size must be smaller than square size');
  const maximumCorners=(Number($('charucoSquaresX').value)-1)*(Number($('charucoSquaresY').value)-1);
  if(Number($('calibrationMinimumCorners').value)>maximumCorners)
    throw new Error(`This board has at most ${maximumCorners} ChArUco corners`);
  return {schema_version:1,
    identity:{unit_id:lastHealth.device_uuid,left_camera_id:lastHealth.left_camera_id,right_camera_id:lastHealth.right_camera_id},
    profile:{preset_id:calibrationProfileId(lastHealth),sensor_mode:$('calibrationSensorMode').value.trim(),
      image_width:Number($('calibrationWidth').value),image_height:Number($('calibrationHeight').value),
      frame_rates_hz:rates,baseline_m:Number($('calibrationBaseline').value)/1000},
    target:{type:'charuco',squares_x:Number($('charucoSquaresX').value),squares_y:Number($('charucoSquaresY').value),
      square_m:squareMm/1000,marker_m:markerMm/1000,dictionary:$('charucoDictionary').value},
    thresholds:{minimum_pairs:Number($('calibrationMinimumPairs').value),minimum_shared_corners:Number($('calibrationMinimumCorners').value),
      maximum_rms_px:Number($('calibrationMaximumRms').value),maximum_epipolar_p95_px:Number($('calibrationMaximumEpipolar').value),
      baseline_tolerance_m:Number($('calibrationBaselineTolerance').value)/1000,minimum_coverage:Number($('calibrationMinimumCoverage').value)}};
}

function calibrationStep(state){
  if(['passed','failed','active'].includes(state))return 'solve';
  if(state==='ready'||state==='collecting'||state==='solving')return state==='solving'?'solve':'capture';
  return 'setup';
}

function calibrationCoverageCells(status){
  if(Array.isArray(status.coverage?.occupied_cell_indices)){
    return new Set(status.coverage.occupied_cell_indices.filter(cell=>Number.isInteger(cell)&&cell>=0&&cell<9));
  }
  const occupied=new Set();
  for(const item of status.captures||[]){
    const metric=item.accepted&&item.quality?.left;
    if(metric)occupied.add(Math.min(2,Math.floor(metric.center_y*3))*3+Math.min(2,Math.floor(metric.center_x*3)));
  }
  return occupied;
}

function renderCalibration(status){
  calibrationSession=status;
  $('calibrationWorkspace').hidden=false;
  $('calibrationAccepted').textContent=`${status.accepted_pairs} / ${status.required_pairs}`;
  $('calibrationRejected').textContent=String(status.rejected_pairs);
  $('calibrationCoverage').textContent=`${Math.round(100*(status.coverage?.score||0))}%`;
  $('calibrationGuidance').textContent=status.message||'Continue collecting varied target poses.';
  $('calibrationCaptureState').textContent=status.captures?.length?`Last pair: ${status.captures.at(-1).accepted?'accepted':'retake'} · ${status.captures.at(-1).common_corners} shared corners`:'';
  const occupied=calibrationCoverageCells(status);
  [...$('calibrationCoverageGrid').children].forEach((cell,index)=>cell.classList.toggle('covered',occupied.has(index)));
  const activeStep=calibrationStep(status.state);
  document.querySelectorAll('[data-calibration-step]').forEach(step=>step.classList.toggle('active',step.dataset.calibrationStep===activeStep));
  $('calibrationSolve').disabled=status.state!=='ready';
  $('calibrationCapture').disabled=status.state==='solving'||status.state==='active';
  $('calibrationPair').hidden=!status.captures?.length;
  if(status.captures?.length){
    const version=Date.now();
    $('calibrationLeftPreview').src=`/api/v1/calibration/preview/left?session=${status.session_id}&v=${version}`;
    $('calibrationRightPreview').src=`/api/v1/calibration/preview/right?session=${status.session_id}&v=${version}`;
  }
  if(status.validation){
    const result=status.validation,passed=result.status==='pass';
    $('calibrationResults').hidden=false;
    $('calibrationResultTitle').textContent=passed?'Calibration passed':'Calibration needs another capture set';
    $('calibrationResultBadge').textContent=passed?'PASS':'RETAKE';
    $('calibrationResultBadge').className=`pill ${passed?'online':'offline'}`;
    $('calibrationRms').textContent=`${result.left_rms_px.toFixed(3)} / ${result.right_rms_px.toFixed(3)} px`;
    $('calibrationStereoRms').textContent=`${result.stereo_rms_px.toFixed(3)} px`;
    $('calibrationEpipolar').textContent=`${result.rectified_vertical_error_p95_px.toFixed(3)} px`;
    $('calibrationEpipolarMax').textContent=result.rectified_vertical_error_max_px.toFixed(3);
    $('calibrationSolvedBaseline').textContent=`${(1000*result.baseline_m).toFixed(2)} mm`;
    $('calibrationBaselineError').textContent=`${(1000*result.baseline_absolute_error_m).toFixed(2)} mm from measured`;
    $('calibrationViews').textContent=`${result.training_pairs} train · ${result.holdout_pairs} holdout`;
    $('calibrationOutliers').textContent=`${result.outlier_capture_ids.length} robust outlier${result.outlier_capture_ids.length===1?'':'s'}`;
    $('calibrationAccuracyNote').textContent=result.accuracy_note;
    $('calibrationActivate').disabled=!passed||status.state==='active';
  }
  if(status.state==='active')$('calibrationActivationState').textContent=`Active preset ${status.bundle_id?.slice(0,16)}…`;
}

$('calibrationSetupForm').addEventListener('submit',async event=>{
  event.preventDefault();$('calibrationSetupError').textContent='';$('calibrationSetupState').textContent='Creating session…';
  try{
    const status=await calibrationOperation('/api/v1/calibration/session',calibrationConfiguration());
    localStorage.setItem(CALIBRATION_SESSION_KEY,status.session_id);renderCalibration(status);
    $('calibrationSetupState').textContent='Session created · raw synchronized capture is ready';
  }catch(error){$('calibrationSetupState').textContent='';$('calibrationSetupError').textContent=error.message;}
});

async function applyCalibrationPreview(){
  if(!encoding)await refreshEncoding();
  if(!encoding)throw new Error('Streaming configuration is unavailable');
  if(!calibrationOriginalEncoding){calibrationOriginalEncoding=structuredClone(encoding);$('calibrationRestorePreview').disabled=false;}
  const source=$('calibrationPreviewSide').value;
  const requested={...encoding,schema_version:1,mode:'both',codec:'h264',source,width:0,height:0,
    cadence_hz:Math.min(30,Math.max(10,Math.round(lastHealth?.capture_target_hz||15))),
    bitrate_bps:Math.max(4000000,encoding.bitrate_bps||0),keyframe_interval:1,threads:Math.max(2,encoding.threads||0),
    low_latency:true,preset:'ultrafast'};
  encoding=await (await api('/api/v1/encoding',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(requested)})).json();
  encodingFormDirty=false;showEncodingConfig(encoding);
  $('calibrationPreviewSideLabel').textContent=source==='left_color'?'Left color · ISP':'Right color · ISP';
  restartStreams();
}

$('calibrationEnablePreview').onclick=async()=>{
  $('calibrationError').textContent='';$('calibrationEnablePreview').disabled=true;$('calibrationEnablePreview').textContent='Starting preview…';
  try{await applyCalibrationPreview();$('calibrationEnablePreview').textContent='Positioning preview active';}
  catch(error){$('calibrationError').textContent=error.message;$('calibrationEnablePreview').textContent='Enable positioning preview';}
  finally{$('calibrationEnablePreview').disabled=false;}
};
$('calibrationPreviewSide').addEventListener('change',async()=>{
  if(!calibrationOriginalEncoding)return;
  try{await applyCalibrationPreview();}catch(error){$('calibrationError').textContent=error.message;}
});
$('calibrationRestorePreview').onclick=async()=>{
  if(!calibrationOriginalEncoding)return;
  try{
    encoding=await (await api('/api/v1/encoding',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(calibrationOriginalEncoding)})).json();
    calibrationOriginalEncoding=null;$('calibrationRestorePreview').disabled=true;$('calibrationEnablePreview').textContent='Enable positioning preview';
    showEncodingConfig(encoding);restartStreams();
  }catch(error){$('calibrationError').textContent=error.message;}
};
$('calibrationCapture').onclick=async()=>{
  if(!calibrationSession)return;
  $('calibrationError').textContent='';$('calibrationCapture').disabled=true;$('calibrationCaptureState').textContent='Capturing both raw ISP frames and checking target quality…';
  try{renderCalibration(await calibrationOperation('/api/v1/calibration/capture',{session_id:calibrationSession.session_id}));}
  catch(error){$('calibrationError').textContent=error.message;}
  finally{$('calibrationCapture').disabled=calibrationSession?.state==='solving'||calibrationSession?.state==='active';}
};
$('calibrationSolve').onclick=async()=>{
  if(!calibrationSession)return;
  $('calibrationError').textContent='';$('calibrationSolve').disabled=true;$('calibrationCaptureState').textContent='Solving robust intrinsics and stereo geometry; this can take a minute…';
  try{renderCalibration(await calibrationOperation('/api/v1/calibration/solve',{session_id:calibrationSession.session_id}));}
  catch(error){$('calibrationError').textContent=error.message;}
};
$('calibrationActivate').onclick=async()=>{
  if(!calibrationSession)return;
  $('calibrationActivationState').textContent='Activating immutable preset and restarting depth processing…';
  try{renderCalibration(await calibrationOperation('/api/v1/calibration/activate',{session_id:calibrationSession.session_id}));setTimeout(refreshHealth,2000);}
  catch(error){$('calibrationActivationState').textContent=error.message;}
};
$('calibrationRollback').onclick=async()=>{
  $('calibrationActivationState').textContent='Rolling back and restarting depth processing…';
  try{const result=await calibrationOperation('/api/v1/calibration/rollback',{});$('calibrationActivationState').textContent=`Restored ${result.active_bundle_id.slice(0,16)}…`;setTimeout(refreshHealth,2000);}
  catch(error){$('calibrationActivationState').textContent=error.message;}
};

async function restoreCalibrationSession(){
  const sessionId=localStorage.getItem(CALIBRATION_SESSION_KEY);
  if(!sessionId)return;
  try{
    const envelope=await (await api(`/api/v1/calibration?session=${encodeURIComponent(sessionId)}`)).json();
    if(envelope.status==='ok'&&envelope.result)renderCalibration(envelope.result);
  }catch(_){localStorage.removeItem(CALIBRATION_SESSION_KEY);}
}

$('copyDiagnostics').addEventListener('click',async()=>{
  try{await navigator.clipboard.writeText($('diagnosticJson').textContent);$('copyDiagnosticsState').textContent='Copied';}
  catch(error){$('copyDiagnosticsState').textContent=`Copy unavailable: ${error.message}`;}
});

async function restoreSession() {
  try {
    const session = await (await api('/api/v1/session')).json(); csrf = session.csrf;
    await refreshSetupStatus();
    if(session.needs_owner_password)$('ownerDialog').showModal();
    await Promise.all([refreshHealth(),refreshTransport(),refreshEncoding(),refreshStereo(),refreshExposure(),refreshCapabilities()]);
    if(lastHealth){
      if(!$('calibrationWidth').value)$('calibrationWidth').value=lastHealth.image_width||728;
      if(!$('calibrationHeight').value)$('calibrationHeight').value=lastHealth.image_height||544;
      $('calibrationRates').value=String(lastHealth.capture_target_hz||$('calibrationRates').value||60);
    }
    await restoreCalibrationSession();restartStreams();
  } catch (_) {}
}

// Performance sample model. Retain observations, never synthesize frame history.
function timingValue(value){return typeof value==='number'&&Number.isFinite(value)&&value>=0?value:null;}
function depthPublicationCount(health){return timingValue(health.matched_frames)??timingValue(health.published_frames);}
function recordPerformanceSample(model,health,now){
  const published=depthPublicationCount(health),run=health.run_uuid||'';
  if(model.run!==run||(published!==null&&model.published!==null&&published<model.published)||model.samples.at(-1)?.time>now){model.samples=[];model.published=null;model.sampledPublished=null;model.run=run;}
  const measured=health.matcher_statistics&&timingValue(health.matched_frames)!==null
    ?(published>0?{...health,...health.matcher_statistics}:{...health,processing_ms:null}):health;
  model.stale=false;model.latest=measured;
  model.samples=model.samples.filter(sample=>sample.time>=now-60000);
  const value=timingValue(measured.processing_ms);
  if(published!==null&&published>0&&published!==model.sampledPublished&&value!==null){model.samples.push({time:now,value});model.samples=model.samples.slice(-120);model.sampledPublished=published;}
  model.published=published;
}
function sampledP95(samples){if(!samples.length)return null;const values=samples.map(s=>s.value).sort((a,b)=>a-b);return values[Math.ceil(values.length*.95)-1];}
function performanceStages(health){
  const total=timingValue(health?.processing_ms);
  const stages=[['Rectification',health?.rectification_ms],['Stereo preparation',health?.stereo_preprocess_ms],['Stereo match',health?.stereo_match_ms],['Disparity filtering',health?.stereo_postprocess_ms]].map(([name,value])=>({name,value:timingValue(value)}));
  const accounted=stages.reduce((sum,s)=>sum+(s.value??0),0);
  stages.push({name:'Depth + overhead',value:total!==null&&stages.every(s=>s.value!==null)&&accounted<=total+.05?Math.max(0,total-accounted):null});
  return stages.map(s=>({...s,share:total>0&&s.value!==null?100*s.value/total:null})).sort((a,b)=>(b.value??-1)-(a.value??-1));
}

// Performance monitor rendering: one component, two deliberate destinations.
function placePerformanceMonitor(page){
  const monitor=$('performanceMonitor');
  const slot=page==='overview'?$('overviewPerformanceSlot'):page==='diagnostics'?$('diagnosticsPerformanceSlot'):null;
  monitor.hidden=!slot;
  if(slot){slot.append(monitor);renderPerformanceMonitor();}
}
function renderPerformanceMonitor(){
  const monitor=$('performanceMonitor');if(monitor.hidden)return;
  performanceModel.samples=performanceModel.samples.filter(s=>s.time>=Date.now()-60000);
  const latest=performanceModel.latest,total=timingValue(latest?.processing_ms),budget=1000/analysisTarget,delta=total===null?null:total-budget;
  setMeasurement('performanceLatest',total===null?null:total.toFixed(1),'ms');
  const p95=sampledP95(performanceModel.samples);setMeasurement('performanceP95',p95===null?null:p95.toFixed(1),'ms');
  setMeasurement('performanceDelta',delta===null?null:Math.abs(delta).toFixed(1),'ms');
  $('performanceDeltaLabel').textContent=delta===null?'Budget difference':delta>0?'Over budget':'Compute headroom';
  const status=$('performanceBudgetStatus');status.className='chip '+(delta===null?'chip-neutral':delta>0?'chip-amber':'chip-green');
  status.textContent=performanceModel.stale?(performanceModel.samples.length?'Disconnected · last known history':'Disconnected'):total===null?'Waiting for samples':delta>0?`Compute over budget · ${(total/budget).toFixed(1)}×`:'Compute within budget';
  $('performanceBudgetNote').textContent=`${budget.toFixed(budget%1?1:0)} ms compute budget · ${analysisTarget} fps analysis target`;
  const rows=$('performanceStages');rows.replaceChildren();
  if(latest){const stages=performanceStages(latest);stages.forEach((stage,index)=>{const row=document.createElement('tr');if(index===0&&stage.value!==null)row.className='largest-stage';[stage.name,stage.value===null?'—':stage.value.toFixed(1)+' ms',stage.share===null?'—':stage.share.toFixed(1)+'%'].forEach(text=>{const cell=document.createElement('td');cell.textContent=text;row.append(cell);});rows.append(row);});$('performanceBottleneck').textContent=stages[0].value===null?'Waiting for stage timings':`${stages[0].name} is the largest measured cost.`;}
  else {const row=document.createElement('tr'),cell=document.createElement('td');cell.colSpan=3;cell.textContent='Waiting for timings';row.append(cell);rows.append(row);$('performanceBottleneck').textContent='Waiting for stage timings';}
  drawPerformanceChart();
}
function drawPerformanceChart(){
  if($('performanceMonitor').hidden)return;
  const svg=$('performanceChart'),wrap=svg.parentElement,width=wrap.clientWidth;
  if(width<100)return;
  const height=205,left=50,right=8,top=25,bottom=33,budget=1000/analysisTarget,now=Date.now();
  const samples=performanceModel.samples.filter(s=>s.time>=now-60000),max=Math.ceil(Math.max(budget,...samples.map(s=>s.value),1)*1.1/50)*50;
  const x=time=>left+(time-(now-60000))/60000*(width-left-right),y=value=>top+(height-top-bottom)*(1-value/max);
  const ns='http://www.w3.org/2000/svg',add=(tag,attrs,text)=>{const e=document.createElementNS(ns,tag);Object.entries(attrs||{}).forEach(([k,v])=>e.setAttribute(k,String(v)));if(text!==undefined)e.textContent=text;svg.append(e);return e;};
  svg.replaceChildren();svg.setAttribute('viewBox',`0 0 ${width} ${height}`);$('performanceTooltip').hidden=true;
  add('title',{},`${samples.length} observed compute-time samples over 60 seconds. Frame budget ${budget.toFixed(1)} milliseconds.`);
  add('text',{x:left,y:12},'Compute (ms)');
  [0,max/3,max*2/3,max].forEach(v=>{add('line',{x1:left,x2:width-right,y1:y(v),y2:y(v),class:'chart-grid'});add('text',{x:left-8,y:y(v)+4,'text-anchor':'end'},Math.round(v));});
  [-60,-30,0].forEach(t=>add('text',{x:x(now+t*1000),y:height-15,'text-anchor':t===-60?'start':t===0?'end':'middle'},t===0?'Now':`${t} s`));
  add('text',{x:left+(width-left-right)/2,y:height-1,'text-anchor':'middle'},'Last 60 seconds');
  add('line',{x1:left,x2:width-right,y1:y(budget),y2:y(budget),class:'chart-budget'});
  if(samples.length){add('path',{d:samples.map((s,i)=>(i?'L':'M')+x(s.time).toFixed(2)+' '+y(s.value).toFixed(2)).join(' '),class:'chart-series'});const last=samples.at(-1);add('circle',{cx:x(last.time),cy:y(last.value),r:3,class:'chart-point'});}
  $('performanceEmpty').hidden=samples.length>0;
  if(!samples.length)return;
  const guide=add('line',{x1:left,x2:left,y1:top,y2:height-bottom,class:'chart-guide',visibility:'hidden'}),point=add('circle',{r:3,class:'chart-point',visibility:'hidden'});
  const overlay=add('rect',{x:left,y:top,width:width-left-right,height:height-top-bottom,fill:'transparent'});let pinned=false;
  const show=event=>{const bounds=svg.getBoundingClientRect(),position=Math.max(left,Math.min(width-right,(event.clientX-bounds.left)*width/bounds.width)),time=now-60000+(position-left)/(width-left-right)*60000;const sample=samples.reduce((a,b)=>Math.abs(a.time-time)<Math.abs(b.time-time)?a:b);guide.setAttribute('x1',x(sample.time));guide.setAttribute('x2',x(sample.time));guide.setAttribute('visibility','visible');point.setAttribute('cx',x(sample.time));point.setAttribute('cy',y(sample.value));point.setAttribute('visibility','visible');const tip=$('performanceTooltip');tip.textContent=`${Math.max(0,(now-sample.time)/1000).toFixed(1)} s ago · ${sample.value.toFixed(1)} ms · ${Math.abs(sample.value-budget).toFixed(1)} ms ${sample.value>budget?'over':'under'} budget`;tip.hidden=false;tip.style.left=Math.max(0,Math.min(width-tip.offsetWidth,position-90))+'px';tip.style.top=Math.max(20,y(sample.value)-40)+'px';};
  overlay.addEventListener('pointermove',e=>{if(!pinned)show(e);});overlay.addEventListener('click',e=>{pinned=!pinned;show(e);});overlay.addEventListener('pointerleave',()=>{if(!pinned){$('performanceTooltip').hidden=true;guide.setAttribute('visibility','hidden');point.setAttribute('visibility','hidden');}});
}
function updateAppliedSummaries(){
  document.querySelectorAll('[data-capture-summary]').forEach(e=>e.textContent=exposure?exposure.mode==='auto'?'Auto exposure':`${Number(exposure.manual_exposure_us).toLocaleString()} µs · ${exposure.manual_analogue_gain}×`:'Loading');
  document.querySelectorAll('[data-matcher-summary]').forEach(e=>e.textContent=stereo?`${stereo.algorithm==='bm'?'StereoBM':'SGBM'} · ${stereo.search_range} px · ${stereo.block_size} px block`:'Loading');
}
function updateLiveMetrics(health){
  document.querySelectorAll('[data-live-metric]').forEach(e=>{const key=e.dataset.liveMetric;e.textContent=key==='capture'?`Capture target · ${health?.capture_target_hz??'—'} fps`:key==='depth'?`Depth output · ${depthOutputRate===null?'—':depthOutputRate.toFixed(1)} fps`:`Compute · ${health?.processing_ms==null?'—':Number(health.processing_ms).toFixed(1)} ms`;});
  document.querySelectorAll('[data-live-resolution]').forEach(e=>e.textContent=`${health?.depth_width??'—'} × ${health?.depth_height??'—'} depth grid`);
  document.querySelectorAll('[data-live-calibration]').forEach(e=>{const accepted=Number(health?.calibration_authority)>0&&!!health?.calibration_sha256;e.classList.toggle('accepted',accepted);e.textContent=!health?'Waiting for calibration status':accepted?'Calibration validation passed':'Metric depth is engineering-only until per-unit validation passes.';});
}

// Settings retain their edit base while device polling continues.
function fieldValue(control){return control.type==='checkbox'?control.checked:control.value;}
function setFormBaseline(id){
  const c=formControllers.get(id);if(!c)return;
  c.baseline=new Map(c.controls.map(input=>[input.id,fieldValue(input)]));c.loaded=true;c.attempted=false;c.errors=new Map();
  if(!c.busy){c.controls.forEach(input=>input.disabled=false);c.updateMode();}
  if(restoredDraftForms.has(id))window.NuvoDeviceSettings.clearDraft(window.NuvoDeviceSettings.DEFINITIONS[{algorithmForm:'algorithm',encodingForm:'encoding',exposureForm:'exposure'}[id]].path);
  if(!restoredDraftForms.has(id)){
    restoredDraftForms.add(id);const kind={algorithmForm:'algorithm',encodingForm:'encoding',exposureForm:'exposure'}[id],saved=window.NuvoDeviceSettings.loadDraft(kind);
    if(saved){for(const input of c.controls)if(Object.hasOwn(saved.fields,input.id)){if(input.type==='checkbox')input.checked=saved.fields[input.id];else input.value=saved.fields[input.id];}
      configurationRevisions.set(window.NuvoDeviceSettings.DEFINITIONS[kind].path,saved.revision);
      c.form.dispatchEvent(new Event('input',{bubbles:true}));c.updateMode();
    }
  }
  updateFormUi(c);
}
function setFormBusy(id,busy){
  const c=formControllers.get(id);if(!c)return;c.busy=busy;c.form.setAttribute('aria-busy',String(busy));
  if(busy)c.controls.forEach(input=>input.disabled=true);
  else {c.controls.forEach(input=>input.disabled=!c.loaded);if(c.loaded)c.updateMode();}
  updateFormUi(c);
}
function changedControls(c,root=c.form){return [...root.querySelectorAll('input:not([type="hidden"]),select')].filter(i=>c.baseline.has(i.id)&&c.baseline.get(i.id)!==fieldValue(i));}
function toneSaveStatus(status){
  const text=status.textContent;
  status.className=/rejected/.test(text)?'chip chip-red':/^Applied|commands reloaded|preview available/.test(text)?'chip chip-green':/^Saved|Applying/.test(text)?'chip chip-blue':/pending change|Device changed/.test(text)?'chip chip-amber':'';
}
function fieldError(control,message){
  const label=control.closest('label');let error=label.querySelector('.field-error');
  if(!error){error=document.createElement('p');error.className='field-error';error.id=control.id+'Error';error.setAttribute('role','alert');label.append(error);control.setAttribute('aria-describedby',((control.getAttribute('aria-describedby')||'')+' '+error.id).trim());}
  error.hidden=!message;error.textContent=message||'';if(message)control.setAttribute('aria-invalid','true');else control.removeAttribute('aria-invalid');
}
function validateSettings(c){
  const errors=new Map();c.controls.filter(i=>!i.disabled).forEach(i=>{if(!i.validity.valid)errors.set(i.id,i.validationMessage);});
  const number=id=>Number($(id).value),issue=(id,text)=>errors.set(id,text);
  if(c.form.id==='algorithmForm'){
    if(number('minDisparity')>=number('maxDisparity'))issue('maxDisparity','Maximum must be greater than minimum disparity.');
    else if(number('maxDisparity')>number('minDisparity')+number('searchRange'))issue('maxDisparity','Maximum must lie inside the search range.');
  }else if(c.form.id==='exposureForm'){
    if(number('autoMinMedian')>number('autoTargetMedian')||number('autoTargetMedian')>number('autoMaxMedian'))issue('autoTargetMedian','Target median must be inside the Auto deadband.');
    if(number('autoMinExposure')>number('autoMaxExposure'))issue('autoMaxExposure','Maximum must be at least the minimum exposure.');
    if(number('autoMinGain')>number('autoMaxGain'))issue('autoMaxGain','Maximum must be at least the minimum gain.');
  }else if(c.form.id==='encodingForm'){
    if(!$('streamEncoderEnabled').checked&&!$('streamRawEnabled').checked)issue('streamRawEnabled','Keep raw delivery or the monitoring encoder enabled.');
    if((number('streamWidth')===0)!==(number('streamHeight')===0))issue(number('streamWidth')===0?'streamWidth':'streamHeight','Set both dimensions, or leave both at 0 for native resolution.');
  }
  c.errors=errors;return !errors.size;
}
function updateFormUi(c,edited=false){
  if(c.attempted&&!c.busy)validateSettings(c);
  c.controls.forEach(i=>fieldError(i,c.errors.get(i.id)));
  const changed=changedControls(c).length;
  const status=$(c.status);c.form.querySelector('button[type="submit"]').disabled=!c.loaded||!changed||c.busy;$(c.reset).disabled=!c.loaded||!changed||c.busy;
  if(changed&&!c.busy){status.textContent=changed+' pending '+(changed===1?'change':'changes');status.className='chip chip-amber';}
  else {status.className='';if(edited||!status.textContent||/pending change|Unsaved changes|Loading device settings/.test(status.textContent))status.textContent=c.loaded?'No pending changes':'Loading device settings…';}
  toneSaveStatus(status);
  c.form.querySelectorAll('.advanced-group').forEach(group=>{
    const own=[...group.querySelectorAll('input,select')],values=[];
    (group.dataset.summaryFields||'').split(',').filter(Boolean).forEach(item=>{const [id,label,unit]=item.split(':'),input=$(id);if(!input||input.disabled||input.closest('label').hidden)return;const value=input.tagName==='SELECT'?input.selectedOptions[0]?.textContent.split(' · ')[0]:input.value===''?'—':Number(input.value).toLocaleString(undefined,{maximumFractionDigits:4});values.push(`${label} ${value}${unit?' '+unit:''}`);});
    group.querySelector('.group-values').textContent=values.length?values.join(' · '):group.classList.contains('auto-advanced')?'Available in Auto exposure mode':'Available when enabled';
    const badges=group.querySelector('.group-badges');badges.replaceChildren();
    const chip=(text,tone)=>{const e=document.createElement('span');e.className='chip chip-'+tone;e.textContent=text;badges.append(e);};
    const count=changedControls(c,group).length,errors=own.filter(i=>c.errors.has(i.id)).length;
    if(group.classList.contains('auto-advanced')&&$('exposureMode').value!=='auto')chip('Auto only','neutral');if(count)chip(count+' pending','amber');if(errors)chip(errors+' '+(errors===1?'error':'errors'),'red');
    group.querySelector('.group-reset').disabled=!count||c.busy;
  });
  c.form.querySelectorAll('.advanced-card').forEach(card=>{
    const summary=card.querySelector(':scope > summary');summary.querySelector('.group-values').textContent=card.querySelectorAll('.advanced-group').length+' groups · closing preserves your changes';
    const badges=summary.querySelector('.group-badges');badges.replaceChildren();const pending=changedControls(c,card).length,errors=[...card.querySelectorAll('input,select')].filter(i=>c.errors.has(i.id)).length;
    [[pending,'pending','amber'],[errors,'errors','red']].filter(([count])=>count).forEach(([count,text,tone])=>{const chip=document.createElement('span');chip.className='chip chip-'+tone;chip.textContent=count+' '+text;badges.append(chip);});
  });
}
function initializeSettingsUi(){
  document.querySelectorAll('.settings-form label').forEach(label=>{
    const input=label.querySelector('input:not([type="hidden"]),select');if(!input||input.type==='checkbox')return;
    const title=document.createElement('span');title.className='field-label';title.id=input.id+'Label';
    title.textContent=[...label.childNodes].filter(n=>n.nodeType===3).map(n=>n.textContent.trim()).filter(Boolean).join(' ');
    [...label.childNodes].filter(n=>n.nodeType===3).forEach(n=>n.remove());label.prepend(title);input.setAttribute('aria-labelledby',title.id);
    const help=document.createElement('span');help.className='field-help';help.id=input.id+'Help';label.querySelectorAll(':scope > small').forEach(small=>help.append(small));
    if(input.dataset.unit){const wrapper=document.createElement('span');wrapper.className='control-with-unit';input.replaceWith(wrapper);wrapper.append(input);const unit=document.createElement('span');unit.className='control-unit';unit.id=input.id+'Unit';unit.textContent=input.dataset.unit;wrapper.append(unit);input.setAttribute('aria-describedby',unit.id);}
    if(help.textContent){label.append(help);input.setAttribute('aria-describedby',((input.getAttribute('aria-describedby')||'')+' '+help.id).trim());}
  });
  $('calibrationSetupForm').addEventListener('invalid',event=>{
    const control=event.target;let parent=control.parentElement;
    while(parent&&parent!==event.currentTarget){if(parent.tagName==='DETAILS')parent.open=true;parent=parent.parentElement;}
    fieldError(control,control.validationMessage);
  },true);
  $('calibrationSetupForm').addEventListener('input',event=>{if(event.target.hasAttribute('aria-invalid'))fieldError(event.target,event.target.validity.valid?'':event.target.validationMessage);});
  const definitions=[['algorithmForm','algorithmSaveState','algorithmReset',updateAlgorithmFields],['exposureForm','exposureSaveState','exposureReset',updateExposureFields],['encodingForm','encodingSaveState','encodingReset',updateEncodingFields]];
  definitions.forEach(([id,status,reset,updateMode])=>{
    const form=$(id),c={form,status,reset,updateMode,controls:[...form.querySelectorAll('input:not([type="hidden"]),select')],baseline:new Map(),errors:new Map(),attempted:false,loaded:false,busy:false};formControllers.set(id,c);form.noValidate=true;
    const edit=()=>{if(!c.loaded)return;if(id==='algorithmForm')algorithmFormDirty=changedControls(c).length>0;if(id==='exposureForm')exposureFormDirty=changedControls(c).length>0;if(id==='encodingForm')encodingFormDirty=changedControls(c).length>0;const kind={algorithmForm:'algorithm',encodingForm:'encoding',exposureForm:'exposure'}[id],path=window.NuvoDeviceSettings.DEFINITIONS[kind].path;if(changedControls(c).length)window.NuvoDeviceSettings.saveDraft(path,Object.fromEntries(c.controls.map(input=>[input.id,fieldValue(input)])),configurationRevisions.get(path));else window.NuvoDeviceSettings.clearDraft(path);updateMode();updateFormUi(c,true);};
    form.addEventListener('input',edit);form.addEventListener('change',edit);
    form.addEventListener('submit',event=>{c.attempted=true;if(c.busy||!c.loaded||!validateSettings(c)){event.preventDefault();event.stopImmediatePropagation();updateFormUi(c);c.controls.filter(i=>c.errors.has(i.id)).forEach(input=>{let parent=input.parentElement;while(parent&&parent!==form){if(parent.tagName==='DETAILS')parent.open=true;parent=parent.parentElement;}});c.controls.find(i=>c.errors.has(i.id))?.focus();}},true);
    form.querySelectorAll('.group-reset').forEach(button=>button.addEventListener('click',()=>{button.closest('.advanced-group').querySelectorAll('input,select').forEach(input=>{const value=c.baseline.get(input.id);if(value!==undefined){if(input.type==='checkbox')input.checked=value;else input.value=value;}});edit();}));
    const context={ 'matcher-validity':'Choose which pixels are accepted as valid depth.','matcher-filter':'Remove isolated regions and smooth disparity.','matcher-compute':'Balance matching quality and processing cost.','capture-limits':'Keep automatic adjustments inside a defined range.','capture-response':'Control brightness deadband and update cadence.','capture-balance':'Colour gains are separate from analogue brightness gain.','stream-size':'Use 0 for both dimensions to retain native resolution.','stream-compression':'Only the controls for the selected codec are active.'};
    form.querySelectorAll('.group-context').forEach(e=>e.textContent=context[e.closest('.advanced-group').dataset.group]||'');
    c.controls.forEach(i=>i.disabled=true);updateFormUi(c);
  });
  const template=$('tuningPreviewTemplate');document.querySelectorAll('[data-preview-slot]').forEach(slot=>slot.prepend(template.content.cloneNode(true)));
  $('performanceTarget').addEventListener('change',event=>{analysisTarget=Number(event.target.value);renderPerformanceMonitor();});
  if('ResizeObserver' in window)new ResizeObserver(drawPerformanceChart).observe($('performanceChart').parentElement);
}
initializeSettingsUi();
let requestedGroup=null;
try{requestedGroup=sessionStorage.getItem('nuvo-open-settings-group');sessionStorage.removeItem('nuvo-open-settings-group');}catch(_){}
if(requestedGroup){const group=[...document.querySelectorAll('[data-group]')].find(element=>element.dataset.group===requestedGroup);if(group){group.open=true;const parent=group.closest('.advanced-card');if(parent)parent.open=true;}}
initializeOutputControls();
document.addEventListener('visibilitychange',restartStreams);
window.addEventListener('pageshow',event=>{if(event.persisted)restoreSession();});
window.addEventListener('pagehide',()=>{rawSocket?.close();encodedSocket?.close();});
updateAlgorithmFields();
updateExposureFields();
updateEncodingFields();
restoreSession();
setInterval(()=>{if(!document.hidden)refreshHealth();},2000);
setInterval(()=>{if(!document.hidden){refreshTransport();refreshEncoding();refreshStereo();refreshExposure();refreshCapabilities();}},5000);
document.addEventListener('visibilitychange',()=>{if(!document.hidden){refreshHealth();refreshTransport();refreshEncoding();refreshStereo();refreshExposure();refreshCapabilities();}});
setInterval(()=>{if(lastFrameAt){const age=(Date.now()-lastFrameAt)/1000;$('age').textContent=age<1?'Updated now':`${age.toFixed(1)} s old`;$('age').classList.toggle('error',age>2);if(age>2)document.querySelectorAll('.page.active [data-preview-state]').forEach(label=>{label.textContent=`Preview stale · ${age.toFixed(1)} s`;label.className='chip chip-amber';});}if(encodedFrameAt){const age=(Date.now()-encodedFrameAt)/1000;$('encodedAge').textContent=age<1?'Updated now':`${age.toFixed(1)} s old`;$('encodedAge').classList.toggle('error',age>2)}},250);
