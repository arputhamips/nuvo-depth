const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const D=require('../web/device-settings.js');
const configs={};
for(const kind of ['exposure','algorithm','encoding']){
  const config=JSON.parse(fs.readFileSync(path.join(__dirname,'../config/'+(kind==='algorithm'?'stereo':kind)+'.json'),'utf8'));configs[kind]=config;
  assert.equal(D.validate(kind,config).size,0,kind+' defaults must pass the shared validator');
  const fields=D.values(kind,config),nativeFields=Object.fromEntries(Object.entries(fields).map(([id,value])=>[id,typeof value==='boolean'?value:String(value)]));
  const roundtrip=D.configuration(kind,nativeFields,config);
  assert.deepEqual(roundtrip,{...config,...(kind==='encoding'?{keyframe_interval:1}:{})},'native select controls must retain numeric JSON types');
}
assert(D.validate('algorithm',{...configs.algorithm,opencv_threads:'2'}).has('algorithmThreads'));
assert(D.validate('algorithm',{...configs.algorithm,search_range:64,max_disparity:128}).has('maxDisparity'));
assert(D.validate('exposure',configs.exposure,{safeExposureUs:4000}).has('manualExposure'));
assert(D.validate('encoding',{...configs.encoding,mode:''}).has('streamRawEnabled'));
assert(D.validate('encoding',{...configs.encoding,width:320,height:0}).has('streamWidth'));
const storage=new Map();global.sessionStorage={setItem:(key,value)=>storage.set(key,value),getItem:key=>storage.get(key),removeItem:key=>storage.delete(key)};
const revision='"'+'a'.repeat(64)+'"',route='/api/v1/exposure',fields=D.values('exposure',configs.exposure);
D.saveDraft(route,fields,revision);assert.deepEqual(D.loadDraft('exposure'),{version:1,fields,revision});D.clearDraft(route);assert.equal(D.loadDraft('exposure'),null);
D.saveDraft(route,{unknown:'injected'},revision);assert.equal(D.loadDraft('exposure'),null);
// The conventional Outputs page must retain admission checks without loading the prototype.
const vm=require('node:vm'),app=fs.readFileSync(path.join(__dirname,'../web/app.js'),'utf8');
const products=vm.runInNewContext(app.slice(app.indexOf('const OUTPUT_PRODUCTS'),app.indexOf('let capabilities'))+'; OUTPUT_PRODUCTS',{window:{NuvoDeviceSettings:D}});
const selection={enabled:true,products:['left_mono'],cadence_hz:5,delivery:'auto',preview:true,preview_cadence_hz:5};
const check=(overrides={},limits={})=>D.validateOutputSelection({...selection,...overrides},limits,products);
assert.equal(check().frameBytes,728*544+65568);
assert.deepEqual(check({products:['left_mono','left_mono']}).products,['left_mono']);
assert.deepEqual(check({enabled:false},{rawEnabled:false}).products,[]);
for(const candidate of [{products:[]},{products:['unknown']},{enabled:'true'},{cadence_hz:NaN},{cadence_hz:61},{preview:'true'},{preview_cadence_hz:0},{delivery:'unsupported'}])assert.throws(()=>check(candidate));
assert.throws(()=>check({}, {supportedProducts:['depth']}),/not supported/);
assert.throws(()=>check({}, {rawEnabled:false}),/Uncompressed network data is off/);
assert.throws(()=>check({delivery:'exact'}),/qualified/);
assert.equal(check({delivery:'exact'},{qualified:true}).delivery,'exact');
assert.throws(()=>check({products:['left_rgb','right_rgb']}),/budget/);
assert.throws(()=>check({}, {maxFrameBytes:100}),/frame size/);
assert.throws(()=>check({}, {width:0}),/dimensions/);
assert.equal(check({products:['depth','validity'],cadence_hz:1},{matchingWidth:364,matchingHeight:272}).frameBytes,364*272*5+65568);
assert.throws(()=>check({products:['depth']},{matchingWidth:0}),/dimensions/);
for(const [product,,bytesPerPixel] of products){
  const candidate={...selection,products:[product],cadence_hz:1};
  const actual=D.validateOutputSelection(candidate,{},products);
  assert.equal(actual.frameBytes,728*544*bytesPerPixel+65568,product+' transport size');
  assert.equal(actual.bytesPerSecond,actual.frameBytes,product+' cadence');
  for(const field of ['enabled','cadence_hz','delivery','preview','preview_cadence_hz'])assert.equal(actual[field],candidate[field],product+' '+field);
  assert.deepEqual(actual.products,[product]);
}
console.log('Shared settings numeric types, limits, cross-field constraints and edit-base draft tests passed');
for(const codec of ['jpeg','h264']){
  const original={...configs.encoding,codec,source:'depth',mode:'both',cadence_hz:3,width:640,height:480,keyframe_interval:codec==='jpeg'?30:1};
  for(const choice of ['compact','balanced','detail']){
    const selected=D.pictureQualityConfiguration(original,choice),field=codec==='jpeg'?'jpeg_quality':'bitrate_bps';
    assert.deepEqual(Object.fromEntries(Object.entries(selected).filter(([key])=>key!==field)),Object.fromEntries(Object.entries(original).filter(([key])=>key!==field)),'shared quality must preserve source, codec, dimensions, delivery mode and rate');
    assert.equal(D.validate('encoding',selected).size,0);
  }
}
assert.equal(D.pictureQualityConfiguration({...configs.encoding,codec:'jpeg'},'detail').jpeg_quality,90);
assert.equal(D.pictureQualityConfiguration({...configs.encoding,codec:'h264'},'compact').bitrate_bps,1000000);
assert.throws(()=>D.pictureQualityConfiguration(configs.encoding,'unmeasured'));

for(const keyframe_interval of [1,30,600])assert.equal(D.validate('encoding',{...configs.encoding,codec:'jpeg',keyframe_interval}).size,0,'JPEG quality accepts the native keyframe field without changing it');
for(const keyframe_interval of [0,1.5,601])assert(D.validate('encoding',{...configs.encoding,codec:'jpeg',keyframe_interval}).has('streamKeyframes'));
assert(D.validate('encoding',{...configs.encoding,codec:'h264',mode:'both',keyframe_interval:30}).has('streamKeyframes'));
assert.equal(D.validate('encoding',{...configs.encoding,codec:'h264',mode:'raw',keyframe_interval:30}).size,0,'inactive H264 matches native configuration validation');
