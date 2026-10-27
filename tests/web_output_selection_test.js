const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(require('node:path').join(__dirname, '../web/app.js'),'utf8');
const context = {TextDecoder, Uint8Array, DataView, Number, Set};
context.window={NuvoFrameCodec:require('../web/frame-codec.js'),NuvoDeviceSettings:require('../web/device-settings.js')};
vm.createContext(context);
vm.runInContext(source.slice(source.indexOf('const OUTPUT_PRODUCTS'),source.indexOf('let capabilities')),context);
vm.runInContext(source.slice(source.indexOf('function outputBandwidth('),source.indexOf('function outputDraft(')),context);
const image=context.outputBandwidth(['left_mono'],728,544,5);
assert.equal(image.frame_bytes,728*544+65568);
assert.equal(context.outputBandwidth(['rectified_stereo_rgb'],728,544,5).bytes_per_second,(728*544*6+65568)*5);
assert.equal(context.outputBandwidth(['depth','validity','left_mono'],728,544,2,364,272).frame_bytes,728*544+364*272*5+65568,'image and matcher products use their own grids');
const presets=context.window.NuvoDeviceSettings.OUTPUT_RECIPES;
assert.deepEqual(presets.depth.products,['depth','validity']);
assert.equal(presets.camera.products.length,1,'camera default must not start matching');
assert(context.outputBandwidth(['left_rgb'],728,544,5).bytes_per_second<8*1024*1024);
assert(context.outputBandwidth(['left_rgb','right_rgb'],728,544,5).bytes_per_second>8*1024*1024);
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
assert.match(D.outputApplicationRecipe(D.outputRecipe('picture')),/encoded_frames\(cadence_hz=5\)/);
assert(!D.outputApplicationRecipe(D.outputRecipe('picture')).includes('latest_encoded(cadence_hz'),'SDK preview snapshot has no invented cadence parameter');
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
const demand={previewVisible:{viewer:true,calibration:true},pageActive:true,document:{hidden:false},viewerPaused:false,calibrationPaused:false,capabilities:{connection:{encoded_products_permitted:true,raw_products_permitted:false}},selectedPage:'overview',$:()=>({open:true})};
vm.createContext(demand);
vm.runInContext(source.slice(source.indexOf('function encodedPreviewPage('),source.indexOf('function startEncodedViewer(')),demand);
for(const page of ['overview','network']){demand.selectedPage=page;assert.equal(demand.encodedPreviewPage(),false,'non-preview pages have no demand');}
demand.selectedPage='viewer';assert.equal(demand.encodedPreviewPage(),true,'raw permission does not gate compressed viewing');
demand.viewerPaused=true;assert.equal(demand.encodedPreviewPage(),false);demand.viewerPaused=false;
demand.previewVisible.viewer=false;assert.equal(demand.encodedPreviewPage(),false,'offscreen Live rectangle releases demand');demand.previewVisible.viewer=true;
demand.document.hidden=true;assert.equal(demand.encodedPreviewPage(),false);demand.document.hidden=false;
demand.selectedPage='device';assert.equal(demand.encodedPreviewPage(),true);demand.calibrationPaused=true;assert.equal(demand.encodedPreviewPage(),false);demand.calibrationPaused=false;
demand.$=()=>({open:false});assert.equal(demand.encodedPreviewPage(),false,'collapsed calibration has no demand');
demand.selectedPage='viewer';demand.pageActive=false;assert.equal(demand.encodedPreviewPage(),false,'closed page cannot reconnect');
assert(!source.includes('/api/v1/frame/latest')&&!source.includes("'/api/v1/stream'"),'production browser has no raw subscription endpoint');
assert(!source.includes('nuvo-output-selection'),'old browser product preferences cannot reactivate raw demand');
const transportInventory={listener:{port:7443},paths:{
  ethernet:{state:'available',products_permitted:true,interfaces:[{name:'eth0',admin_up:true,carrier:true,addresses:[{address:'192.0.2.10',family:'ipv4',gateway_listening:true},{address:'2001:db8::10',family:'ipv6',gateway_listening:false}]}]},
  usb_device:{state:'unavailable',products_permitted:true,provisioned:false,bound:false,host_connected:false,interfaces:[]},
  wifi:{state:'available',products_permitted:false,interfaces:[{name:'wlan0',admin_up:true,carrier:true,addresses:[{address:'198.51.100.20',family:'ipv4',gateway_listening:true}]}]}}};
assert.deepEqual(D.deliveryPathModel(transportInventory,'ethernet').addresses,[{address:'192.0.2.10',interface:'eth0'}],'only addresses served by this gateway enter a recipe');
assert.equal(D.deliveryPathModel(transportInventory,'ethernet').ready,true);
const mixedInterfaces={paths:{ethernet:{state:'available',products_permitted:true,interfaces:[
  ...transportInventory.paths.ethernet.interfaces,
  {name:'eth1',admin_up:false,carrier:true,addresses:[{family:'ipv4',address:'192.0.2.21',gateway_listening:true}]},
  {name:'eth2',admin_up:true,carrier:false,addresses:[{family:'ipv4',address:'192.0.2.22',gateway_listening:true}]},
  {name:'eth3',admin_up:true,carrier:null,addresses:[{family:'ipv4',address:'192.0.2.23',gateway_listening:true}]}]}}};
