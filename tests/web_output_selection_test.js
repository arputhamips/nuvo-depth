const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(require('node:path').join(__dirname, '../web/app.js'),'utf8');
const context = {TextDecoder, Uint8Array, DataView, Number, Set};
context.window={NuvoFrameCodec:require('../web/frame-codec.js'),NuvoDeviceSettings:require('../web/device-settings.js')};
vm.createContext(context);
vm.runInContext(source.slice(source.indexOf('const OUTPUT_PRODUCTS'),source.indexOf('let capabilities')),context);
vm.runInContext(source.slice(source.indexOf('function outputSubscriptionPath'),source.indexOf('function updateOutputBudget')),context);
vm.runInContext(source.slice(source.indexOf('function parseFrame('),source.indexOf('function product(')),context);
vm.runInContext(source.slice(source.indexOf('function drawGray('),source.indexOf('function drawDepth(')),context);
const settings={products:['left_rgb','validity'],cadence_hz:5,delivery:'best_effort'};
assert.equal(context.outputSubscriptionPath('/api/v1/stream',settings),'/api/v1/stream?cadence_hz=5&products=left_rgb,validity&delivery=best_effort');
const image=context.outputBandwidth(['left_mono'],728,544,5);
assert.equal(image.frame_bytes,728*544+65568);
assert.equal(context.outputBandwidth(['rectified_stereo_rgb'],728,544,5).bytes_per_second,(728*544*6+65568)*5);
assert.equal(context.outputBandwidth(['depth','validity','left_mono'],728,544,2,364,272).frame_bytes,728*544+364*272*5+65568,'image and matcher products use their own grids');
const presets=context.window.NuvoDeviceSettings.OUTPUT_RECIPES;
assert.deepEqual(presets.depth.products,['depth','validity']);
assert.equal(presets.camera.products.length,1,'camera default must not start matching');
assert(context.outputBandwidth(['left_rgb'],728,544,5).bytes_per_second<8*1024*1024);
assert(context.outputBandwidth(['left_rgb','right_rgb'],728,544,5).bytes_per_second>8*1024*1024);
function frame(format,data,kind='left_rgb'){
  const metadata=Buffer.from(JSON.stringify({schema_version:1,capture_epoch:1,capture_sequence:7,capture_time_ns:1000000000,configuration_epoch:1,calibration_sha256:'a'.repeat(64),calibration_authority:0,products:[{kind,format,width:2,height:1,offset:0,length:data.length}]}));
  const wire=Buffer.alloc(32+metadata.length+data.length);wire.write('NUV1');wire.writeUInt16BE(1,4);wire.writeUInt16BE(5,8);wire.writeUInt32BE(metadata.length,12);wire.writeBigUInt64BE(BigInt(data.length),16);wire.writeBigUInt64BE(7n,24);metadata.copy(wire,32);Buffer.from(data).copy(wire,32+metadata.length);
  return wire.buffer.slice(wire.byteOffset,wire.byteOffset+wire.byteLength);
}
let drawn;
const canvas={style:{},getContext:()=>({createImageData:(w,h)=>({data:new Uint8Array(w*h*4)}),putImageData:image=>drawn=image.data})};
for(const [format,expected] of [['rgb8',[11,22,33,255,44,55,66,255]],['bgr8',[33,22,11,255,66,55,44,255]]]){
  const parsed=context.parseFrame(frame(format,[11,22,33,44,55,66],'left_rectified'));context.drawGray(canvas,parsed,parsed.metadata.products[0]);
  assert.deepEqual(Array.from(drawn),expected);
}
assert.throws(()=>context.parseFrame(frame('rgb8',[11,22])),/Invalid product/,'short RGB payload must reject instead of showing partial pixels');
let allocations=0,resizes=0,writes=0;
function trackedCanvas(){
  let width=0,height=0;
  return {style:{},get width(){return width},set width(value){width=value;resizes++},
    get height(){return height},set height(value){height=value;resizes++},
    getContext:()=>({createImageData:(w,h)=>{allocations++;return {data:new Uint8Array(w*h*4)}},
      putImageData:image=>{drawn=Array.from(image.data);writes++}})};
}
const codec=context.window.NuvoFrameCodec,firstCanvas=trackedCanvas(),secondCanvas=trackedCanvas();
function depthFrame(values){const bytes=Buffer.alloc(values.length*4);values.forEach((z,i)=>bytes.writeFloatLE(z,i*4));return context.parseFrame(frame('float32_le',bytes,'depth'));}
const numeric=depthFrame([1,NaN]);
assert.equal(codec.drawDepth(firstCanvas,numeric,numeric.metadata.products[0]),50);
const numericPixels=drawn;
codec.drawDepth(secondCanvas,numeric,numeric.metadata.products[0]);
assert.equal(allocations,1,'two visible destinations share one colour map and ImageData');
assert.deepEqual(drawn,numericPixels);
codec.drawDepth(firstCanvas,numeric,numeric.metadata.products[0]);
assert.equal(writes,3,'canvas pixels are restored even if a caller reset its backing store');
const invalid=depthFrame([NaN,-1]);codec.drawDepth(firstCanvas,invalid,invalid.metadata.products[0]);
assert.equal(allocations,1,'same-size frames reuse image storage');
assert.equal(resizes,4,'steady frame dimensions do not reset canvas backing stores');
assert.deepEqual(drawn,[0,0,0,255,0,0,0,255],'invalid depth clears pixels from the prior frame');
const disparity=context.parseFrame(frame('float32_le',Buffer.from(new Float32Array([32,64]).buffer),'disparity'));
codec.drawDisparity(firstCanvas,disparity,disparity.metadata.products[0],128);const wide=drawn;
codec.drawDisparity(secondCanvas,disparity,disparity.metadata.products[0],64);assert.notDeepEqual(drawn,wide);
codec.drawDisparity(trackedCanvas(),disparity,disparity.metadata.products[0],128);assert.deepEqual(drawn,wide,'alternate disparity scales cannot poison cached colour maps');
const visibleContext={document:{hidden:false},window:{NuvoFrameCodec:{drawGray:()=>writes++}},stereo:null,$:()=>({})};
vm.createContext(visibleContext);
vm.runInContext(source.slice(source.indexOf('function drawGray('),source.indexOf('function renderFrame(')),visibleContext);
const visible={closest:selector=>selector==='.page.active'?{}:null};
const hidden={closest:selector=>selector==='[hidden]'?{}:selector==='.page.active'?{}:null};
const beforeVisible=writes;
visibleContext.drawVisibleProduct(hidden,numeric,{kind:'left_mono'});
visibleContext.document.hidden=true;visibleContext.drawVisibleProduct(visible,numeric,{kind:'left_mono'});
assert.equal(writes,beforeVisible,'hidden destinations and hidden tabs do no pixel work');
visibleContext.document.hidden=false;visibleContext.drawVisibleProduct(visible,numeric,{kind:'left_mono'});
assert.equal(writes,beforeVisible+1,'newly visible destination renders the current frame');
assert(source.includes("document.addEventListener('visibilitychange',restartStreams)"),'hidden tabs must release stream demand');
const consoleTimers=[],consoleListeners={},consoleCalls={health:0,settings:0,setup:0};
const consolePolling={setInterval:fn=>consoleTimers.push(fn),document:{hidden:false,addEventListener:(name,fn)=>consoleListeners[name]=fn},
  refreshHealth:()=>consoleCalls.health++,refreshTransport:()=>consoleCalls.settings++,refreshEncoding:()=>consoleCalls.settings++,
  refreshStereo:()=>consoleCalls.settings++,refreshExposure:()=>consoleCalls.settings++,refreshCapabilities:()=>consoleCalls.settings++,refreshSetupStatus:()=>consoleCalls.setup++};
