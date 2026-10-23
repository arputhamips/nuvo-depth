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
let lastSequence = 0, lastIdentity = '', lastFrameAt = 0;
let lastHealthRun = '', lastHealthPublished = null, lastHealthAt = 0;
let rawPolling = false, rawSocket = null, encoding = null;
let encodedPolling = false, encodedSocket = null, encodedFrameAt = 0;
let decoder = null, decoderCodec = '', encodedLastSequence = 0;
let encodedWaitingForKeyframe = true;
let encodedGeneration = 0, encodedArrival = 0, encodedLastPresented = 0;
let jpegDecodeBusy = false, pendingJpeg = null;
const encodedMetadata = new Map();
let rawRetryTimer = null, encodedRetryTimer = null;
let rawRetryDelay = 1000, encodedRetryDelay = 1000;
let rawPollToken = 0, encodedPollToken = 0;
let encodingFormDirty = false, encodingPreviewPending = false;
let stereo = null, algorithmFormDirty = false, activeStereoBackend = '';
let exposure = null, exposureFormDirty = false, exposurePending = null;
let csrf = '';
const configurationRevisions = new Map(), latestConfigurationRevisions = new Map();
let selectedPage = '';
let lastHealth = null, calibrationSession = null, calibrationOriginalEncoding = null;
let firstDepth = {};
let calibrationReadiness = null;
let supportReport = null;
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
  ['disparity','Disparity · float32 matching-grid pixels',4], ['depth','Depth · float32 metres (optical-axis Z)',4],
  ['confidence','Matcher support · binary 0 / 255',1], ['validity','Depth validity · 1 means valid',1]
];
let capabilities = null;
let outputSettings = window.NuvoDeviceSettings.outputRecipe('camera');
let outputReception = null, viewerPaused = false, displayedNumericFrame = null;
let receivingTimes = [], initialEncodingLoaded = false;
let wifiDirty=false, transportState=null;
let pictureQualityDirty=false, pictureQualityBusy=false;
try {
  const saved=JSON.parse(localStorage.getItem('nuvo-output-selection')||'null');
  if(saved&&Array.isArray(saved.products)&&saved.products.length<=14&&saved.products.every(p=>OUTPUT_PRODUCTS.some(x=>x[0]===p))&&
    saved.cadence_hz>0&&saved.cadence_hz<=60&&saved.preview_cadence_hz>0&&saved.preview_cadence_hz<=60&&
    ['auto','best_effort','exact'].includes(saved.delivery)){
    outputSettings=window.NuvoDeviceSettings.normalizeOutputSelection({...outputSettings,...saved});
    if(outputSettings.enabled)outputSettings.preview=false;
  }
} catch (_) {}

function outputSubscriptionPath(path, settings=outputSettings) {
  return `${path}?cadence_hz=${settings.cadence_hz}&products=${settings.products.join(',')}&delivery=${settings.delivery}`;
}
function outputBandwidth(products,width,height,cadence,matchingWidth=width,matchingHeight=height) {
  const bytes=window.NuvoDeviceSettings.outputFrameBytes(products,{width,height,matchingWidth,matchingHeight},OUTPUT_PRODUCTS);
  return {frame_bytes:bytes, bytes_per_second:bytes*cadence};
}
function outputDraft(){
  const picture=$('outputFormat').value==='picture';
  return window.NuvoDeviceSettings.normalizeOutputSelection({enabled:!picture,
    products:picture?[]:[...document.querySelectorAll('[name="outputProduct"]:checked')].map(x=>x.value),
    cadence_hz:Number($('outputCadence').value),delivery:$('outputDelivery').value,
    preview:picture,preview_cadence_hz:Number($('outputCadence').value)});
}
function fillOutputDraft(value){
  const name=window.NuvoDeviceSettings.outputRecipeName(value),task=name==='depth_reference'?'depth':name==='picture'?'camera':name;
  document.querySelectorAll('[name="outputProduct"]').forEach(input=>input.checked=value.products.includes(input.value));
  document.querySelectorAll('[name="outputTask"]').forEach(input=>input.checked=input.value===task);
  $('outputPreset').value=name==='picture'?'camera':name;
  $('outputCadence').value=value.enabled?value.cadence_hz:value.preview_cadence_hz;
  $('outputDelivery').value=value.delivery;$('outputFormat').value=value.enabled?'data':'picture';
  $('outputCameraReference').checked=name==='depth_reference';
  $('outputAdvanced').open=name==='custom';
}
function updateOutputBudget() {
  const draft=outputDraft(),products=draft.products,cadence=draft.cadence_hz,connection=capabilities?.connection;
  const width=lastHealth?.image_width||728,height=lastHealth?.image_height||544;
  const estimate=outputBandwidth(products,width,height,cadence,lastHealth?.depth_width||width,lastHealth?.depth_height||height);
  const budget=connection?.resource_budget_bytes_per_second,over=draft.enabled&&budget&&estimate.bytes_per_second>budget;
  const exactUnavailable=draft.enabled&&draft.delivery==='exact'&&connection&&!connection.qualified_link;
  const name=window.NuvoDeviceSettings.outputRecipeName(draft),app=$('outputDestination').value==='app';
  $('outputFormatLabel').hidden=!['camera','picture','custom'].includes(name);
  $('outputReferenceLabel').hidden=!['depth','depth_reference'].includes(name);
  $('outputValidityNote').hidden=!products.includes('depth');
  $('outputBandwidth').textContent=draft.enabled?`${(estimate.bytes_per_second/1048576).toFixed(2)} MiB/s estimate · image grid ${width} × ${height} · depth grid ${lastHealth?.depth_width||width} × ${lastHealth?.depth_height||height}`:'Compressed size varies with the scene and device encoder settings.';
  const pictureSource=encoding?`${encoding.source.replaceAll('_',' ')} · ${encoding.codec.toUpperCase()}`:'source not loaded';
  $('outputSelectionSummary').textContent=draft.enabled?`${window.NuvoDeviceSettings.outputSelectionLabel(draft)} · ${cadence} frames/s${products.includes('depth')?' · numeric metres':''}`:`Shared picture: ${pictureSource} · up to ${cadence} frames/s`;
  $('outputScope').textContent=app?'Connection code only. Generating a recipe does not change this browser, the device encoder or another application.':draft.preview?'This browser requests the existing shared picture. It does not change its source, quality or other clients.':'Only this browser subscription changes. Other clients keep their own selections.';
  $('outputApply').textContent=app?'Show connection code':'Apply to this browser';
  const state=window.NuvoDeviceSettings.outputSelectionStatus(outputSettings,draft,outputReception);
  $('outputPending').textContent=state.dirty?'Editing · changes are not requested yet':app?'Ready to generate application code':'No pending browser changes';
  $('outputDiscard').disabled=!state.dirty;
  $('outputRequested').textContent=`Requested in this browser: ${state.requested} · ${outputSettings.enabled?outputSettings.cadence_hz:outputSettings.preview_cadence_hz} frames/s`;
  $('outputReceiving').textContent=receivingStatus();
  $('outputWarning').textContent=exactUnavailable?'Exact delivery is not qualified on this connection. Choose Best effort or a qualified wired connection.':over?'This selection exceeds the camera connection budget. Reduce the requested rate or choose fewer products.':draft.preview&&encoding?.mode==='raw'?'The shared picture encoder is off. Enable it explicitly in Device → Advanced preview encoder.':draft.enabled&&encoding?.mode==='encoded'?'The device currently serves pictures only. Enable Uncompressed network data in Device to receive these products.':'';
}
function outputLimits(){return {width:lastHealth?.image_width||lastHealth?.depth_width||728,height:lastHealth?.image_height||lastHealth?.depth_height||544,
  matchingWidth:lastHealth?.depth_width,matchingHeight:lastHealth?.depth_height,
  supportedProducts:capabilities?.products,qualified:!!capabilities?.connection?.qualified_link,bytesPerSecond:capabilities?.connection?.resource_budget_bytes_per_second,
  maxFrameBytes:capabilities?.stream_admission?.raw_maximum_wire_bytes,rawEnabled:encoding?.mode!=='encoded'};}