assert.deepEqual(D.deliveryPathModel(mixedInterfaces,'ethernet').addresses,[{address:'192.0.2.10',interface:'eth0'}],'one ready interface cannot make down or unknown-carrier interfaces selectable');
for(const address of ['fe80::1','FEA0::2','febf::3','fe80::1%eth0']){
  const scoped={paths:{ethernet:{state:'available',products_permitted:true,interfaces:[{name:'eth0',admin_up:true,carrier:true,addresses:[{family:'ipv6',address,gateway_listening:true}]}]}}};
  assert.equal(D.deliveryPathModel(scoped,'ethernet').ready,false);assert.deepEqual(D.deliveryPathModel(scoped,'ethernet').addresses,[]);
  assert.match(D.deliveryPathModel(scoped,'ethernet').reason,/No usable address/,'link-local-only inventory must not promise an unscoped SDK route');
}

assert.match(D.deliveryPathModel(transportInventory,'usb_device').reason,/setup required/);
assert.equal(D.deliveryPathModel(transportInventory,'wifi').ready,false,'link readiness does not override device permission');
assert.match(D.deliveryPathModel(transportInventory,'wifi').reason,/blocked/);
assert.equal(D.deliveryPathModel({},'ethernet').discovered,false,'old gateways must not invent alternate endpoints');
for(const state of ['disconnected','no_address','unknown'])assert.equal(D.deliveryPathModel({paths:{ethernet:{...transportInventory.paths.ethernet,state}}},'ethernet').ready,false,'address alone does not prove an active link');
const endpoint={origin:'https://camera.local:7443',path:'ethernet',interface:'eth0',address:'192.0.2.10'};
const routedCode=D.outputApplicationRecipe(depth,endpoint);
assert.match(routedCode,/NuvoClient\("https:\/\/camera.local:7443", ca_file="device.crt", connect_address="192.0.2.10"\)/);
assert(routedCode.indexOf('client.claim(os.environ["NUVO_TOKEN"])')<routedCode.indexOf('client.capabilities()'),'authenticate before inspecting the connection');
for(const [key,value] of [['path','ethernet'],['interface','eth0'],['local_address','192.0.2.10']]){
  assert(routedCode.includes('connection.get('+JSON.stringify(key)+') != '+JSON.stringify(value)));
}
assert(routedCode.indexOf('client.capabilities()')<routedCode.indexOf('client.frames('),'verify actual path before creating product demand');
assert.match(routedCode,/"depth", "validity"/,'selecting a network path retains numeric depth');
assert(!routedCode.includes('verify=False')&&!routedCode.includes('CERT_NONE'));
const wifiDepthCode=D.outputApplicationRecipe(depth,{...endpoint,path:'wifi',interface:'wlan0',address:'198.51.100.20'});
assert(wifiDepthCode.includes('client.frames(')&&!wifiDepthCode.includes('latest_encoded()'),'Wi-Fi does not silently convert measurements to pictures');
assert.match(D.outputApplicationRecipe(D.outputRecipe('picture'),endpoint),/client.encoded_frames\(/);
const olderCode=D.outputApplicationRecipe(depth,{origin:endpoint.origin});
assert(!olderCode.includes('connect_address='));assert.match(olderCode,/Path discovery unavailable/);
assert.match(D.outputApplicationRecipe(depth,{...endpoint,origin:'http://127.0.0.1:1234'}),/os.environ\["NUVO_DEVICE_URL"\]/,'HTTP fixtures never fabricate an HTTPS certificate identity');
for(const wifi_enabled of [true,false])assert.deepEqual(D.wiredTransportPatch('usb',{wifi_enabled}),{preference:'usb',wifi_enabled},'wired changes preserve effective Wi-Fi permission, including legacy configuration');
assert.throws(()=>D.wiredTransportPatch('both',{}),/Read the current Wi-Fi/);
assert.throws(()=>D.wiredTransportPatch('off',{wifi_enabled:true}),/local device confirmation/);

console.log('Web output recipes, verified delivery endpoints, state and demand lifecycle tests passed');

const paired={...depth,preview:true,preview_cadence_hz:2};
const pairedCode=D.outputApplicationRecipe(paired,endpoint);
assert.match(pairedCode,/client.paired_frames\(products=\("depth", "validity"\), cadence_hz=2/);
assert.match(pairedCode,/preview_cadence_hz=2/);assert.match(pairedCode,/for frame, picture in pairs:/);assert.match(pairedCode,/may time out/);
transportInventory.paths.wifi.raw_products_permitted=false;transportInventory.paths.wifi.encoded_products_permitted=true;
assert.equal(D.deliveryPathModel(transportInventory,'wifi',D.outputRecipe('picture')).ready,true);
assert.equal(D.deliveryPathModel(transportInventory,'wifi',paired).ready,false);

const activityElements=new Map();
const activityContext={capabilities:{connection:{encoded_products_permitted:true}},viewerPaused:true,calibrationPaused:false,encodedFrameAt:0,encoding:{mode:'both'},lastHealth:{encoding_active:true,preview_demand_cadence_hz:2},setSampleCanvasLive(){},$:id=>{if(!activityElements.has(id))activityElements.set(id,{});return activityElements.get(id)}};
vm.runInNewContext(source.slice(source.indexOf('function updateViewerAvailability('),source.indexOf('function recordReceiving('))+';updateViewerAvailability();',activityContext);
assert.match(activityElements.get('sharedEncoderActivity').textContent,/active.*2 frames\/s.*other clients/,'native top-level activity remains active while own preview is paused');
activityContext.lastHealth={encoding_active:false,preview_demand_cadence_hz:0};vm.runInNewContext(source.slice(source.indexOf('function updateViewerAvailability('),source.indexOf('function recordReceiving('))+';updateViewerAvailability();',activityContext);
assert.match(activityElements.get('sharedEncoderActivity').textContent,/idle.*0 frames\/s/);

assert.throws(()=>D.outputApplicationRecipe({...paired,preview_cadence_hz:5},endpoint),/equal requested rates/);

assert.throws(()=>D.outputApplicationRecipe({...paired,delivery:'exact'},endpoint),/not Exact/);
