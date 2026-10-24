/* Shared device settings transport, validation, field definitions and edit-base drafts. */
(function(root,factory){const api=factory();if(typeof module==='object'&&module.exports)module.exports=api;else root.NuvoDeviceSettings=api;})(globalThis,function(){
  'use strict';
  const DEFINITIONS={
  "exposure": {
    "path": "/api/v1/exposure",
    "form": "exposureForm",
    "fields": {
      "exposureMode": {
        "key": "mode",
        "type": "select",
        "options": [
          {
            "value": "manual",
            "label": "Manual · constant"
          },
          {
            "value": "auto",
            "label": "Auto · stereo locked"
          }
        ]
      },
      "manualExposure": {
        "key": "manual_exposure_us",
        "type": "number",
        "min": "250",
        "max": "8000",
        "step": "50",
        "required": null
      },
      "manualGain": {
        "key": "manual_analogue_gain",
        "type": "number",
        "min": "1",
        "max": "3",
        "step": "0.05",
        "required": null
      },
      "redGain": {
        "key": "red_gain",
        "type": "number",
        "min": "0.1",
        "max": "16",
        "step": "0.0001",
        "required": null
      },
      "blueGain": {
        "key": "blue_gain",
        "type": "number",
        "min": "0.1",
        "max": "16",
        "step": "0.0001",
        "required": null
      },
      "autoTargetMedian": {
        "key": "auto_target_median",
        "type": "number",
        "min": "1",
        "max": "254",
        "step": "1",
        "required": null
      },
      "autoMinMedian": {
        "key": "auto_min_median",
        "type": "number",
        "min": "1",
        "max": "254",
        "step": "1",
        "required": null
      },
      "autoMaxMedian": {
        "key": "auto_max_median",
        "type": "number",
        "min": "1",
        "max": "254",
        "step": "1",
        "required": null
      },
      "autoMinExposure": {
        "key": "auto_min_exposure_us",
        "type": "number",
        "min": "250",
        "max": "8000",
        "step": "50",
        "required": null
      },
      "autoMaxExposure": {
        "key": "auto_max_exposure_us",
        "type": "number",
        "min": "250",
        "max": "8000",
        "step": "50",
        "required": null
      },
      "autoMinGain": {
        "key": "auto_min_analogue_gain",
        "type": "number",
        "min": "1",
        "max": "3",
        "step": "0.05",
        "required": null
      },
      "autoMaxGain": {
        "key": "auto_max_analogue_gain",
        "type": "number",
        "min": "1",
        "max": "3",
        "step": "0.05",
        "required": null
      },
      "autoUpdateInterval": {
        "key": "auto_update_interval_frames",
        "type": "number",
        "min": "1",
        "max": "120",
        "step": "1",
        "required": null
      },
      "autoSettleFrames": {
        "key": "auto_settle_frames_after_update",
        "type": "number",
        "min": "0",
        "max": "120",
        "step": "1",
        "required": null
      }
    }
  },
  "algorithm": {
    "path": "/api/v1/stereo",
    "form": "algorithmForm",
    "fields": {
      "algorithmType": {
        "key": "algorithm",
        "type": "select",
        "options": [
          {
            "value": "sgbm",
            "label": "Semi-global block matching (SGBM)"
          },
          {
            "value": "bm",
            "label": "Block matching (StereoBM)"
          }
        ]
      },
      "sgbmMode": {
        "key": "sgbm_mode",
        "type": "select",
        "options": [
          {
            "value": "3way",
            "label": "3-way · fastest"
          },
          {
            "value": "standard",
            "label": "Standard · 5 paths"
          },
          {
            "value": "hh4",
            "label": "HH4 · 4 full paths"
          },
          {
            "value": "hh",
            "label": "HH · 8 directions / cleanest"
          }
        ]
      },
      "minDisparity": {
        "key": "min_disparity",
        "type": "number",
        "min": "0",
        "max": "512",
        "step": "1",
        "required": null
      },
      "searchRange": {
        "key": "search_range",
        "type": "number",
        "min": "16",
        "max": "512",
        "step": "16",
        "required": null
      },
      "maxDisparity": {
        "key": "max_disparity",
        "type": "number",
        "min": "1",
        "max": "727",
        "step": "1",
        "required": null
      },
      "blockSize": {
        "key": "block_size",
        "type": "number",
        "min": "5",
        "max": "31",
        "step": "2",
        "required": null
      },
      "uniquenessRatio": {
        "key": "uniqueness_ratio",
        "type": "number",
        "min": "0",
        "max": "100",
        "required": null
      },
      "speckleWindow": {
        "key": "speckle_window_size",
        "type": "number",
        "min": "0",
        "max": "10000",
        "required": null
      },
      "speckleRange": {
        "key": "speckle_range",
        "type": "number",
        "min": "0",
        "max": "256",
        "required": null
      },
      "disparity12Diff": {
        "key": "disparity_12_max_diff",
        "type": "number",
        "min": "-1",
        "max": "256",
        "required": null
      },
      "preFilterCap": {
        "key": "pre_filter_cap",
        "type": "number",
        "min": "1",
        "max": "63",
        "required": null
      },
      "bmPreFilterSize": {
        "key": "bm_pre_filter_size",
        "type": "number",
        "min": "5",
        "max": "255",
        "step": "2",
        "required": null
      },
      "bmTextureThreshold": {
        "key": "bm_texture_threshold",
        "type": "number",
        "min": "0",
        "max": "255",
        "required": null
      },
      "specularThreshold": {
        "key": "specular_threshold",
        "type": "number",
        "min": "1",
        "max": "256",
        "required": null
      },
      "medianFilterSize": {
        "key": "median_filter_size",
        "type": "select",
        "options": [
          {
            "value": "0",
            "label": "No additional filter"
          },
          {
            "value": "3",
            "label": "Additional 3 × 3"
          },
          {
            "value": "5",
            "label": "5 × 5 · strongest"
          }
        ]
      },
      "algorithmThreads": {
        "key": "opencv_threads",
        "type": "select",
        "options": [
          {
            "value": "1",
            "label": "1"
          },
          {
            "value": "2",
            "label": "2"
          },
          {
            "value": "3",
            "label": "3"
          },
          {
            "value": "4",
            "label": "4 · all Pi 5 cores"
          }
        ]
      }
    }
  },
  "encoding": {
    "path": "/api/v1/encoding",
    "form": "encodingForm",
    "fields": {
      "streamCodec": {
        "key": "codec",
        "type": "select",
        "options": [
          {
            "value": "h264",
            "label": "H.264 · independent frames"
          },
          {
            "value": "jpeg",
            "label": "MJPEG · universal"
          }
        ]
      },
      "streamSource": {
        "key": "source",
        "type": "select",
        "options": [
          {
            "value": "left_color",
            "label": "Left color · ISP"
          },
          {
            "value": "right_color",
            "label": "Right color · ISP"
          },
          {
            "value": "left_rectified",
            "label": "Left rectified · luma"
          },
          {
            "value": "right_rectified",
            "label": "Right rectified · luma"
          },
          {
            "value": "disparity",
            "label": "Disparity preview · 8-bit visualization"
          },
          {
            "value": "depth",
            "label": "Depth preview · 8-bit visualization, 0.2–12.2 m"
          },
          {
            "value": "left_mono",
            "label": "Left mono · unrectified"
          },
          {
            "value": "right_mono",
            "label": "Right mono · unrectified"
          },
          {
            "value": "left_rgb",
            "label": "Left RGB · unrectified"
          },
          {
            "value": "right_rgb",
            "label": "Right RGB · unrectified"
          },
          {
            "value": "left_rectified_rgb",
            "label": "Left RGB · rectified"
          },
          {
            "value": "right_rectified_rgb",
            "label": "Right RGB · rectified"
          },
          {
            "value": "rectified_stereo",
            "label": "Stereo side by side · mono"
          },
          {
            "value": "rectified_stereo_rgb",
            "label": "Stereo side by side · RGB"
          }
        ]
      },
      "streamWidth": {
        "key": "width",
        "type": "number",
        "min": "0",
        "max": "1920",
        "step": "2",
        "required": null
      },
      "streamHeight": {
        "key": "height",
        "type": "number",
        "min": "0",
        "max": "1080",
        "step": "2",
        "required": null
      },
      "streamFps": {
        "key": "cadence_hz",
        "type": "number",
        "min": "1",
        "max": "60",
        "required": null
      },
      "streamBitrate": {
        "key": "bitrate_bps",
        "type": "number",
        "min": "100000",
        "max": "50000000",
        "step": "100000",
        "required": null
      },
      "streamQuality": {
        "key": "jpeg_quality",
        "type": "number",
        "min": "1",
        "max": "100",
        "required": null
      },
      "streamKeyframes": {
        "key": "keyframe_interval",
        "type": "number",
        "min": "1",
        "max": "600",
        "step": "1",
        "required": null
      },
      "streamThreads": {
        "key": "threads",
        "type": "select",
        "options": [
          {
            "value": "1",
            "label": "1"
          },
          {
            "value": "2",
            "label": "2"
          },
          {
            "value": "3",
            "label": "3"
          },
          {
            "value": "4",
            "label": "4"
          }
        ]
      },
      "streamLowLatency": {
        "key": "low_latency",
        "type": "checkbox"
      },
      "streamPreset": {
        "key": "preset",
        "type": "select",
        "options": [
          {
            "value": "ultrafast",
            "label": "Ultra-fast"
          },
          {
            "value": "superfast",
            "label": "Super-fast"
          },
          {
            "value": "veryfast",
            "label": "Very fast"
          },
          {
            "value": "faster",
            "label": "Faster"
          },
          {
            "value": "fast",
            "label": "Fast / higher quality"
          }
        ]
      },
      "streamEncoderEnabled": {
        "type": "checkbox"
      },
      "streamRawEnabled": {
        "type": "checkbox"
      }
    }
  }
};
  const numeric=field=>field.type==='number'||field.type==='select'&&field.options.every(option=>/^-?\d+(?:\.\d+)?$/.test(option.value));
  function validate(kind,config,limits={}){
    const errors=new Map(),definition=DEFINITIONS[kind];if(!definition||!config||config.schema_version!==1){errors.set('schema','Unsupported settings schema.');return errors;}
    for(const [id,field] of Object.entries(definition.fields)){
      if(!field.key)continue;const value=config[field.key];
      if(field.type==='checkbox'){if(typeof value!=='boolean')errors.set(id,'Choose an enabled state.');continue;}
      if(field.type==='select'){if(!field.options.some(o=>o.value===String(value))||numeric(field)&&typeof value!=='number')errors.set(id,'Choose a supported '+field.key.replaceAll('_',' ')+'.');continue;}
      if(!Number.isFinite(value)||field.min!==undefined&&value<Number(field.min)||field.max!==undefined&&value>Number(field.max))errors.set(id,field.key.replaceAll('_',' ')+' is outside its allowed range.');
      else if(field.step&&field.step!=='any'&&Math.abs((value-Number(field.min||0))/Number(field.step)-Math.round((value-Number(field.min||0))/Number(field.step)))>1e-6)errors.set(id,'Use a valid step for '+field.key.replaceAll('_',' ')+'.');
    }
    if(kind==='algorithm'&&(config.min_disparity>=config.max_disparity||config.max_disparity>config.min_disparity+config.search_range))errors.set('maxDisparity','Maximum must lie inside the search range.');
    if(kind==='encoding'){
      if(config.mode!=='raw'&&config.codec==='h264'&&config.keyframe_interval!==1)errors.set('streamKeyframes','Live H.264 pictures require an independent keyframe every frame.');
      if(!['raw','both','encoded'].includes(config.mode))errors.set('streamRawEnabled','Keep uncompressed network data or the compressed preview encoder enabled.');
      if((config.width===0)!==(config.height===0))errors.set('streamWidth','Set both dimensions, or leave both at 0 for native resolution.');
    }
    if(kind==='exposure'){
      const max=limits.safeExposureUs||8000;
      if(config.manual_exposure_us>max||config.auto_max_exposure_us>max)errors.set('manualExposure',`Exposure must not exceed the live ${max} µs trigger-period limit.`);
      if(config.auto_min_median>config.auto_target_median||config.auto_target_median>config.auto_max_median)errors.set('autoTargetMedian','Target median must be inside the Auto deadband.');
      if(config.auto_min_exposure_us>config.auto_max_exposure_us||config.auto_min_analogue_gain>config.auto_max_analogue_gain)errors.set('autoMaxExposure','Auto minimums must not exceed maximums.');
    }
    return errors;
  }
  function outputFrameBytes(products,limits={},productTypes=[]){
    const bytes=new Map(productTypes.map(([id,,size])=>[id,size]));
    const width=limits.width??728,height=limits.height??544;
    const matchingWidth=limits.matchingWidth??width,matchingHeight=limits.matchingHeight??height;
    if(![width,height,matchingWidth,matchingHeight].every(x=>Number.isSafeInteger(x)&&x>0&&x<=8192))throw new Error('Device dimensions are unavailable or outside supported bounds.');
    return 65568+products.reduce((sum,product)=>{
      if(!bytes.has(product))throw new Error('This source is not a supported camera product.');
      const pixels=['depth','disparity','confidence','validity'].includes(product)?matchingWidth*matchingHeight:width*height;
      return sum+pixels*bytes.get(product);
    },0);
  }
  function validateOutputSelection(value,limits={},productTypes=[]){
    if(!value||typeof value.enabled!=='boolean'||!Array.isArray(value.products)||value.products.length>14)throw new Error('Choose a valid data output selection.');
    const bytes=new Map(productTypes.map(([id,,size])=>[id,size]));
    if(value.products.some(product=>typeof product!=='string'||!bytes.has(product)))throw new Error('This source is not a supported camera product.');
    if(!Number.isFinite(value.cadence_hz)||value.cadence_hz<=0||value.cadence_hz>60)throw new Error('Output cadence must be greater than 0 and at most 60 Hz.');
    if(!['auto','best_effort','exact'].includes(value.delivery))throw new Error('Choose a supported delivery policy.');
    if(typeof value.preview!=='boolean'||!Number.isFinite(value.preview_cadence_hz)||value.preview_cadence_hz<=0||value.preview_cadence_hz>60)throw new Error('Use a valid monitoring state and cadence from 0–60 Hz, greater than zero.');
    if(value.enabled&&!value.products.length)throw new Error('Select at least one data product.');
    const products=value.enabled?[...new Set(value.products)]:[];
    if(!products.length)return {...value,enabled:false,products,frameBytes:0,bytesPerSecond:0};
    if(Array.isArray(limits.supportedProducts)&&products.some(product=>!limits.supportedProducts.includes(product)))throw new Error('A selected product is not supported by this device.');
    if(value.delivery==='exact'&&!limits.qualified)throw new Error('Exact delivery requires a measured, qualified connection. Choose Auto or Best effort.');
    if(limits.rawEnabled===false)throw new Error('Uncompressed network data is off. Enable it in Device → Advanced preview encoder, or choose the shared picture preview.');
    const frameBytes=outputFrameBytes(products,limits,productTypes);
    const maxFrame=Number.isFinite(limits.maxFrameBytes)&&limits.maxFrameBytes>0?limits.maxFrameBytes:16777216;
    const budget=Number.isFinite(limits.bytesPerSecond)&&limits.bytesPerSecond>0?limits.bytesPerSecond:8*1048576;
    if(frameBytes>maxFrame)throw new Error('Selected products exceed the data frame size limit. Choose fewer products.');
    const bytesPerSecond=frameBytes*value.cadence_hz;
    if(bytesPerSecond>budget){const safe=Math.floor(budget/frameBytes*10)/10;throw new Error(`Outputs exceed the connection resource budget. Use ${safe>0?safe+' Hz or less':'fewer products'}, or choose fewer products.`);}
    return {...value,enabled:true,products,frameBytes,bytesPerSecond};
  }
  function values(kind,config){const result={};for(const [id,f] of Object.entries(DEFINITIONS[kind].fields))result[id]=f.key?config[f.key]:id==='streamEncoderEnabled'?config.mode!=='raw':config.mode!=='encoded';return result;}
  function configuration(kind,fields,base){const result={...base,schema_version:1};for(const [id,f] of Object.entries(DEFINITIONS[kind].fields))if(f.key)result[f.key]=numeric(f)?Number(fields[id]):fields[id];
    if(kind==='encoding'){result.mode=fields.streamEncoderEnabled?(fields.streamRawEnabled?'both':'encoded'):(fields.streamRawEnabled?'raw':'');result.keyframe_interval=1;}return result;}
  const draftKey=path=>'nuvo-settings-draft:'+path;
  function clearDraft(path){try{sessionStorage.removeItem(draftKey(path));}catch(_){}}
  function saveDraft(path,fields,revision){try{sessionStorage.setItem(draftKey(path),JSON.stringify({version:1,fields,revision}));}catch(_){}}
  function loadDraft(kind){try{const text=sessionStorage.getItem(draftKey(DEFINITIONS[kind].path));if(!text||text.length>16384)return null;const saved=JSON.parse(text);if(saved.version!==1||!/^"[a-f0-9]{64}"$/.test(saved.revision)||!saved.fields||Object.keys(saved.fields).some(id=>!Object.hasOwn(DEFINITIONS[kind].fields,id))||Object.values(saved.fields).some(v=>!['string','number','boolean'].includes(typeof v)))return null;return saved;}catch(_){return null;}}
  async function request(path, options = {}, state) {
  const {csrf,configurationRevisions,latestConfigurationRevisions,encodingFormDirty,algorithmFormDirty,exposureFormDirty}=state;
  const method = options.method || 'GET';
  const headers = new Headers(options.headers || {});
  if (!['GET','HEAD'].includes(method) && path !== '/api/v1/session' && csrf) headers.set('X-Nuvo-CSRF',csrf);
  const configurationPath = ['/api/v1/encoding','/api/v1/stereo','/api/v1/exposure'].includes(path);
  if(configurationPath && method==='POST'){
    const revision=configurationRevisions.get(path);
    if(!revision)throw new Error('Load device settings before saving');
    headers.set('If-Match',revision);
    const kind=Object.keys(DEFINITIONS).find(kind=>DEFINITIONS[kind].path===path);
    const errors=validate(kind,JSON.parse(options.body),{safeExposureUs:state.safeExposureUs});
    if(errors.size)throw new Error([...errors.values()].join(' '));
  }
  const response = await (state.fetch||fetch)(path, {...options, method, headers, credentials:'same-origin', cache:'no-store'});
  if (response.status === 401) { state.onUnauthorized?.(); const error=new Error('Device session expired'); error.status=401; throw error; }
  if (!response.ok) {
    let detail = `${response.status} ${response.statusText}`;
    try { const value = await response.json(); detail = value.error || detail; } catch (_) {}
    throw new Error(detail);
  }
  if(configurationPath){
    const revision=response.headers.get('ETag');
    if(!/^"[a-f0-9]{64}"$/.test(revision||''))throw new Error('Device settings revision is unavailable');
    latestConfigurationRevisions.set(path,revision);
    const dirty=path==='/api/v1/encoding'?encodingFormDirty:path==='/api/v1/stereo'?algorithmFormDirty:exposureFormDirty;
    // Polling must retain the revision from which the unsaved form was edited.
    if(method==='POST'||!dirty)configurationRevisions.set(path,revision);
    if(method==='POST')clearDraft(path);
  }
  return response;
}


  const OUTPUT_RECIPES=Object.freeze({
    camera:{label:'Camera image',products:['left_mono'],cadence_hz:5},
    depth:{label:'Depth values + valid pixels',products:['depth','validity'],cadence_hz:2},
    depth_reference:{label:'Depth with camera reference',products:['left_rectified','depth','validity'],cadence_hz:1},
    stereo:{label:'Stereo check',products:['left_rectified','right_rectified','disparity','validity'],cadence_hz:1},
    picture:{label:'Shared picture preview',products:[],cadence_hz:5}
  });
  function outputRecipe(name){
    const recipe=OUTPUT_RECIPES[name];if(!recipe)throw new Error('Choose a named output recipe.');
    return {enabled:name!=='picture',products:[...recipe.products],cadence_hz:recipe.cadence_hz,
      delivery:'best_effort',preview:name==='picture',preview_cadence_hz:recipe.cadence_hz};
  }
  function normalizeOutputSelection(value){
    const products=[...new Set(value.products)];
    if(products.includes('depth')&&!products.includes('validity'))products.push('validity');
    return {...value,products};
  }
  function outputRecipeName(value){
    if(!value.enabled&&value.preview)return 'picture';
    return Object.keys(OUTPUT_RECIPES).find(name=>name!=='picture'&&OUTPUT_RECIPES[name].products.length===value.products.length&&OUTPUT_RECIPES[name].products.every(p=>value.products.includes(p)))||'custom';
  }
  function outputSelectionLabel(value){
    return OUTPUT_RECIPES[outputRecipeName(value)]?.label||value.products.join(' + ');
  }
  function outputSelectionStatus(requested,draft,received){
    const key=value=>JSON.stringify([value.enabled,[...value.products].sort(),value.cadence_hz,value.delivery,value.preview,value.preview_cadence_hz]);
    const matches=received&&(requested.preview&&!requested.enabled?received.kind==='picture':received.kind==='data'&&requested.products.every(p=>received.products.includes(p)));
    return {dirty:key(requested)!==key(draft),phase:matches?'receiving':'requested',
      requested:outputSelectionLabel(requested),receiving:received?.label||null};
  }
  const DELIVERY_PATHS=Object.freeze({ethernet:'Ethernet port',usb_device:'USB-C device network',wifi:'Wi-Fi'});
  function deliveryPathModel(transport,path){
    const entry=transport?.paths?.[path];
    if(!entry)return {discovered:false,addresses:[],ready:false,reason:'Path discovery unavailable. This recipe uses the current console address.'};
    const addresses=[];
    for(const network of entry.interfaces||[]){
      if(network.admin_up!==true||network.carrier!==true)continue;
      for(const ip of network.addresses||[]){
        if(ip.gateway_listening!==true||typeof ip.address!=='string'||typeof network.name!=='string')continue;
        const ipv6=ip.family==='ipv6'||ip.address.includes(':');
        const linkLocal=ipv6&&(parseInt(ip.address.split(':')[0],16)&0xffc0)===0xfe80;
        if(ip.scope_required===true||linkLocal||ip.address.includes('%'))continue;
        addresses.push({address:ip.address,interface:network.name});
      }
    }
    const states={unavailable:path==='usb_device'?'USB device network setup required':'Interface unavailable',disconnected:'No active link',no_address:'No address served by this gateway',unknown:'Readiness unknown',available:'Camera link and gateway address ready'};
    const permitted=entry.products_permitted===true;
    const reason=(entry.state==='available'&&!addresses.length?'No usable address for application code':states[entry.state]||'Readiness unknown')+(!permitted?' · product delivery blocked by device permissions':'');
    return {discovered:true,addresses,ready:entry.state==='available'&&addresses.length>0&&permitted,reason,state:entry.state,permitted};
  }
  function wiredTransportPatch(preference,transport){
    if(!['auto','ethernet','usb','both','wifi'].includes(preference))throw new Error('Choose a supported wired preference. Turning all outputs off requires local device confirmation.');
    if(typeof transport?.wifi_enabled!=='boolean')throw new Error('Read the current Wi-Fi permission before changing wired delivery.');
    return {preference,wifi_enabled:transport.wifi_enabled};
  }
  function outputApplicationRecipe(value,target){
    const rate=value.enabled?value.cadence_hz:value.preview_cadence_hz;
    const products=value.products.map(p=>JSON.stringify(p)).join(', ')+(value.products.length===1?',':'');
    const body=!value.enabled&&value.preview?'# Snapshot only; latest_encoded() does not accept a cadence option.\nframe = client.latest_encoded()\n# JPEG / H.264 picture from the shared source; numeric depth is unavailable.\n# For a persistent stream: /api/v1/encoded/stream?cadence_hz='+rate:
      'with client.frames(products=('+products+'), cadence_hz='+rate+',\n                   delivery='+JSON.stringify(value.delivery)+') as frames:\n    for frame in frames:\n        print(frame.capture_sequence, frame.products.keys())'+(value.products.includes('depth')?'\n        # Accept depth only where validity == 1 and Z is finite and positive.':'');
    if(!target)return '# Use an authenticated NuvoClient with verified TLS.\n'+body;
    const direct=target.address&&target.interface&&DELIVERY_PATHS[target.path];
    const secure=String(target.origin).startsWith('https://');
    const origin=secure?JSON.stringify(target.origin):'os.environ["NUVO_DEVICE_URL"]';
    let code='import os\nfrom nuvo_depth import NuvoClient\n\n# Trust the device certificate; keep hostname verification enabled.\n';
    if(!secure)code+='# This development console uses HTTP. Set NUVO_DEVICE_URL to the verified HTTPS device URL.\n';
    if(!direct)code+='# Path discovery unavailable: the current console URL does not select a physical interface.\n';
    code+='with NuvoClient('+origin+', ca_file="device.crt"'+(direct?', connect_address='+JSON.stringify(target.address):'')+') as client:\n    client.claim(os.environ["NUVO_TOKEN"])\n';
    if(direct){
      code+='    connection = client.capabilities()["connection"]\n';
      for(const [key,expected] of [['path',target.path],['interface',target.interface],['local_address',target.address]])code+='    if connection.get('+JSON.stringify(key)+') != '+JSON.stringify(expected)+':\n        raise RuntimeError("Connected device '+key+' does not match the selected delivery path")\n';
    }
    return code+body.split('\n').map(line=>'    '+line).join('\n');
  }
  function pictureQualityConfiguration(config,choice){
    const preset={compact:{jpeg_quality:45,bitrate_bps:1000000},balanced:{jpeg_quality:70,bitrate_bps:4000000},detail:{jpeg_quality:90,bitrate_bps:8000000}}[choice];
    if(!preset||!['jpeg','h264'].includes(config?.codec))throw new Error('Choose a supported picture quality.');
    return {...config,...(config.codec==='jpeg'?{jpeg_quality:preset.jpeg_quality}:{bitrate_bps:preset.bitrate_bps})};
  }
  return Object.freeze({DEFINITIONS,validate,validateOutputSelection,outputFrameBytes,values,configuration,request,saveDraft,loadDraft,clearDraft,
    OUTPUT_RECIPES,outputRecipe,normalizeOutputSelection,outputRecipeName,outputSelectionLabel,outputSelectionStatus,outputApplicationRecipe,DELIVERY_PATHS,deliveryPathModel,wiredTransportPatch,pictureQualityConfiguration});
});