function commitOutputSelection(candidate){
  const normalized=window.NuvoDeviceSettings.normalizeOutputSelection(candidate);
  if(normalized.preview&&encoding?.mode==='raw')throw new Error('The shared picture encoder is off. Enable it explicitly in Device → Advanced preview encoder.');
  const checked=window.NuvoDeviceSettings.validateOutputSelection(normalized,outputLimits(),OUTPUT_PRODUCTS);
  const next={...normalized,enabled:checked.enabled,products:checked.products};
  let persisted=true;try{localStorage.setItem('nuvo-output-selection',JSON.stringify(next));}catch(_){persisted=false;}
  outputSettings=next;viewerPaused=false;outputReception=null;
  fillOutputDraft(next);
  $('liveRecipe').value=window.NuvoDeviceSettings.outputRecipeName(next);
  $('liveCadence').value=next.enabled?next.cadence_hz:next.preview_cadence_hz;
  $('outputSaveState').textContent=persisted?'Requested · waiting for a matching frame':'Requested for this session · waiting for a matching frame';
  restartStreams();updateOutputBudget();
  return persisted;
}
function initializeOutputControls() {
  const groups=[['Camera images',OUTPUT_PRODUCTS.slice(0,10)],['Measurements and masks',OUTPUT_PRODUCTS.slice(10)]];
  for(const [title,products] of groups){
    const group=document.createElement('fieldset'),legend=document.createElement('legend');legend.textContent=title;group.append(legend);
    for(const [name,label] of products){
      const item=document.createElement('label'),input=document.createElement('input');input.type='checkbox';input.name='outputProduct';input.value=name;
      item.append(input,document.createTextNode(label));group.append(item);
    }
    $('outputProductChoices').append(group);
  }
  fillOutputDraft(outputSettings);$('liveRecipe').value=window.NuvoDeviceSettings.outputRecipeName(outputSettings);
  $('liveCadence').value=outputSettings.enabled?outputSettings.cadence_hz:outputSettings.preview_cadence_hz;
  const choose=name=>{fillOutputDraft(window.NuvoDeviceSettings.outputRecipe(name));$('outputSaveState').textContent='';$('outputRecipe').hidden=true;updateOutputBudget();};
  document.querySelectorAll('[name="outputTask"]').forEach(input=>input.addEventListener('change',()=>choose(input.value)));
  $('outputPreset').addEventListener('change',()=>{if($('outputPreset').value!=='custom')choose($('outputPreset').value);else $('outputAdvanced').open=true;});
  $('outputCameraReference').addEventListener('change',()=>choose($('outputCameraReference').checked?'depth_reference':'depth'));
  $('outputFormat').addEventListener('change',()=>choose($('outputFormat').value==='picture'?'picture':'camera'));
  $('outputProductChoices').addEventListener('change',()=>{
    const draft=outputDraft();fillOutputDraft({...draft,enabled:true,preview:false});$('outputAdvanced').open=true;updateOutputBudget();
  });
  $('outputForm').addEventListener('input',()=>{$('outputSaveState').textContent='';$('outputRecipe').hidden=true;updateOutputBudget();});
  $('outputForm').addEventListener('change',()=>{$('outputRecipe').hidden=true;updateOutputBudget();});
  $('outputDiscard').addEventListener('click',()=>{fillOutputDraft(outputSettings);$('outputRecipe').hidden=true;$('outputSaveState').textContent='Draft discarded';updateOutputBudget();});
  $('outputForm').addEventListener('submit',event=>{
    event.preventDefault();
    try{
      const candidate=outputDraft();
      if($('outputDestination').value==='app'){
        window.NuvoDeviceSettings.validateOutputSelection(candidate,{...outputLimits(),rawEnabled:true,qualified:true,bytesPerSecond:Number.MAX_SAFE_INTEGER},OUTPUT_PRODUCTS);
        $('outputRecipe').textContent=window.NuvoDeviceSettings.outputApplicationRecipe(candidate);$('outputRecipe').hidden=false;
        $('outputSaveState').textContent='Connection code ready · browser selection unchanged';
      }else commitOutputSelection(candidate);
    }catch(error){$('outputWarning').textContent=error.message;}
  });
  $('liveRecipe').addEventListener('change',()=>{const name=$('liveRecipe').value;if(name!=='custom')$('liveCadence').value=window.NuvoDeviceSettings.outputRecipe(name).cadence_hz;});
  $('liveSelectionForm').addEventListener('submit',event=>{
    event.preventDefault();try{const name=$('liveRecipe').value,rate=Number($('liveCadence').value);commitOutputSelection({...(name==='custom'?outputSettings:window.NuvoDeviceSettings.outputRecipe(name)),cadence_hz:rate,preview_cadence_hz:rate});}catch(error){$('frameError').textContent=error.message;}
  });
  $('viewerPause').addEventListener('click',()=>{viewerPaused=!viewerPaused;restartStreams();});
  updateOutputBudget();
}
async function refreshCapabilities() {
  try {
    const before=capabilities?.connection?.products_permitted;
    capabilities=await (await api('/api/v1/capabilities')).json();
    const c=capabilities.connection;
    $('outputConnection').textContent=`Camera connection: ${c.kind.replaceAll('_',' ')} · ${c.interface}`;
    $('outputRecommendation').textContent=`${(c.resource_budget_bytes_per_second/1048576).toFixed(1)} MiB/s ${c.budget_is_measured_capacity?'measured capacity':'resource budget'}. Wi-Fi is best effort. USB means USB Ethernet. This identifies the camera-side link, not every segment to this browser.`;
    updateOutputBudget();
    if(typeof before==='boolean'&&before!==c.products_permitted)restartStreams();
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

const LEGACY_PAGES={overview:'device',exposure:'viewer',presets:'viewer',algorithm:'viewer',streaming:'device',calibration:'device',diagnostics:'device',recovery:'device'};
function openPage(id) {
  const section=LEGACY_PAGES[id]?$(id):null;
  const page=LEGACY_PAGES[id]||(['viewer','network','device'].includes(id)?id:'viewer');
  if(section)section.open=true;
  const changed=page!==selectedPage;selectedPage=page;
  document.querySelectorAll('.page').forEach(x=>x.classList.toggle('active',x.id===page));
  document.querySelectorAll('nav [data-page]').forEach(x=>{
    const active=x.dataset.page===page;x.classList.toggle('active',active);
    if(active)x.setAttribute('aria-current','page');else x.removeAttribute('aria-current');
  });
  placePerformanceMonitor(page);
  if(changed)restartStreams();
  if(section)requestAnimationFrame(()=>section.scrollIntoView({block:'start'}));
  else {window.scrollTo(0,0);requestAnimationFrame(()=>window.scrollTo(0,0));}
  if(page==='device')queueMicrotask(refreshDiagnostics);
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

let sessionBootstrap = null;
async function connectSession() {
  if(!sessionBootstrap)sessionBootstrap=(async()=>{
    const response=await fetch('/api/v1/session',{credentials:'same-origin',cache:'no-store'});
    if(!response.ok)throw new Error('Device connection unavailable');
    const session=await response.json();
    if(!/^[a-f0-9]{64}$/.test(session.csrf||''))throw new Error('Device session is invalid');
    csrf=session.csrf;
  })().finally(()=>{sessionBootstrap=null;});
  return sessionBootstrap;
}
async function api(path,options={}) {
  if(!csrf)await connectSession();
  const previous=csrf;
  const request=()=>window.NuvoDeviceSettings.request(path,options,{csrf,configurationRevisions,latestConfigurationRevisions,
    encodingFormDirty,algorithmFormDirty,exposureFormDirty,safeExposureUs:Number(lastHealth?.exposure_safe_max_us)||8000});
  try{return await request();}
  catch(error){
    if(error.status!==401)throw error;
    // A rejected request has not run. Refresh once, sharing concurrent recovery
    // and preserving the form's original configuration revision on a retry.
    if(csrf===previous)await connectSession();
    return request();
  }
}

async function refreshSetupStatus(){
  try {
    const payload=await(await api('/api/v1/setup/status')).json(),state=payload.result;
    if(!state?.schema_version)return;
    calibrationReadiness=state.calibration_readiness||{state:'unavailable',label:'Status unavailable/error',message:'This device does not expose shared calibration readiness.',next_action:'Inspect device status and the installed software version.'};
    renderCalibrationReadiness();
    const note=$('setupState');note.hidden=false;
    note.textContent=`${state.display_name||'Nuvo'} · ${state.phase||'Setup pending'}`+
      (state.reboot_required?' · Camera changes require a reboot':'')+
      ` · ${calibrationReadiness.label}`+
      (state.capture_error?` · ${state.capture_error}`:'')+
      (state.error?` · ${state.error}`:'');
    return true;
  }catch(_){
    calibrationReadiness={state:'unavailable',label:'Status unavailable/error',message:'Cannot read calibration readiness from the device.',next_action:'Reconnect and inspect setup and capture services.'};
    renderCalibrationReadiness();return false;
  }
}

function renderCalibrationReadiness(){
  const ready=calibrationReadiness||{state:'unavailable',label:'Status unavailable/error',message:'Waiting for shared calibration status.',next_action:'Wait for the device status response.'};
  const tone=ready.state==='calibration_accepted'?'green':ready.state==='unavailable'?'red':'amber';
  $('calibrationState').textContent=ready.label;$('calibrationState').className=`chip chip-${tone}`;
  $('calibrationDetail').textContent=ready.message;
  $('calibrationNotice').textContent=`${ready.label}. ${ready.message} Next: ${ready.next_action}`;
  $('calibrationBanner').hidden=ready.state==='calibration_accepted';
  $('calibrationReadinessTitle').textContent=ready.label;
  $('calibrationReadinessMessage').textContent=ready.message;
  $('calibrationReadinessAction').textContent=ready.next_action;
}

async function refreshTransport() {
  try {
    const state = await (await api('/api/v1/transport')).json();
    transportState=state;
    $('transportState').textContent = `Current preference: ${state.preference}`;
    $('transportError').textContent = '';
    const supported=typeof state.wifi_enabled==='boolean';
    if(!wifiDirty)$('wifiEnabled').checked=state.wifi_enabled===true;
    $('wifiEnabled').disabled=!supported;$('wifiApply').disabled=!supported||!wifiDirty;
    const pending=state.transaction?.status==='pending',expired=state.transaction?.status==='expired';
    $('wifiConfirm').hidden=!pending;$('wifiRollback').hidden=!(pending||expired);
    $('wifiApply').disabled=!supported||!wifiDirty||pending;
    $('wifiState').textContent=!supported?'Separate Wi-Fi permission requires an updated gateway.':pending?'A device transport change is provisional. Confirm it or undo; it rolls back after 60 seconds.':expired?'The pending transport change expired. Undo it or refresh the restored policy.':`Wi-Fi permission: ${state.wifi_enabled?'allowed':'blocked'}${state.preference==='off'?' · master Off still blocks all product streaming':''}.${state.transaction===null?' Pending-change status unavailable.':''}`;
  } catch (error) { $('transportState').textContent = 'Connection preference unavailable'; $('transportError').textContent = error.message; }
}

$('wifiEnabled').addEventListener('change',()=>{wifiDirty=$('wifiEnabled').checked!==transportState?.wifi_enabled;$('wifiApply').disabled=!wifiDirty||transportState?.transaction?.status==='pending';$('wifiState').textContent=wifiDirty?'Editing Wi-Fi permission · not applied':'No pending permission changes';});
async function wifiAction(operation){
  $('wifiError').textContent='';$('wifiApply').disabled=true;
  try{
    await api('/api/v1/transport/'+operation,{method:'POST',headers:{'Content-Type':'application/json'},body:operation==='stage'?JSON.stringify({wifi_enabled:$('wifiEnabled').checked}):''});
    wifiDirty=false;
    await refreshTransport();await refreshCapabilities();
  }catch(error){$('wifiError').textContent=error.message;$('wifiApply').disabled=!wifiDirty;}
}
$('wifiApply').addEventListener('click',()=>wifiAction('stage'));
$('wifiConfirm').addEventListener('click',()=>wifiAction('confirm'));
$('wifiRollback').addEventListener('click',()=>wifiAction('rollback'));

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
  if(!encodedFrameAt)$('encodedFormat').textContent=value.mode==='raw'?'Picture encoder off':'Waiting for a decoded picture';
  updateEncodingFields();
  setFormBaseline('encodingForm');
  updatePictureQuality();
}

function updatePictureQuality(){
  if(!encoding)return;
  const choices=['compact','balanced','detail'],field=encoding.codec==='jpeg'?'jpeg_quality':'bitrate_bps';
  if(!pictureQualityDirty)$('pictureQuality').value=choices.find(choice=>window.NuvoDeviceSettings.pictureQualityConfiguration(encoding,choice)[field]===encoding[field])||'custom';
  $('pictureQuality').disabled=pictureQualityBusy;
  const choice=$('pictureQuality').value;
  const selected=choice==='custom'?encoding:window.NuvoDeviceSettings.pictureQualityConfiguration(encoding,choice);
  $('pictureQualityDetail').textContent=`${encoding.codec.toUpperCase()} · ${encoding.source.replaceAll('_',' ')} · rate ceiling ${encoding.cadence_hz} frames/s. ${encoding.codec==='jpeg'?'JPEG quality '+selected.jpeg_quality+' / 100':'H.264 target bitrate '+(selected.bitrate_bps/1000000).toFixed(1)+' Mbit/s (not a link-speed guarantee)'}.${encoding.mode==='raw'?' Encoder is currently off; applying quality will not enable it.':''}`;
  $('pictureQualityApply').disabled=pictureQualityBusy||choice==='custom'||selected[field]===encoding[field];
}
$('pictureQuality').addEventListener('change',()=>{pictureQualityDirty=true;$('pictureQualityState').textContent='Editing shared quality · not applied';updatePictureQuality();});
$('pictureQualityApply').addEventListener('click',async()=>{
  if(!encoding||pictureQualityBusy)return;
  $('pictureQualityState').textContent='Applying shared picture quality…';pictureQualityBusy=true;updatePictureQuality();
  try{
    if(encodingFormDirty)throw new Error('Apply or discard pending Advanced preview encoder changes first.');
    const requested=window.NuvoDeviceSettings.pictureQualityConfiguration(encoding,$('pictureQuality').value);
    encoding=await (await api('/api/v1/encoding',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(requested)})).json();
    pictureQualityDirty=false;encodingPreviewPending=encoding.mode!=='raw';showEncodingConfig(encoding);restartStreams();
    $('pictureQualityState').textContent=encoding.mode==='raw'?'Saved device-wide · picture encoder remains off':'Saved device-wide · waiting for a picture with the requested settings';
  }catch(error){$('pictureQualityState').textContent=error.message;}
  finally{pictureQualityBusy=false;updatePictureQuality();}
});

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
  const revision=(latestConfigurationRevisions.get('/api/v1/encoding')||configurationRevisions.get('/api/v1/encoding'))?.replaceAll('"','');
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
    const changed=!encoding||JSON.stringify(updated)!==JSON.stringify(encoding);
    encoding=updated;
    if(!initialEncodingLoaded){
      initialEncodingLoaded=true;
      if(encoding.mode==='encoded'&&outputSettings.enabled)commitOutputSelection(window.NuvoDeviceSettings.outputRecipe('picture'));
    }
    if(!encodingFormDirty)showEncodingConfig(encoding);
    else if(changed)$('encodingSaveState').textContent='Device changed · review and apply';
    if(changed){encodingPreviewPending=encoding.mode!=='raw';restartStreams();}
    $('encodingError').textContent='';
    const delivery = encoding.mode === 'both' ? 'Network data + preview' : encoding.mode === 'raw' ? 'Network data' : 'Network preview only';
    setFlowState('flowDeliveryState', delivery, encoding.mode === 'encoded' ? 'attention' : 'waiting');
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
    if(!requested.mode)throw new Error('Keep uncompressed network data or the compressed preview encoder enabled');
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
  updateDepthProfileLabel();
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
    setFlowState('flowQualityState', profile, 'complete');
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
    updateDepthProfileLabel();
    for(const button of buttons){
      const profile=catalog.profiles[button.dataset.depthPreset];
      button.closest('article').querySelector('h2').textContent=profile.label;
      button.closest('article').querySelector('p').textContent=profile.description;
      button.disabled=false;
    }
  }catch(error){depthProfiles=null;$('presetError').textContent=error.message;}
}
loadDepthProfiles();