const consoleStart=source.indexOf('setInterval(()=>{if(!document.hidden)refreshHealth();}');
vm.runInNewContext(source.slice(consoleStart,source.indexOf('\nsetInterval(()=>{if(lastFrameAt)',consoleStart)),consolePolling);
consoleTimers.forEach(fn=>fn());assert.deepEqual(consoleCalls,{health:1,settings:5,setup:1});
consolePolling.document.hidden=true;consoleTimers.forEach(fn=>fn());consoleListeners.visibilitychange();
assert.deepEqual(consoleCalls,{health:1,settings:5,setup:1},'hidden console must stop all routine endpoint polling');
consolePolling.document.hidden=false;consoleListeners.visibilitychange();assert.deepEqual(consoleCalls,{health:2,settings:10,setup:1});
assert(!source.includes("socketUrl('/api/v1/stream')"),'browser must not request implicit legacy-all depth products');
const emptyElements=Object.fromEntries(['viewerModeNotice','viewerEmptyState','encodedEmptyState','viewerPicture','selectedProductGrid','pictureMeasurementNotice','viewerPause','viewerRate','viewerReceiving','viewerSource','outputReceiving','outputSaveState','outputDestination'].map(id=>[id,{}]));
let encodedLive=true;
const availability={$:id=>emptyElements[id],lastFrameAt:0,encodedFrameAt:0,outputReception:null,viewerPaused:false,outputSettings:{enabled:true,preview:false,cadence_hz:5},encoding:{mode:'both'},
  refreshFirstDepth:()=>{},firstDepth:{count:2},setSampleCanvasLive:(id,live)=>{assert.equal(id,'encodedCanvas');encodedLive=live}};
