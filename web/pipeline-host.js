/* Standalone pipeline adapter; uses the same settings transport, validation and drafts as index. */
(() => {
  'use strict';
  const D=window.NuvoDeviceSettings,P=window.NuvoPipeline,C=window.NuvoFrameCodec,$=id=>document.getElementById(id);
  const dialog=$('claimDialog'),formControllers=new Map(),configurationRevisions=new Map(),latestConfigurationRevisions=new Map();
  let csrf='',capabilities=null,lastHealth=null,stereo=null,encoding=null,depthOutputRate=null,lastPublished=null,lastHealthAt=0,lastRun='';
  let outputSettings={enabled:true,products:['left_mono'],cadence_hz:5,delivery:'auto',preview:true,preview_cadence_hz:5};
  try{const saved=JSON.parse(localStorage.getItem('nuvo-output-selection'));if(saved){P.fromSettings(saved);outputSettings=saved;}}catch(_){}
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


  $('themeToggle').onclick=()=>applyTheme(document.documentElement.dataset.theme==='light'?'dark':'light',true);
  const names={exposure:['exposureForm','exposureError','exposureSaveState','exposureReset'],algorithm:['algorithmForm','algorithmError','algorithmSaveState','algorithmReset'],encoding:['encodingForm','encodingError','encodingSaveState','encodingReset']};
  const fieldValue=input=>input.type==='checkbox'?input.checked:input.value;
  const changedControls=c=>!c?.loaded?[]:c.controls.filter(input=>c.baseline.get(input.id)!==fieldValue(input));
  const fields=c=>Object.fromEntries(c.controls.map(input=>[input.id,fieldValue(input)]));
  function remember(c){if(!c.loaded)return;const path=D.DEFINITIONS[c.kind].path;if(changedControls(c).length)D.saveDraft(path,fields(c),configurationRevisions.get(path));else D.clearDraft(path);}
  function configuration(c){return D.configuration(c.kind,fields(c),c.saved);}
  function updateMode(c){
    c.controls.forEach(input=>{let inactive=false;
      if(c.kind==='exposure')inactive=['manualExposure','manualGain'].includes(input.id)&&$('exposureMode').value==='auto';
      if(c.kind==='algorithm')inactive=input.id==='sgbmMode'&&$('algorithmType').value==='bm';
      if(c.kind==='encoding')inactive=!['streamEncoderEnabled','streamRawEnabled'].includes(input.id)&&!$('streamEncoderEnabled').checked;
      input.disabled=!c.loaded||c.busy||inactive;
    });
  }
  function validateSettings(c){c.errors=D.validate(c.kind,configuration(c),{safeExposureUs:lastHealth?.exposure_safe_max_us||8000});c.controls.filter(input=>!input.disabled&&!input.validity.valid).forEach(input=>c.errors.set(input.id,input.validationMessage));return !c.errors.size;}
  function updateFormUi(c){updateMode(c);if(c.attempted)validateSettings(c);c.controls.forEach(input=>input.setAttribute('aria-invalid',String(c.errors.has(input.id))));window.NuvoPipelineUI?.refresh();}
  function baseline(c,config){c.saved=config;c.controls.forEach(input=>{const value=D.values(c.kind,config)[input.id];if(input.type==='checkbox')input.checked=value;else input.value=value;});c.baseline=new Map(c.controls.map(input=>[input.id,fieldValue(input)]));c.loaded=true;c.errors=new Map();c.attempted=false;updateMode(c);}
  async function api(path,options={}){return D.request(path,options,{csrf,configurationRevisions,latestConfigurationRevisions,
    encodingFormDirty:formControllers.get('encodingForm').loaded&&changedControls(formControllers.get('encodingForm')).length>0,algorithmFormDirty:formControllers.get('algorithmForm').loaded&&changedControls(formControllers.get('algorithmForm')).length>0,exposureFormDirty:formControllers.get('exposureForm').loaded&&changedControls(formControllers.get('exposureForm')).length>0,
    safeExposureUs:lastHealth?.exposure_safe_max_us||8000,onUnauthorized(){if(!dialog.open)dialog.showModal();}});}
  for(const [kind,[id,error,status,reset]] of Object.entries(names)){
    const form=document.createElement('form');form.id=id;form.noValidate=true;
    for(const [fieldId,meta] of Object.entries(D.DEFINITIONS[kind].fields)){
      const input=document.createElement(meta.type==='select'?'select':'input');input.id=fieldId;
      if(meta.options)meta.options.forEach(option=>input.append(new Option(option.label,option.value)));else input.type=meta.type||'text';
      for(const key of ['min','max','step'])if(meta[key]!==undefined)input[key]=meta[key];if('required' in meta)input.required=true;form.append(input);
    }
    const errorNode=document.createElement('p'),statusNode=document.createElement('p'),discard=document.createElement('button');errorNode.id=error;statusNode.id=status;discard.id=reset;discard.type='button';form.append(errorNode,statusNode,discard);$('pipelineSharedControls').append(form);
    const c={kind,form,controls:[...form.querySelectorAll('input,select')],baseline:new Map(),saved:null,loaded:false,busy:false,attempted:false,errors:new Map()};formControllers.set(id,c);
    statusNode.textContent='Loading device settings…';
    const edit=()=>{if(!c.loaded)return;statusNode.textContent=changedControls(c).length+' pending changes';remember(c);updateFormUi(c);};form.addEventListener('input',edit);form.addEventListener('change',edit);
    discard.onclick=()=>{if(!c.saved)return;baseline(c,c.saved);configurationRevisions.set(D.DEFINITIONS[kind].path,latestConfigurationRevisions.get(D.DEFINITIONS[kind].path));D.clearDraft(D.DEFINITIONS[kind].path);errorNode.textContent='';statusNode.textContent='Reset to saved device settings';updateFormUi(c);};
    form.onsubmit=async event=>{event.preventDefault();c.attempted=true;if(c.busy||!c.loaded||!validateSettings(c)){updateFormUi(c);return;}const requested=configuration(c);c.busy=true;statusNode.textContent='Applying…';errorNode.textContent='';updateFormUi(c);
      try{const saved=await(await api(D.DEFINITIONS[kind].path,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(requested)})).json();baseline(c,saved);if(kind==='algorithm')stereo=saved;if(kind==='encoding'){encoding=saved;restartStreams();}statusNode.textContent=kind==='algorithm'?'Saved · applies on next depth frame':kind==='exposure'?'Saved · waiting for camera confirmation':'Saved · preview syncing…';}
      catch(error){errorNode.textContent=error.message;statusNode.textContent='Settings not saved';}finally{c.busy=false;updateFormUi(c);}
    };
  }
  const outputForm=document.createElement('form');outputForm.id='outputForm';
  for(const [id,type,value] of [['outputEnabled','checkbox',true],['outputCadence','number',5],['outputDelivery','text','auto'],['outputPreviewEnabled','checkbox',true],['outputPreviewCadence','number',5]]){const input=document.createElement('input');input.id=id;input.type=type;if(type==='checkbox')input.checked=value;else input.value=value;outputForm.append(input);}
  Object.keys(P.PRODUCTS).forEach(product=>{const input=document.createElement('input');input.type='checkbox';input.name='outputProduct';input.value=product;outputForm.append(input);});$('pipelineSharedControls').append(outputForm);
  function outputLimits(){return {width:lastHealth?.image_width||lastHealth?.depth_width||728,height:lastHealth?.image_height||lastHealth?.depth_height||544,supportedProducts:capabilities?.products,qualified:!!capabilities?.connection?.qualified_link,bytesPerSecond:capabilities?.connection?.resource_budget_bytes_per_second,maxFrameBytes:capabilities?.stream_admission?.raw_maximum_wire_bytes,rawEnabled:encoding?.mode!=='encoded'};}
  function commitOutputSelection(settings){const result=P.compile(P.fromSettings(settings),outputLimits());outputSettings={enabled:result.enabled,products:result.products,cadence_hz:result.cadence_hz,delivery:result.delivery,preview:result.preview,preview_cadence_hz:result.preview_cadence_hz};let persistent=true;try{localStorage.setItem('nuvo-output-selection',JSON.stringify(outputSettings));}catch(_){persistent=false;}restartStreams();window.NuvoPipelineUI?.adoptOutputs(outputSettings);return persistent;}
  const drawGray=C.drawGray,drawDepth=C.drawDepth;
  function drawDisparity(canvas,frame,descriptor){C.drawDisparity(canvas,frame,descriptor,stereo?.max_disparity||128);}
  window.NuvoPipelineHost=Object.freeze({
  context(){return {
    ready:!!capabilities&&!!encoding,settings:{...outputSettings,products:[...outputSettings.products]},
    health:lastHealth,depthRate:depthOutputRate,stereo,encoding,connected:$('connection').classList.contains('online'),limits:outputLimits()
  };},
  exposure(){const c=formControllers.get('exposureForm');return {
    loaded:!!c?.loaded,busy:!!c?.busy,pending:c?changedControls(c).length:0,
    mode:$('exposureMode').value,time:$('manualExposure').value,gain:$('manualGain').value,
    max:Number($('manualExposure').max),commandTime:lastHealth?.requested_exposure_us,commandGain:lastHealth?.requested_analogue_gain,
    errors:c?[...c.errors.values()]:[],error:$('exposureError').textContent,status:$('exposureSaveState').textContent
  };},
  editExposure(patch){
    const c=formControllers.get('exposureForm');if(!c?.loaded||c.busy)return;
    const fields={mode:'exposureMode',time:'manualExposure',gain:'manualGain'};
    for(const [key,value] of Object.entries(patch)){
      if(!Object.prototype.hasOwnProperty.call(fields,key))throw new Error('Unsupported exposure control.');
      if(key==='mode'&&!['manual','auto'].includes(value))throw new Error('Choose Manual or Auto exposure.');
      const field=$(fields[key]);if(field.disabled)throw new Error('This exposure control is not active in the selected mode.');
      field.value=value;field.dispatchEvent(new Event('input',{bubbles:true}));field.dispatchEvent(new Event('change',{bubbles:true}));
    }
  },
  applyExposure(){const c=formControllers.get('exposureForm');if(!c?.loaded||c.busy)return;
    if(!validateSettings(c)){c.attempted=true;updateFormUi(c);return;}
    c.form.requestSubmit();
  },
  discardExposure(){$('exposureReset').click();},
  form(kind){const names={algorithm:['algorithmForm','algorithmError','algorithmSaveState'],encoding:['encodingForm','encodingError','encodingSaveState']};
    const [id,error,status]=names[kind],c=formControllers.get(id),fields={};
    c?.controls.forEach(input=>fields[input.id]={value:fieldValue(input),disabled:input.disabled,min:input.min,max:input.max,step:input.step});
    return {loaded:!!c?.loaded,busy:!!c?.busy,pending:c?changedControls(c).length:0,fields,errors:c?[...c.errors.values()]:[],error:$(error).textContent,status:$(status).textContent};
  },
  editForm(kind,id,value){const formId={algorithm:'algorithmForm',encoding:'encodingForm'}[kind],c=formControllers.get(formId);
    const field=c?.controls.find(input=>input.id===id);if(!field||!c.loaded||c.busy||field.disabled)return;
    if(field.type==='checkbox')field.checked=!!value;else field.value=value;
    field.dispatchEvent(new Event('input',{bubbles:true}));field.dispatchEvent(new Event('change',{bubbles:true}));
  },
  applyForm(kind){const c=formControllers.get({algorithm:'algorithmForm',encoding:'encodingForm'}[kind]);if(!c?.loaded||c.busy)return;
    if(!validateSettings(c)){c.attempted=true;updateFormUi(c);return;}c.form.requestSubmit();
  },
  discardForm(kind){$({algorithm:'algorithmReset',encoding:'encodingReset'}[kind]).click();},
  editOutputDraft(graph){const selected=window.NuvoPipeline.products(graph);
    document.querySelectorAll('[name="outputProduct"]').forEach(input=>input.checked=selected.includes(input.value));
    $('outputEnabled').checked=graph.outputs.some(o=>o.enabled);$('outputCadence').value=graph.cadence;$('outputDelivery').value=graph.delivery;
    $('outputPreviewEnabled').checked=graph.preview;$('outputPreviewCadence').value=graph.previewCadence;
    $('outputForm').dispatchEvent(new CustomEvent('input',{bubbles:true,detail:{pipeline:true}}));
  },
  outputDraft(){return {enabled:$('outputEnabled').checked,products:[...document.querySelectorAll('[name="outputProduct"]:checked')].map(i=>i.value),
    cadence_hz:Number($('outputCadence').value),delivery:$('outputDelivery').value,preview:$('outputPreviewEnabled').checked,preview_cadence_hz:Number($('outputPreviewCadence').value)};},
  applyOutputs(graph){
    if(!capabilities||!encoding)throw new Error('Wait for device capabilities before applying outputs.');
    const result=window.NuvoPipeline.compile(graph,this.context().limits);
    return commitOutputSelection({...outputSettings,enabled:result.enabled,products:result.products,cadence_hz:result.cadence_hz,delivery:result.delivery,preview:result.preview,preview_cadence_hz:result.preview_cadence_hz});
  },
  restartPreview(){restartStreams();},
  openSettings(page,group){if(group)try{sessionStorage.setItem('nuvo-open-settings-group',group);}catch(_){}location.href='/index.html#'+page;},
  draw(canvas,frame,descriptor){if(descriptor.kind==='depth')drawDepth(canvas,frame,descriptor);else if(descriptor.kind==='disparity')drawDisparity(canvas,frame,descriptor);else drawGray(canvas,frame,descriptor);}
});

  async function refreshConfiguration(kind){const [id,error,status]=names[kind],c=formControllers.get(id);try{const config=await(await api(D.DEFINITIONS[kind].path)).json();const first=!c.loaded,dirty=c.loaded&&changedControls(c).length>0,changed=c.saved&&JSON.stringify(c.saved)!==JSON.stringify(config);c.saved=config;if(kind==='encoding'){const previous=encoding;encoding=config;if(previous&&JSON.stringify(previous)!==JSON.stringify(config))restartStreams();}if(kind==='algorithm')stereo=config;
    if(!dirty&&!c.busy){baseline(c,config);if(first){const saved=D.loadDraft(kind);if(saved){for(const input of c.controls)if(Object.hasOwn(saved.fields,input.id)){if(input.type==='checkbox')input.checked=saved.fields[input.id];else input.value=saved.fields[input.id];}configurationRevisions.set(D.DEFINITIONS[kind].path,saved.revision);$(status).textContent='Restored pending changes';}else $(status).textContent='No pending changes';}}else if(changed)$(status).textContent='Device changed · review before applying';$(error).textContent='';updateFormUi(c);
    }catch(error){$(names[kind][1]).textContent=error.message;window.NuvoPipelineUI?.refresh();}}
  async function refreshHealth(){try{const health=await(await api('/api/v1/health')).json(),now=performance.now();depthOutputRate=lastRun===health.run_uuid&&lastPublished!==null&&now>lastHealthAt?Math.max(0,((health.matched_frames??health.published_frames)-lastPublished)*1000/(now-lastHealthAt)):null;if(health.matcher_active===false&&!(depthOutputRate>0))depthOutputRate=0;lastPublished=health.matched_frames??health.published_frames;lastRun=health.run_uuid;lastHealthAt=now;lastHealth=health;$('connection').textContent='Connected';$('connection').className='pill online';for(const id of ['manualExposure','autoMinExposure','autoMaxExposure'])$(id).max=health.exposure_safe_max_us||8000;}
    catch(_){$('connection').textContent='Not connected';$('connection').className='pill offline';depthOutputRate=null;}window.NuvoPipelineUI?.refresh();}
  async function refreshCapabilities(){try{capabilities=await(await api('/api/v1/capabilities')).json();window.NuvoPipelineUI?.refresh();}catch(_){}}
  let generation=0,sockets=[],decoder=null,decodeKey='',waitingKeyframe=true,lastEncodedSequence=0,abort=null;
  function active(encoded){return !document.hidden&&(encoded?outputSettings.preview&&encoding?.mode!=='raw'&&window.NuvoPipelineUI?.wantsEncoded():outputSettings.enabled&&encoding?.mode!=='encoded');}
  async function presentEncoded(buffer,version){const frame=C.parseEncoded(buffer),m=frame.metadata;if(version!==generation||!active(true)||m.codec!==encoding.codec||m.source!==encoding.source||m.encoding_applied_revision&&m.encoding_applied_revision!==configurationRevisions.get('/api/v1/encoding')?.replaceAll('"',''))return;
    if(m.codec==='jpeg'){const image=await createImageBitmap(new Blob([frame.payload],{type:'image/jpeg'}));if(version===generation)window.NuvoPipelineUI?.acceptEncoded(image,image.width,image.height);image.close();return;}
    if(!window.VideoDecoder)throw new Error('H.264 preview is unavailable in this browser. Choose MJPEG in Streaming.');
  const key=m.codec_string+':'+m.width+'x'+m.height;if(!decoder||decoder.state==='closed'||decodeKey!==key||lastEncodedSequence&&frame.streamSequence!==lastEncodedSequence+1){try{decoder?.close();}catch(_){}decodeKey=key;waitingKeyframe=true;decoder=new VideoDecoder({output:image=>{if(version===generation)window.NuvoPipelineUI?.acceptEncoded(image,image.displayWidth,image.displayHeight);image.close();},error:error=>{waitingKeyframe=true;window.NuvoPipelineUI?.streamError(error.message,'encoded');}});decoder.configure({codec:m.codec_string,codedWidth:m.width,codedHeight:m.height,optimizeForLatency:true,hardwareAcceleration:'prefer-hardware'});}lastEncodedSequence=frame.streamSequence;if(waitingKeyframe&&!m.keyframe)return;if(decoder.decodeQueueSize>2){waitingKeyframe=true;return;}decoder.decode(new EncodedVideoChunk({type:m.keyframe?'key':'delta',timestamp:Math.max(0,Math.floor(m.capture_time_ns/1000)),data:frame.payload}));waitingKeyframe=false;
  }
  async function present(buffer,encoded,version){if(version!==generation||!active(encoded))return;if(encoded)await presentEncoded(buffer,version);else window.NuvoPipelineUI?.acceptFrame(C.parseFrame(buffer));}
  function subscribe(encoded,version){if(!encoding||!active(encoded))return;const query=encoded?`cadence_hz=${outputSettings.preview_cadence_hz}`:`cadence_hz=${outputSettings.cadence_hz}&products=${outputSettings.products.join(',')}&delivery=${outputSettings.delivery}`,base=encoded?'/api/v1/encoded':'/api/v1',path=encoded?base+'/stream':base+'/stream',channel=encoded?'encoded':'raw';let polling=false;
    const report=error=>{if(version===generation)window.NuvoPipelineUI?.streamError(error.message,channel);};
    const poll=async()=>{if(version!==generation||!active(encoded))return;try{await present(await(await api((encoded?base+'/latest':base+'/frame/latest')+'?'+query,{signal:abort.signal})).arrayBuffer(),encoded,version);}catch(error){if(error.name!=='AbortError')report(error);}if(version===generation)setTimeout(poll,1000/(encoded?outputSettings.preview_cadence_hz:outputSettings.cadence_hz));};
    const socket=new WebSocket(`${location.protocol==='https:'?'wss':'ws'}://${location.host}${path}?${query}`);sockets.push(socket);socket.binaryType='arraybuffer';socket.onmessage=event=>{present(event.data,encoded,version).catch(report);};socket.onclose=()=>{if(version===generation&&!polling){polling=true;poll();}};
  }
  function restartStreams(){++generation;abort?.abort();abort=new AbortController();sockets.forEach(socket=>{socket.onclose=null;socket.close();});sockets=[];try{decoder?.close();}catch(_){}decoder=null;decodeKey='';lastEncodedSequence=0;window.NuvoPipelineUI?.resetPreview();subscribe(false,generation);subscribe(true,generation);}
  async function refresh(){await Promise.all([refreshHealth(),refreshCapabilities(),...Object.keys(names).map(refreshConfiguration)]);restartStreams();}
  $('claimForm').onsubmit=async event=>{event.preventDefault();try{const session=await(await api('/api/v1/session',{method:'POST',headers:{Authorization:'Bearer '+$('claimToken').value}})).json();csrf=session.csrf;$('claimToken').value='';dialog.close();await refresh();}catch(error){$('claimError').textContent=error.message;}};
  api('/api/v1/session').then(response=>response.json()).then(session=>{csrf=session.csrf;return refresh();}).catch(()=>{});
  setInterval(()=>{if(!document.hidden)refreshHealth();},2000);setInterval(()=>{if(!document.hidden){refreshCapabilities();Object.keys(names).forEach(refreshConfiguration);}},10000);
  document.addEventListener('visibilitychange',()=>{if(document.hidden)restartStreams();else refresh();});window.addEventListener('pagehide',()=>{++generation;abort?.abort();sockets.forEach(socket=>socket.close());try{decoder?.close();}catch(_){}decoder=null;});
  window.addEventListener('pageshow',event=>{if(event.persisted)refresh();});
})();