function updateDepthProfileLabel(){
  if(!depthProfiles||!stereo)return;
  const candidate=algorithmFormDirty?requestedStereo():stereo;
  const name=Object.keys(depthProfiles.profiles).find(key=>{
    const controls={...depthProfiles.base_controls,...depthProfiles.profiles[key].controls};
    return Object.keys(controls).every(field=>controls[field]===candidate[field]);
  })||'custom';
  $('depthProfileChoice').disabled=false;$('depthProfileChoice').value=name;
  $('depthProfileApply').disabled=!algorithmFormDirty;
  $('depthProfileState').textContent=algorithmFormDirty?`Pending matcher changes · ${name==='custom'?'Custom':depthProfiles.profiles[name].label}`:`Requested profile: ${name==='custom'?'Custom':depthProfiles.profiles[name].label}`;
}
$('depthProfileChoice').addEventListener('change',()=>{
  const name=$('depthProfileChoice').value;
  if(name==='custom'){openPage('algorithm');return;}
  const config={...depthProfiles.base_controls,...depthProfiles.profiles[name].controls};
  for(const [id,value] of Object.entries(window.NuvoDeviceSettings.values('algorithm',config)))$(id).value=value;
  $('algorithmForm').dispatchEvent(new Event('input',{bubbles:true}));updateDepthProfileLabel();
});
$('depthProfileApply').addEventListener('click',()=>$('algorithmForm').requestSubmit());
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
    $('outputDeliveryStats').textContent=health.publication_sequence_gaps==null?'Detailed sequence counters are unavailable from this runtime.':
      `Sensor-frame sequence gaps: L ${health.left_sensor_sequence_gaps??'—'} / R ${health.right_sensor_sequence_gaps??'—'} · publication sequence skips: ${health.publication_sequence_gaps} (includes intentional demand/cadence skips). These counters are not lost stereo pairs or browser/network loss rates.`;
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
    const stateLabels={within_deadband:'Inside target band',stable:health.exposure_last_reason==='highlight_compromise'?'Exposure held · highlight/shadow compromise':'Exposure held · monitoring scene changes',awaiting_applied_controls:'Waiting for both cameras to report the command',applied_metadata_unavailable:'Holding exposure · camera control metadata unavailable',settling:'Camera controls settling',waiting_interval:'Waiting for next sample',confirming_dark:'Confirming darker scene',confirming_bright:'Confirming brighter scene',adjusted:'Adjustment sent',at_limit_or_quantized:'At limit or below one step',auto_ready:'Auto ready'};
    const stateText=health.exposure_last_error||stateLabels[health.exposure_controller_state]||health.exposure_last_reason||'No adjustment needed';
    $('exposureReason').textContent=!exposureAutomatic?'Manual values held constant':`${stateText}${leftMedian==null?'':` · L/R ${leftMedian.toFixed(0)}/${rightMedian.toFixed(0)}`}`;
    if(exposurePending){
      const reloaded=Number(health.exposure_configuration_reloads)>exposurePending.baselineReloads;
      if(reloaded&&health.exposure_mode===exposurePending.mode){
        $('exposureSaveState').textContent='Saved · exposure commands reloaded';exposurePending=null;
        toneSaveStatus($('exposureSaveState'));
      }
    }
    renderCalibrationReadiness();
    $('calibrationHash').textContent = health.calibration_sha256 ? health.calibration_sha256.slice(0,16)+'…' : 'Not loaded';
    $('calibrationHash').title=health.calibration_sha256 || '';
  } catch (_) {
    if(await refreshSetupStatus()){
      updateViewerAvailability(true);
      $('connection').textContent='Connected';$('connection').className='pill online';
      $('cameraState').textContent='Capture unavailable';
      $('cameraDetail').textContent=calibrationReadiness?.camera_service?.error||'Inspect capture service health separately from calibration.';
      $('overviewCaptureState').textContent='Unavailable';$('overviewDepthState').textContent='No fresh depth';
      $('frameRate').textContent='—';depthOutputRate=null;lastHealthPublished=null;
      setFlowState('flowConnectionState','Secure session','complete');setFlowState('flowCaptureState','Needs attention','attention');
      return;
    }
    updateViewerAvailability(true); $('connection').textContent = 'Not connected'; $('connection').className = 'pill offline'; $('frameRate').textContent='—'; depthOutputRate=null; lastHealthPublished=null; performanceModel.stale=true; performanceModel.latest=null; renderPerformanceMonitor(); updateLiveMetrics(null); document.querySelector('.status-strip').classList.add('stale'); $('overviewCaptureState').textContent='Disconnected'; $('overviewDepthState').textContent='Unknown'; $('deviceResolution').textContent='Last known device state'; setFlowState('flowConnectionState','Waiting'); setFlowState('flowCaptureState','Waiting'); }
}