vm.createContext(availability);
vm.runInContext(source.slice(source.indexOf('function updateViewerAvailability('),source.indexOf('function recordReceiving(')),availability);
availability.updateViewerAvailability();assert.equal(emptyElements.viewerEmptyState.hidden,false);
availability.lastFrameAt=100;availability.updateViewerAvailability();assert.equal(emptyElements.viewerEmptyState.hidden,true);
availability.updateViewerAvailability(true);assert.equal(emptyElements.viewerEmptyState.hidden,false);
assert.match(emptyElements.viewerEmptyState.textContent,/Disconnected.*last received/);assert.equal(encodedLive,false);
assert.equal(availability.firstDepth.count,undefined,'disconnect invalidates first-depth completion');
availability.encoding.mode='encoded';availability.updateViewerAvailability();
assert.equal(emptyElements.viewerModeNotice.hidden,false,'uncompressed selection explains why encoded-only mode cannot supply it');
availability.lastFrameAt=0;availability.outputSettings={enabled:false,preview:true,preview_cadence_hz:5};availability.encoding.mode='encoded';
availability.updateViewerAvailability();assert.equal(emptyElements.viewerPicture.hidden,false,'picture-only mode is supported by Live view');
assert.equal(emptyElements.viewerModeNotice.hidden,true,'selected picture preview must not prompt the user to select picture preview again');
assert.equal(emptyElements.selectedProductGrid.hidden,true);assert.match(emptyElements.viewerEmptyState.textContent,/shared picture/);
availability.encodedFrameAt=100;availability.updateViewerAvailability();assert.equal(emptyElements.viewerEmptyState.hidden,true);
availability.viewerPaused=true;availability.updateViewerAvailability();assert.match(emptyElements.viewerEmptyState.textContent,/Paused/);
const D=require('../web/device-settings.js');
const camera=D.outputRecipe('camera'),depth=D.outputRecipe('depth'),stereoRecipe=D.outputRecipe('stereo');
assert.equal(stereoRecipe.cadence_hz,1);assert.equal(D.outputRecipe('camera').cadence_hz,5,'camera defaults must not inherit the stereo cadence');
assert.equal(depth.cadence_hz,2);assert.equal(depth.preview,false,'depth does not inherit a redundant compressed subscription');
assert.deepEqual(D.normalizeOutputSelection({...camera,products:['depth']}).products,['depth','validity']);
assert.equal(D.outputRecipeName(D.outputRecipe('depth_reference')),'depth_reference','first depth is a named recipe');
const received={kind:'data',products:depth.products,label:'Depth values + valid pixels'};
const state=D.outputSelectionStatus(depth,camera,received);
assert.equal(state.dirty,true);assert.equal(state.requested,'Depth values + valid pixels');
assert.equal(state.receiving,'Depth values + valid pixels','editing Camera cannot relabel an arriving depth stream');
assert.equal(D.outputSelectionStatus(camera,camera,received).phase,'requested','an old depth frame cannot confirm a camera request');
assert.equal(D.outputSelectionStatus(depth,depth,received).dirty,false,'discard restores the requested baseline');
const beforeRecipe=JSON.stringify(depth);const recipe=D.outputApplicationRecipe(depth);
assert.match(recipe,/products=\("depth", "validity"\)/);assert.match(recipe,/cadence_hz=2/);assert.equal(JSON.stringify(depth),beforeRecipe);
assert.match(D.outputApplicationRecipe(D.outputRecipe('picture')),/latest_encoded\(\)/);
assert(!D.outputApplicationRecipe(D.outputRecipe('picture')).includes('latest_encoded(cadence_hz'),'SDK preview snapshot has no invented cadence parameter');
const freshness={viewerPaused:false,capabilities:{connection:{products_permitted:true}},outputReception:{at:1000,label:'Depth values'},outputSettings:depth,Date:{now:()=>2000}};
vm.createContext(freshness);vm.runInContext(source.slice(source.indexOf('function receivingStatus('),source.indexOf('function drawGray(')),freshness);
assert.match(freshness.receivingStatus(),/^Receiving:/);
freshness.Date.now=()=>10000;assert.match(freshness.receivingStatus(),/^Stale/);
freshness.viewerPaused=true;assert.match(freshness.receivingStatus(),/^Paused/);
freshness.viewerPaused=false;freshness.capabilities.connection={products_permitted:false,product_policy_reason:'Wi-Fi product permission is off'};
assert.match(freshness.receivingStatus(),/^Blocked.*Wi-Fi/);
const pageElements=new Map(),navigationPages=['overview','viewer','network','device'];
const navElements=navigationPages.map(page=>({dataset:{page},classList:{toggle(){}},setAttribute(){},removeAttribute(){}}));
function pageElement(id){if(!pageElements.has(id))pageElements.set(id,{id,open:false,classList:{toggle(){}},scrollIntoView(){}});return pageElements.get(id);}
const navigation={selectedPage:'',document:{querySelectorAll:selector=>selector==='.page'?navigationPages.map(pageElement):navElements},
  $:pageElement,placePerformanceMonitor:()=>{},restartStreams:()=>{},requestAnimationFrame:fn=>fn(),window:{scrollTo(){}},queueMicrotask:()=>{},refreshDiagnostics(){}};
vm.createContext(navigation);
vm.runInContext(source.slice(source.indexOf('const LEGACY_PAGES='),source.indexOf('function navigateToPage(')),navigation);
for(const id of ['', 'unknown', 'overview']){navigation.openPage(id);assert.equal(navigation.selectedPage,'overview','Overview is the default and explicit landing page');}
for(const id of navigationPages){navigation.openPage(id);assert.equal(navigation.selectedPage,id);}
for(const [id,parent] of Object.entries({exposure:'viewer',presets:'viewer',algorithm:'viewer',streaming:'device',calibration:'device',diagnostics:'device',recovery:'device'})){
  navigation.openPage(id);assert.equal(navigation.selectedPage,parent);assert.equal(pageElement(id).open,true,'old links disclose the correct settings without restoring separate pages');
}
const demand={document:{hidden:false},viewerPaused:false,capabilities:{connection:{products_permitted:true}},outputSettings:{enabled:true,preview:true},selectedPage:'overview',$:()=>({open:true})};
vm.createContext(demand);
vm.runInContext(source.slice(source.indexOf('function rawPreviewPage('),source.indexOf('function refreshFirstDepth(')),demand);
vm.runInContext(source.slice(source.indexOf('function encodedPreviewPage('),source.indexOf('function startEncodedViewer(')),demand);
assert.equal(demand.rawPreviewPage(),false,'Overview must not request image or numeric products');
assert.equal(demand.encodedPreviewPage(),false,'Overview must not request compressed pictures, even with calibration open elsewhere');
for(const page of ['viewer','network']){demand.selectedPage=page;assert.equal(demand.rawPreviewPage(),true);assert.equal(demand.encodedPreviewPage(),true);}
demand.selectedPage='device';assert.equal(demand.rawPreviewPage(),false);assert.equal(demand.encodedPreviewPage(),true,'calibration keeps its explicit positioning preview');
console.log('Web output recipes, requested/received state, image rendering and demand lifecycle tests passed');