function parseFrame(buffer) { return window.NuvoFrameCodec.parseFrame(buffer); }
function product(frame, name) { return frame.metadata.products.find(x => x.kind === name); }
function describeProduct(descriptor){
  const label=OUTPUT_PRODUCTS.find(p=>p[0]===descriptor.kind)?.[1]||descriptor.kind;
  return `${label}${descriptor.kind.startsWith('rectified_stereo')?' · left camera | right camera':''} · ${descriptor.width} × ${descriptor.height}`;
}
function setSampleCanvasLive(id, live) {
  $(id).closest('.sample-canvas')?.classList.toggle('live', live);
}
function updateViewerAvailability(disconnected=false) {
  if(disconnected){firstDepth={};refreshFirstDepth();}
  const empty=$('viewerEmptyState'),picture=outputSettings.preview&&!outputSettings.enabled;
  $('viewerModeNotice').hidden=encoding?.mode!=='encoded'||picture;
  empty.hidden=!disconnected&&!viewerPaused&&(picture?!!encodedFrameAt:!!lastFrameAt);
  empty.textContent=disconnected?(lastFrameAt||encodedFrameAt?'Disconnected · showing last received products':'Disconnected · waiting for device'):viewerPaused?'Paused · this browser has released its stream demand.':picture?(encoding?.mode==='raw'?'The shared picture encoder is off. Enable it explicitly in Device → Advanced preview encoder.':'Waiting for the shared picture preview.'):encoding?.mode==='encoded'?'Uncompressed network data is off. Choose Shared picture preview, or enable data in Device → Advanced preview encoder.':'Waiting for selected live products.';
  $('viewerPicture').hidden=!picture;$('selectedProductGrid').hidden=picture;$('pictureMeasurementNotice').hidden=!picture;
  $('encodedEmptyState').textContent=disconnected?'Disconnected · waiting for picture':encoding?.mode==='raw'?'Picture encoder is off':'Waiting for picture';
  $('viewerPause').textContent=viewerPaused?'Resume':'Pause';
  if(disconnected||viewerPaused||!outputReception)$('viewerRate').textContent=`Requested: ${picture?outputSettings.preview_cadence_hz:outputSettings.cadence_hz} frames/s · Receiving: ${viewerPaused?'paused':'waiting'}`;
  if(!lastFrameAt&&!encodedFrameAt){$('viewerReceiving').textContent=viewerPaused?'Paused':'Requested · waiting for a matching frame';$('viewerSource').textContent='Source and format appear after a frame is decoded.';}
  if(disconnected)setSampleCanvasLive('encodedCanvas',false);
  if(disconnected||viewerPaused){
    const state=disconnected?'Disconnected · last received selection': 'Paused · this browser';
    $('viewerReceiving').textContent=state;$('outputReceiving').textContent=state;
    if($('outputDestination').value==='browser')$('outputSaveState').textContent=state;
  }
}
function recordReceiving(kind,label,products=[]){
  const now=Date.now();receivingTimes.push(now);receivingTimes=receivingTimes.filter(t=>t>=now-5000);
  outputReception={kind,label,products,at:now};
  const state=window.NuvoDeviceSettings.outputSelectionStatus(outputSettings,outputSettings,outputReception);
  $('viewerReceiving').textContent=`Receiving: ${label}`;$('outputReceiving').textContent=`Receiving: ${label}`;
  if(state.phase==='receiving'&&$('outputDestination').value==='browser')$('outputSaveState').textContent='Receiving requested selection';
  const rate=receivingTimes.length>1?(receivingTimes.length-1)*1000/(now-receivingTimes[0]):null;
  $('viewerRate').textContent=`Requested: ${kind==='picture'?outputSettings.preview_cadence_hz:outputSettings.cadence_hz} frames/s · Receiving: ${rate===null?'measuring':rate.toFixed(1)+' frames/s'}`;
}
function receivingStatus(){
  if(viewerPaused)return 'Paused · this browser';
  if(capabilities?.connection?.products_permitted===false)return 'Blocked · '+(capabilities.connection.product_policy_reason||'product delivery is disabled on this camera connection');
  if(!outputReception)return 'Requested · waiting for a matching frame';
  const rate=outputSettings.enabled?outputSettings.cadence_hz:outputSettings.preview_cadence_hz;
  return Date.now()-outputReception.at>Math.max(2500,3000/rate)?`Stale · last received ${outputReception.label}`:`Receiving: ${outputReception.label}`;
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
  return !document.hidden && !!canvas.closest('.page.active') && !canvas.closest('[hidden]') && !canvas.closest('details:not([open])');
}
function drawVisibleProduct(canvas,frame,descriptor) {
  if(!descriptor||!canvasVisible(canvas))return;
  if(descriptor.kind==='depth')drawDepth(canvas,frame,descriptor);
  else if(descriptor.kind==='disparity')drawDisparity(canvas,frame,descriptor);
  else drawGray(canvas,frame,descriptor);
}
function renderFrame(frame) {
  window.NuvoFirstUse.observe(firstDepth,frame,Date.now());refreshFirstDepth();
  const identity=`${frame.metadata.run_uuid}:${frame.metadata.capture_epoch}:${frame.metadata.capture_sequence}:${frame.metadata.products.map(p=>p.kind).join(',')}`;
  if(identity===lastIdentity)return;
  const grid=$('selectedProductGrid'),ordered=OUTPUT_PRODUCTS.map(([name])=>product(frame,name)).filter(Boolean);
  grid.querySelectorAll('[data-output-product]').forEach(canvas=>{if(!product(frame,canvas.dataset.outputProduct))canvas.closest('figure').remove();});
  for(const descriptor of ordered){
    let canvas=grid.querySelector(`[data-output-product="${descriptor.kind}"]`);
    if(!canvas){const figure=document.createElement('figure'),caption=document.createElement('figcaption');canvas=document.createElement('canvas');canvas.dataset.outputProduct=descriptor.kind;figure.append(caption,canvas);grid.append(figure);}
    canvas.closest('figure').querySelector('figcaption').textContent=describeProduct(descriptor);
    canvas.setAttribute('aria-label',describeProduct(descriptor)+' · uncompressed data rendered in this browser');
    drawVisibleProduct(canvas,frame,descriptor);
  }
  displayedNumericFrame=product(frame,'depth')?frame:null;
  $('depthLegend').hidden=!displayedNumericFrame;$('pixelInspector').hidden=!displayedNumericFrame;
  $('viewerEmptyState').hidden=true;$('viewerTelemetry').hidden=false;
  lastFrameAt=Date.now();lastSequence=frame.sequence;lastIdentity=identity;
  const label=window.NuvoDeviceSettings.outputSelectionLabel({enabled:true,preview:false,products:ordered.map(p=>p.kind)});
  recordReceiving('data',label,ordered.map(p=>p.kind));
  $('viewerSource').textContent=product(frame,'depth')?`Numeric depth${product(frame,'validity')?' + validity':' · validity unavailable; do not measure'} · rendered locally from uncompressed measurements`:'Uncompressed ISP/processed images · rendered locally in this browser';
  $('outputFrameSummary').textContent=`Frame ${frame.metadata.capture_sequence} · ${ordered.map(describeProduct).join('; ')}`;
  setFlowState('flowCaptureState','Products live','complete');
  $('sequence').textContent=`Frame ${frame.metadata.capture_sequence}`;$('age').textContent='Updated now';$('frameError').textContent='';
}
function rawPreviewPage(){return !document.hidden&&!viewerPaused&&capabilities?.connection?.products_permitted!==false&&outputSettings.enabled&&['viewer','network'].includes(selectedPage);}
function refreshFirstDepth(){
  const live=window.NuvoFirstUse.ready(firstDepth,Date.now())&&encoding?.mode!=='encoded';
  $('firstDepthSave').disabled=!live;
  setFlowState('flowDeliveryState',live?'Fresh depth ready':'Waiting for fresh depth',live?'complete':'waiting');
  if(firstDepth.error)$('firstDepthError').textContent=firstDepth.error;
  $('firstDepthStatus').textContent=live?`Fresh depth confirmed · frame ${firstDepth.sequence} · ready to save`:
    firstDepth.frame?'Waiting for fresh depth · keep Live view open and check capture status.':'Waiting for two fresh associated depth and validity frames.';
  if(firstDepth.frame){const authority=window.NuvoFirstUse.calibration(firstDepth.frame.metadata);$('firstDepthAuthority').textContent=`${authority.label}. ${authority.detail}`;}
}
$('firstDepthStart').addEventListener('click',()=>{
  $('firstDepthError').textContent='';
  try{
    if(encoding?.mode==='encoded')throw new Error('Enable Uncompressed network data in Device → Advanced preview encoder, apply settings, then try again.');
    commitOutputSelection(window.NuvoDeviceSettings.outputRecipe('depth_reference'));
    firstDepth={};refreshFirstDepth();
  }catch(error){$('firstDepthError').textContent=error.message;}
});
$('firstDepthSave').addEventListener('click',()=>{
  refreshFirstDepth();if($('firstDepthSave').disabled)return;
  const frame=firstDepth.frame,m=frame.metadata,url=URL.createObjectURL(new Blob([frame.buffer],{type:'application/octet-stream'}));
  const link=document.createElement('a');link.href=url;link.download=`nuvo-depth-${m.run_uuid}-${m.capture_epoch}-${m.capture_sequence}.nuv`;link.click();setTimeout(()=>URL.revokeObjectURL(url),1000);
  $('firstDepthStatus').textContent=`Saved frame ${m.capture_sequence} with depth, validity and calibration metadata.`;
});
setInterval(refreshFirstDepth,500);
async function pollFrame(generation,token=rawPollToken) {
  if (!rawPolling||token!==rawPollToken||generation!==rawGeneration||!rawPreviewPage()) return;
  const started=Date.now();
  try { const buffer=await (await api(outputSubscriptionPath('/api/v1/frame/latest'))).arrayBuffer();if(generation!==rawGeneration||token!==rawPollToken||!rawPolling)return;renderFrame(parseFrame(buffer)); }
  catch (error) { if(generation!==rawGeneration||token!==rawPollToken||!rawPolling)return;$('frameError').textContent=error.message; }
  if(generation===rawGeneration&&token===rawPollToken&&rawPolling)setTimeout(()=>pollFrame(generation,token),Math.max(0,1000/outputSettings.cadence_hz-(Date.now()-started)));
}
function socketUrl(path) { return `${location.protocol==='https:'?'wss':'ws'}://${location.host}${path}`; }
function startRawViewer(){
  if(rawSocket||!rawPreviewPage()||!encoding||encoding.mode==='encoded')return;
  const generation=rawGeneration,socket=new WebSocket(socketUrl(outputSubscriptionPath('/api/v1/stream')));rawSocket=socket;socket.binaryType='arraybuffer';
  socket.onopen=()=>{if(generation===rawGeneration&&socket===rawSocket){rawPolling=false;++rawPollToken;}};
  socket.onmessage=event=>{if(generation!==rawGeneration||socket!==rawSocket)return;try{renderFrame(parseFrame(event.data));rawRetryDelay=1000;}catch(error){$('frameError').textContent=error.message;socket.close()}};
  socket.onclose=()=>{
    if(generation!==rawGeneration||socket!==rawSocket)return;
    rawSocket=null;
    if(!rawPreviewPage()||!encoding||encoding.mode==='encoded')return;
    $('frameError').textContent='Stream interrupted · temporary polling; reconnecting automatically.';
    if(!rawPolling){rawPolling=true;pollFrame(generation,++rawPollToken);}
    rawRetryTimer=setTimeout(()=>{rawRetryTimer=null;if(generation===rawGeneration)startRawViewer();},rawRetryDelay);
    rawRetryDelay=Math.min(15000,rawRetryDelay*2);
  };
}

function parseEncoded(buffer) { return window.NuvoFrameCodec.parseEncoded(buffer); }
function resetEncodedDecoder() {
  if(decoder){try{decoder.close()}catch(_){}}
  decoder=null;decoderCodec='';encodedLastSequence=0;encodedWaitingForKeyframe=true;encodedMetadata.clear();
}
function ensureDecoder(codec,width,height,generation) {
  if(!('VideoDecoder' in window))throw new Error('This browser cannot preview H.264; the network stream remains available');
  const key=`${codec}:${width}x${height}`;
  if(decoder&&decoder.state!=='closed'&&decoderCodec===key)return decoder;
  if(decoder){try{decoder.close()}catch(_){}}
  decoder=null;decoderCodec=key;encodedWaitingForKeyframe=true;
  encodedMetadata.clear();
  const activeDecoder=new VideoDecoder({output:video=>{
    const presentation=encodedMetadata.get(video.timestamp);
    if(generation!==encodedGeneration||decoder!==activeDecoder){video.close();return;}
    encodedMetadata.delete(video.timestamp);
    if(!presentation||presentation.arrival<encodedLastPresented||!encodedFrameMatchesConfiguration(presentation.metadata)){video.close();return;}
    encodedLastPresented=presentation.arrival;
    drawEncodedSource(video,video.displayWidth,video.displayHeight,presentation.metadata,presentation.payloadBytes);video.close();
  },error:error=>{
    if(generation!==encodedGeneration||decoder!==activeDecoder)return;
    encodedWaitingForKeyframe=true;$('encodingError').textContent=`Decoder resynchronizing: ${error.message}`;
  }});
  decoder=activeDecoder;
  decoder.configure({codec,codedWidth:width,codedHeight:height,optimizeForLatency:true,hardwareAcceleration:'prefer-hardware'});
  return decoder;
}
function drawEncodedSource(source,width,height,metadata,payloadBytes){
  const canvases=[];
  if(selectedPage==='viewer'&&outputSettings.preview)canvases.push($('encodedCanvas'));
  if(selectedPage==='device'&&$('calibration').open)canvases.push($('calibrationCanvas'));
  for(const canvas of canvases){if(canvasVisible(canvas))window.NuvoFrameCodec.prepareCanvas(canvas,width,height).context.drawImage(source,0,0);}
  encodedFrameAt=Date.now();$('encodedAge').textContent='Updated now';setSampleCanvasLive('encodedCanvas',true);
  if(metadata&&['viewer','network'].includes(selectedPage)){
    const numeric=['depth','disparity'].includes(metadata.source);
    const label=`${metadata.codec.toUpperCase()} picture · ${metadata.source.replaceAll('_',' ')}${numeric?' visualization':''} · ${width} × ${height}`;
    recordReceiving('picture',label);
    $('viewerSource').textContent=numeric?'Compressed grayscale visualization · numeric measurement unavailable':'Compressed ISP/processed camera picture · one shared device encoder';
    $('viewerEmptyState').hidden=true;$('depthLegend').hidden=true;$('pixelInspector').hidden=true;
  }
  $('encodedSequence').textContent=`Frame ${metadata.capture_sequence}`;
  $('encodedSize').textContent=`${(payloadBytes/1024).toFixed(1)} KiB`;
  $('encodedLatency').textContent=Number.isFinite(metadata.total_ms)?`Preview pipeline ${metadata.total_ms.toFixed(1)} ms`:`Encode only ${metadata.encode_ms.toFixed(1)} ms`;
  $('encodedLatency').title=Number.isFinite(metadata.total_ms)?`Preparation ${metadata.preparation_ms.toFixed(1)} ms · queue ${metadata.queue_ms.toFixed(1)} ms · encode ${metadata.encode_ms.toFixed(1)} ms. Excludes camera exposure, transport and display.`:'Encode only; excludes preview preparation, queueing, transport and display.';
  $('encodedFormat').textContent=`${metadata.codec.toUpperCase()} · ${metadata.source.replaceAll('_',' ')}${['depth','disparity'].includes(metadata.source)?' visualization':''} · ${metadata.width} × ${metadata.height}`;
  $('encodingError').textContent='';
  if(encodingPreviewPending){
    encodingPreviewPending=false;
    if(!encodingFormDirty)$('encodingSaveState').textContent='Saved · preview available';
    if(!pictureQualityDirty&&$('pictureQualityState').textContent.startsWith('Saved device-wide'))$('pictureQualityState').textContent='Saved device-wide · picture received with these settings';
  }
}
async function renderEncoded(frame,generation,arrival) {
  const {metadata,payload}=frame;
  if(generation!==encodedGeneration||!encodedFrameMatchesConfiguration(metadata))return;
  if(metadata.codec==='jpeg'){
    const bitmap=await createImageBitmap(new Blob([payload],{type:'image/jpeg'}));
    if(generation!==encodedGeneration||!encodedFrameMatchesConfiguration(metadata)||arrival<encodedLastPresented){bitmap.close();return}
    encodedLastPresented=arrival;drawEncodedSource(bitmap,bitmap.width,bitmap.height,metadata,payload.byteLength);bitmap.close();
  }else{
    // Latest-frame delivery intentionally skips frames. Independent keyframes
    // carry everything needed to decode the next image without reconfiguration.
    if(encodedLastSequence&&frame.streamSequence!==encodedLastSequence+1&&!metadata.keyframe)resetEncodedDecoder();
    encodedLastSequence=frame.streamSequence;
    const activeDecoder=ensureDecoder(metadata.codec_string,metadata.width,metadata.height,generation);
    if(activeDecoder.decodeQueueSize>=2){if(!metadata.keyframe)encodedWaitingForKeyframe=true;return;}
    if(encodedWaitingForKeyframe&&!metadata.keyframe){
      $('encodingError').textContent='Stream resynchronizing at the next keyframe';
      return;
    }
    try{
      const timestamp=Math.max(0,Math.floor(metadata.capture_time_ns/1000));
      if(encodedMetadata.size>=4)encodedMetadata.delete(encodedMetadata.keys().next().value);encodedMetadata.set(timestamp,{metadata,payloadBytes:payload.byteLength,arrival});
      activeDecoder.decode(new EncodedVideoChunk({type:metadata.keyframe?'key':'delta',timestamp,data:payload}));
      encodedWaitingForKeyframe=false;
    }catch(error){resetEncodedDecoder();throw error}
  }

}
async function queueEncoded(frame,generation,arrival){
  if(generation!==encodedGeneration)return;
  if(frame.metadata.codec!=='jpeg')return renderEncoded(frame,generation,arrival);
  pendingJpeg={frame,generation,arrival};
  if(jpegDecodeBusy)return;
  jpegDecodeBusy=true;
  try{
    while(pendingJpeg){const next=pendingJpeg;pendingJpeg=null;try{await renderEncoded(next.frame,next.generation,next.arrival);}catch(error){if(next.generation===encodedGeneration)$('encodingError').textContent=error.message;}}
  }finally{jpegDecodeBusy=false;}
}
async function pollEncoded(generation,token=encodedPollToken) {
  if(!encodedPolling||token!==encodedPollToken||generation!==encodedGeneration||!encodedPreviewPage())return;
  const started=Date.now();
  try{
    const frame=parseEncoded(await (await api(`/api/v1/encoded/latest?cadence_hz=${outputSettings.preview_cadence_hz}`)).arrayBuffer());
    if(token===encodedPollToken&&encodedPolling)await queueEncoded(frame,generation,++encodedArrival);
  }
  catch(error){if(generation===encodedGeneration&&token===encodedPollToken&&encodedPolling){$('encodingError').textContent=error.message;}}
  if(generation===encodedGeneration&&token===encodedPollToken&&encodedPolling)setTimeout(()=>pollEncoded(generation,token),Math.max(0,1000/outputSettings.preview_cadence_hz-(Date.now()-started)));
}
function encodedPreviewPage(){return !document.hidden&&capabilities?.connection?.products_permitted!==false&&(selectedPage==='device'&&$('calibration').open||!viewerPaused&&outputSettings.preview&&['viewer','network'].includes(selectedPage));}
function startEncodedViewer(){
  if(encodedSocket||!encodedPreviewPage()||!encoding||encoding.mode==='raw')return;
  const generation=encodedGeneration, socket=new WebSocket(socketUrl(`/api/v1/encoded/stream?cadence_hz=${outputSettings.preview_cadence_hz}`));
  encodedSocket=socket;socket.binaryType='arraybuffer';
  socket.onopen=()=>{if(generation===encodedGeneration&&socket===encodedSocket){encodedPolling=false;++encodedPollToken;}};
  socket.onmessage=event=>{if(generation!==encodedGeneration||socket!==encodedSocket)return;try{queueEncoded(parseEncoded(event.data),generation,++encodedArrival).catch(error=>{$('encodingError').textContent=error.message});encodedRetryDelay=1000;}catch(error){$('encodingError').textContent=error.message}};
  socket.onclose=()=>{
    if(generation!==encodedGeneration||socket!==encodedSocket)return;
    encodedSocket=null;
    if(!encodedPreviewPage()||!encoding||encoding.mode==='raw')return;
    $('encodingError').textContent='Preview interrupted · temporary polling; reconnecting automatically.';
    if(!encodedPolling){encodedPolling=true;pollEncoded(generation,++encodedPollToken);}
    encodedRetryTimer=setTimeout(()=>{encodedRetryTimer=null;if(generation===encodedGeneration)startEncodedViewer();},encodedRetryDelay);
    encodedRetryDelay=Math.min(15000,encodedRetryDelay*2);
  };
}
function restartStreams(){
  ++rawGeneration;
  ++encodedGeneration;encodedArrival=0;encodedLastPresented=0;
  pendingJpeg=null;
  clearTimeout(rawRetryTimer);clearTimeout(encodedRetryTimer);rawRetryTimer=null;encodedRetryTimer=null;
  rawRetryDelay=1000;encodedRetryDelay=1000;++rawPollToken;++encodedPollToken;
  rawPolling=false;encodedPolling=false;
  if(rawSocket){const value=rawSocket;rawSocket=null;value.onmessage=null;value.onclose=null;value.close()}
  if(encodedSocket){const value=encodedSocket;encodedSocket=null;value.onmessage=null;value.onclose=null;value.onerror=null;value.close()}
  resetEncodedDecoder();
  $('selectedProductGrid').replaceChildren();
  for(const id of ['encodedCanvas','calibrationCanvas']){const canvas=$(id);canvas.width=728;canvas.height=544;}
  setSampleCanvasLive('encodedCanvas',false);
  displayedNumericFrame=null;outputReception=null;receivingTimes=[];
  $('outputReceiving').textContent=viewerPaused?'Paused · this browser':'Receiving: waiting for a matching frame';
  $('depthLegend').hidden=true;$('pixelInspector').hidden=true;$('pixelValue').textContent='Click the depth image or enter a pixel coordinate.';
  $('viewerEmptyState').hidden=false;$('viewerTelemetry').hidden=true;
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
  try{const result=await calibrationOperation('/api/v1/calibration/rollback',{});$('calibrationActivationState').textContent=result.active_bundle_id?`Restored ${result.active_bundle_id.slice(0,16)}…`:'Restored the previous calibration and capture policy';setTimeout(()=>{refreshHealth();refreshSetupStatus();},2000);}
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

async function refreshDiagnostics(){
  $('downloadDiagnostics').disabled=true;
  try{
    const payload=await(await api('/api/v1/diagnostics')).json();
    if(payload.result?.schema_version!==1||payload.result?.report_type!=='nuvo-depth-support')throw new Error('Unavailable report');
    supportReport=payload.result;
    $('diagnosticJson').textContent=JSON.stringify(supportReport,null,2);
    $('installedVersions').textContent=Object.entries(supportReport.versions||{}).map(([name,version])=>`${name}: ${version}`).join('\n')||'Installed versions unavailable; inspect dpkg-query.';
    $('upgradeNotice').textContent=supportReport.upgrade?.notice||'Release availability has not been checked.';
    $('upgradeCommand').textContent=supportReport.upgrade?.command||'Approved installation reference is unavailable; inspect the release guide.';
    $('downloadDiagnostics').disabled=false;
  }catch(_){
    supportReport=null;
    $('diagnosticJson').textContent='Diagnostics unavailable. Reconnect, or run sudo nuvoctl diagnostics --json in the Pi terminal.';
    $('installedVersions').textContent='Installed versions unavailable.';
    $('upgradeCommand').textContent='Read the approved release guide; no verified command was obtained.';
  }
}
$('refreshDiagnostics').addEventListener('click',refreshDiagnostics);
$('downloadDiagnostics').addEventListener('click',()=>{
  if(!supportReport)return;
  const url=URL.createObjectURL(new Blob([JSON.stringify(supportReport,null,2)+'\n'],{type:'application/json'}));
  const link=document.createElement('a');link.href=url;link.download='nuvo-depth-support.json';link.click();
  setTimeout(()=>URL.revokeObjectURL(url),1000);
});
$('copyDiagnostics').addEventListener('click',async()=>{
  if(!supportReport)await refreshDiagnostics();
  if(!supportReport)return;
  try{await navigator.clipboard.writeText($('diagnosticJson').textContent);$('copyDiagnosticsState').textContent='Copied';}
  catch(error){$('copyDiagnosticsState').textContent=`Copy unavailable: ${error.message}`;}
});

async function restoreSession() {
  try {
    await connectSession();
    await refreshSetupStatus();
    await Promise.all([refreshHealth(),refreshTransport(),refreshEncoding(),refreshStereo(),refreshExposure(),refreshCapabilities()]);
    if(lastHealth){
      if(!$('calibrationWidth').value)$('calibrationWidth').value=lastHealth.image_width||728;
      if(!$('calibrationHeight').value)$('calibrationHeight').value=lastHealth.image_height||544;
      $('calibrationRates').value=String(lastHealth.capture_target_hz||$('calibrationRates').value||60);
    }
    await restoreCalibrationSession();restartStreams();
  } catch (error) {
    $('connection').textContent='Not connected';$('connection').className='pill offline';
    $('diagnosticJson').textContent=error.message;
  }
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

// Diagnostics owns the only performance component; navigation never reparents it.
function placePerformanceMonitor(page){
  $('performanceMonitor').hidden=page!=='device'||!$('diagnostics').open;
  if(!$('performanceMonitor').hidden)renderPerformanceMonitor();
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
  document.querySelectorAll('[data-live-calibration]').forEach(e=>{e.classList.toggle('accepted',calibrationReadiness?.accepted===true);e.textContent=calibrationReadiness?.state==='preview_enabled'?'Provisional calibration · distance accuracy unverified':calibrationReadiness?.label||'Calibration status unavailable';e.title=calibrationReadiness?.message||'Refresh calibration readiness.';});
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
    const input=label.querySelector('input:not([type="hidden"]),select');if(!input||!input.id||['checkbox','radio'].includes(input.type))return;
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
  $('performanceTarget').addEventListener('change',event=>{analysisTarget=Number(event.target.value);renderPerformanceMonitor();});
  if('ResizeObserver' in window)new ResizeObserver(drawPerformanceChart).observe($('performanceChart').parentElement);
}
initializeSettingsUi();
let requestedGroup=null;
try{requestedGroup=sessionStorage.getItem('nuvo-open-settings-group');sessionStorage.removeItem('nuvo-open-settings-group');}catch(_){}
if(requestedGroup){const group=[...document.querySelectorAll('[data-group]')].find(element=>element.dataset.group===requestedGroup);if(group){group.open=true;const parent=group.closest('.advanced-card');if(parent)parent.open=true;}}
initializeOutputControls();
$('algorithmForm').addEventListener('input',updateDepthProfileLabel);
$('algorithmForm').addEventListener('change',updateDepthProfileLabel);

document.querySelectorAll('.device-section').forEach(section=>section.addEventListener('toggle',()=>{
  if(section.id==='calibration'&&selectedPage==='device')restartStreams();
  if(section.id==='diagnostics'){placePerformanceMonitor(selectedPage);if(section.open)refreshDiagnostics();}
}));
function inspectSelectedPixel(x,y){
  const result=window.NuvoFrameCodec.inspectDepth(displayedNumericFrame,x,y);
  $('pixelValue').textContent=result.valid?`Depth Z: ${result.z.toFixed(3)} m · valid pixel (${x}, ${y}) · capture ${result.captureSequence}${result.outsideDisplay?' · outside display scale':''}`:result.reason;
}
$('pixelInspector').addEventListener('submit',event=>{event.preventDefault();inspectSelectedPixel(Number($('pixelX').value),Number($('pixelY').value));});
$('selectedProductGrid').addEventListener('click',event=>{
  const canvas=event.target.closest('canvas[data-output-product="depth"]');if(!canvas)return;
  const bounds=canvas.getBoundingClientRect();
  const x=Math.min(canvas.width-1,Math.max(0,Math.floor((event.clientX-bounds.left)*canvas.width/bounds.width)));
  const y=Math.min(canvas.height-1,Math.max(0,Math.floor((event.clientY-bounds.top)*canvas.height/bounds.height)));
  $('pixelX').value=x;$('pixelY').value=y;inspectSelectedPixel(x,y);
});
document.addEventListener('visibilitychange',restartStreams);
setInterval(()=>{
  const status=receivingStatus();
  $('viewerReceiving').textContent=status;$('outputReceiving').textContent=status;
  if(!status.startsWith('Receiving:')){
    $('viewerRate').textContent=`Requested: ${outputSettings.enabled?outputSettings.cadence_hz:outputSettings.preview_cadence_hz} frames/s · ${status.split(' · ')[0]}`;
    if($('outputDestination').value==='browser')$('outputSaveState').textContent=status;
  }
},500);
window.addEventListener('pageshow',event=>{if(event.persisted)restoreSession();});
window.addEventListener('pagehide',()=>{rawSocket?.close();encodedSocket?.close();});
updateAlgorithmFields();
updateExposureFields();
updateEncodingFields();
restoreSession();
setInterval(()=>{if(!document.hidden)refreshHealth();},2000);
setInterval(()=>{if(!document.hidden){refreshTransport();refreshEncoding();refreshStereo();refreshExposure();refreshCapabilities();refreshSetupStatus();}},5000);
document.addEventListener('visibilitychange',()=>{if(!document.hidden){refreshHealth();refreshTransport();refreshEncoding();refreshStereo();refreshExposure();refreshCapabilities();}});
setInterval(()=>{if(lastFrameAt){const age=(Date.now()-lastFrameAt)/1000;$('age').textContent=age<1?'Updated now':`${age.toFixed(1)} s old`;$('age').classList.toggle('error',age>2);}if(encodedFrameAt){const age=(Date.now()-encodedFrameAt)/1000;$('encodedAge').textContent=age<1?'Updated now':`${age.toFixed(1)} s old`;$('encodedAge').classList.toggle('error',age>2)}},250);
