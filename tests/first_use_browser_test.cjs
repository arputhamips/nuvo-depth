/* Render the actual console against deterministic API fixtures; no Pi is touched. */
const assert=require('node:assert/strict'),http=require('node:http'),fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const {chromium}=require(process.env.NUVO_PLAYWRIGHT_MODULE||'playwright-core');
const root=path.resolve(__dirname,'..'),out=path.join(root,'output/first-use-browser');fs.mkdirSync(out,{recursive:true});
let sequence=0,invalid=false,pending=false,readinessOverride=null,holdFrames=false;
const requests={raw:[],encoded:0,configurationWrites:[]};
// Synthetic inventory for UI state coverage only; the real-gateway suite uses host discovery.
const fixtureAddress=address=>({family:address.includes(':')?'ipv6':'ipv4',address,gateway_listening:true,scope_required:false});
const fixtureInterface=(name,addresses)=>({name,admin_up:true,carrier:true,addresses:addresses.map(fixtureAddress)});
const defaultPaths=()=>({
  ethernet:{state:'available',interfaces:[fixtureInterface('eth0',['192.0.2.10','192.0.2.11'])],products_permitted:true,policy_kind:'ethernet'},
  usb_device:{state:'unavailable',interfaces:[],products_permitted:true,policy_kind:'usb_ethernet',provisioned:false,controller_present:false,bound:false,host_connected:false},
  wifi:{state:'available',interfaces:[fixtureInterface('wlan0',['198.51.100.20'])],products_permitted:true,policy_kind:'wifi'}
});
let fixturePaths=defaultPaths();
const defaultConnection=()=>({kind:'ethernet',path:'ethernet',interface:'eth0',local_address:'192.0.2.10',interface_scope:'camera',products_permitted:true,qualified_link:false,budget_is_measured_capacity:false,resource_budget_bytes_per_second:20000000});
let fixtureConnection=defaultConnection();
const encoding=JSON.parse(fs.readFileSync(path.join(root,'config/encoding.json')));encoding.mode='raw';
const jpeg=Buffer.from('/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAYEBQYFBAYGBQYHBwYIChAKCgkJChQODwwQFxQYGBcUFhYaHSUfGhsjHBYWICwgIyYnKSopGR8tMC0oMCUoKSj/2wBDAQcHBwoIChMKChMoGhYaKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCj/wAARCAAMABADASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwDzHSfCnT93+ldnpPhTp+7/AErttJ062+X5K7PSdOtuPkrzsdndTUy4V4iq+6f/2Q==','base64');
const encodingRevision=()=>crypto.createHash('sha256').update(JSON.stringify(encoding)).digest('hex');
const metadataBase={schema_version:1,device_uuid:'22'.repeat(16),run_uuid:'11'.repeat(16),capture_epoch:1,configuration_epoch:1,calibration_sha256:'ab'.repeat(32),calibration_authority:0};
function packet(names){
  const width=728,height=544,pieces=[],products=[];let offset=0;sequence++;
  for(const kind of names){const numeric=kind==='depth'||kind==='disparity',length=width*height*(numeric?4:1),data=Buffer.alloc(length);
    for(let i=0;i<width*height;i++){const x=i%width,y=Math.floor(i/width),valid=x>30&&x<width-30&&y>20&&y<height-20&&!invalid;
      if(numeric)data.writeFloatLE(valid?1+4*x/width:NaN,i*4);else data[i]=kind==='validity'?(valid?1:0):Math.round(40+170*((x+y)%120)/120);}
    pieces.push(data);products.push({kind,format:numeric?'float32_le':kind==='validity'?'uint8':'gray8',width,height,offset,length});offset+=length;}
  const metadata=Buffer.from(JSON.stringify({...metadataBase,capture_sequence:sequence,capture_time_ns:sequence*1000000000,products}));
  const header=Buffer.alloc(32);header.write('NUV1');header.writeUInt16BE(1,4);header.writeUInt16BE(5,8);header.writeUInt32BE(metadata.length,12);header.writeBigUInt64BE(BigInt(offset),16);header.writeBigUInt64BE(BigInt(sequence),24);
  return Buffer.concat([header,metadata,...pieces]);
}
function picturePacket(){
  sequence++;
  const metadata=Buffer.from(JSON.stringify({...metadataBase,capture_sequence:sequence,capture_time_ns:sequence*1000000000,
    source:encoding.source,codec:'jpeg',codec_string:'jpeg',mime_type:'image/jpeg',width:16,height:12,keyframe:true,
    encode_ms:.1,encoding_applied_revision:encodingRevision(),encoding_configuration_generation:1}));
  const header=Buffer.alloc(32);header.write('NVE1');header.writeUInt16BE(1,4);header.writeUInt16BE(1,6);header.writeUInt32BE(metadata.length,8);
  header.writeBigUInt64BE(BigInt(jpeg.length),12);header.writeBigUInt64BE(BigInt(sequence),20);
  return Buffer.concat([header,metadata,jpeg]);
}
const server=http.createServer((request,response)=>{
  const url=new URL(request.url,'http://localhost');const json=(value,status=200,revision='a'.repeat(64))=>{response.writeHead(status,{'content-type':'application/json',ETag:`"${revision}"`});response.end(JSON.stringify(value));};
  if(url.pathname==='/api/v1/session')return json({csrf:'f'.repeat(64)});
  if(url.pathname==='/api/v1/setup/status')return json({result:{schema_version:1,display_name:'First-use test fixture',phase:'installed',calibration_state:'pending',engineering:!pending,calibration_readiness:readinessOverride||{state:pending?'calibration_needed':'preview_enabled',label:pending?'Calibration needed':'Preview enabled',message:pending?'Capture is waiting for calibration or explicit preview.':'Distance accuracy remains unverified.',next_action:pending?'Import calibration or deliberately enable preview.':'Continue evaluation or import accepted calibration.',accepted:false}}});
  if(url.pathname==='/api/v1/health')return pending?json({error:'capture waiting for calibration'},503):json({...metadataBase,status:'running',capture_sequence:sequence,image_width:728,image_height:544,depth_width:728,depth_height:544,capture_target_hz:10,processing_ms:42,depth_published:sequence,exposure_mode:'manual',stereo_backend:'cpu_sgbm'});
  if(url.pathname==='/api/v1/capabilities')return json({products:['left_rectified','right_rectified','depth','disparity','validity','left_mono'],connection:fixtureConnection});
  if(url.pathname==='/api/v1/encoding'){
    if(request.method==='POST'){
      requests.configurationWrites.push(url.pathname);
      if(request.headers['if-match']!==`"${encodingRevision()}"`)return json({error:'Configuration changed; reload before saving.'},412);
      let body='';request.on('data',chunk=>{body+=chunk;if(body.length>65536)request.destroy()});
      request.on('end',()=>{try{Object.assign(encoding,JSON.parse(body));json(encoding,202,encodingRevision())}catch(_){json({error:'Invalid fixture configuration'},400)}});return;
    }
    response.writeHead(200,{'content-type':'application/json',ETag:`"${encodingRevision()}"`});return response.end(JSON.stringify(encoding));
  }
  if(url.pathname==='/api/v1/stereo'||url.pathname==='/api/v1/exposure')return json(JSON.parse(fs.readFileSync(path.join(root,'config',url.pathname.endsWith('stereo')?'stereo.json':'exposure.json'))));
  if(url.pathname==='/api/v1/transport')return json({fixture:true,preference:'auto',wifi_enabled:true,wifi_enabled_explicit:true,transaction:{status:'no_pending_change'},connection:fixtureConnection,listener:{address:'0.0.0.0',port:server.address().port,family:'ipv4'},paths:fixturePaths});
  if(url.pathname.startsWith('/api/v1/transport/')&&request.method==='POST')requests.configurationWrites.push(url.pathname);
  if(url.pathname==='/api/v1/frame/latest'){
    requests.raw.push({products:url.searchParams.get('products'),cadence:Number(url.searchParams.get('cadence_hz'))});
    if(encoding.mode==='encoded')return json({error:'Uncompressed network data is off'},403);
    if(pending||holdFrames)return json({error:'capture waiting for calibration'},503);
    response.writeHead(200,{'content-type':'application/octet-stream'});return response.end(packet((url.searchParams.get('products')||'left_mono').split(',')));
  }
  if(url.pathname==='/api/v1/encoded/latest'){
    requests.encoded++;
    if(encoding.mode==='raw'||pending||holdFrames)return json({error:'Picture preview unavailable'},503);
    response.writeHead(200,{'content-type':'application/octet-stream'});return response.end(picturePacket());
  }
  if(url.pathname.startsWith('/api/'))return json({},404);
  const name=url.pathname==='/'?'index.html':url.pathname.slice(1),file=path.join(root,'web',name);
  if(!file.startsWith(path.join(root,'web')+path.sep)||!fs.existsSync(file)){response.writeHead(404);return response.end();}
  response.setHeader('content-type',name.endsWith('.js')?'text/javascript':name.endsWith('.css')?'text/css':name.endsWith('.json')?'application/json':name.endsWith('.ttf')?'font/ttf':'text/html');response.end(fs.readFileSync(file));
});
server.on('upgrade',(request,socket)=>{socket.on('error',()=>{});socket.end('HTTP/1.1 426 Upgrade Required\r\nConnection: close\r\n\r\n');});
(async()=>{
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
  if(process.argv.includes('--serve-only')){console.log(JSON.stringify({fixture:true,origin:`http://127.0.0.1:${server.address().port}/`,mode:'synthetic polling only'}));return;}
  const executable=process.env.BROWSER_EXECUTABLE||(process.platform==='win32'?'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe':undefined);
  const browser=await chromium.launch({headless:true,...(executable?{executablePath:executable}:{})});
  let context,page;const errors=[],checks=[];
  try{
    context=await browser.newContext({acceptDownloads:true,viewport:{width:1440,height:1050}});
    await context.tracing.start({screenshots:true,snapshots:true,sources:true});page=await context.newPage();
    page.on('pageerror',error=>errors.push(error.stack||error.message));const url=`http://127.0.0.1:${server.address().port}/`;
    const overviewProductAttempts=[];
    const recordOverviewProduct=(kind,address)=>{
      if(['/api/v1/frame/latest','/api/v1/encoded/latest','/api/v1/stream','/api/v1/encoded/stream'].includes(new URL(address).pathname))overviewProductAttempts.push({kind,url:address});
    };
    const overviewRequest=request=>recordOverviewProduct('HTTP',request.url()),overviewSocket=socket=>recordOverviewProduct('WebSocket',socket.url());
    page.on('request',overviewRequest);page.on('websocket',overviewSocket);
    await page.goto(url);
    for(const id of ['exposureForm','algorithmForm','encodingForm'])assert.equal(await page.locator(`#${id} button[type="submit"]`).count(),1,`${id} must retain its submit control after HTML parser repair`);
    await page.waitForFunction(()=>document.querySelector('#connection').textContent==='Connected');
    assert.deepEqual(await page.locator('nav [data-page]').evaluateAll(items=>items.map(item=>item.dataset.page)),['overview','viewer','network','device']);
    assert.equal(await page.locator('.page.active').getAttribute('id'),'overview','hashless URLs land on Overview');
    for(const id of ['cameraState','overviewCaptureState','overviewDepthState','performanceMonitor'])assert(await page.isVisible('#'+id),`${id} belongs on Overview`);
    assert(await page.isVisible('#overview .setup-guide > summary'));
    assert(await page.evaluate(()=>{const nav=document.querySelector('nav').getBoundingClientRect(),main=document.querySelector('.page.active').getBoundingClientRect(),buttons=[...document.querySelectorAll('nav [data-page]')].map(x=>x.getBoundingClientRect());return nav.height>nav.width&&main.left>=nav.right-1&&buttons.every((b,i)=>!i||b.top>=buttons[i-1].bottom-1)}),'desktop navigation must be a sidebar beside the content');
    assert(await page.locator('nav [data-page]').evaluateAll(items=>items.every(item=>getComputedStyle(item).justifyContent==='flex-start'&&Math.abs(item.querySelector('svg').getBoundingClientRect().left-items[0].querySelector('svg').getBoundingClientRect().left)<1)),'desktop sidebar icons and labels stay left aligned');
    await page.goto(url+'#unknown-section');assert.equal(await page.locator('.page.active').getAttribute('id'),'overview');
    await page.goto(url+'#overview');
    await page.waitForResponse(response=>response.url().endsWith('/api/v1/health')&&response.ok());
    assert.deepEqual(overviewProductAttempts,[],'Overview must not request raw or encoded products');
    assert.equal(requests.raw.length,0);assert.equal(requests.encoded,0);
    await page.screenshot({path:path.join(out,'overview-desktop.png')});
    page.off('request',overviewRequest);page.off('websocket',overviewSocket);
    const overviewHeadingTop=await page.locator('#overview h1').evaluate(heading=>heading.getBoundingClientRect().top);
    for(const destination of ['viewer','network','device','overview']){
      await page.click(`nav [data-page="${destination}"]`);
      await page.waitForFunction(()=>window.scrollY<=1);
      const position=await page.locator(`#${destination} h1`).evaluate(heading=>({top:heading.getBoundingClientRect().top,headerBottom:document.querySelector('header').getBoundingClientRect().bottom}));
      assert(Math.abs(position.top-overviewHeadingTop)<=2,`${destination} heading must stay top-aligned with Overview, including short collapsed Device content`);
      assert(position.top>=position.headerBottom&&position.top<=position.headerBottom+64,`${destination} heading must remain near the top below the header`);
    }
    checks.push('all four desktop headings stay top aligned');
    checks.push('Overview default and unknown-link landing','Overview status and setup visible','desktop sidebar geometry','Overview has no product demand');
    await page.click('nav [data-page="viewer"]');
    await page.waitForFunction(()=>document.querySelector('#viewerReceiving').textContent.includes('Camera image'));
    assert.equal(requests.encoded,0,'a numeric/image recipe must not also request the shared picture');
    await page.locator('#firstDepthGuide > summary').focus();await page.keyboard.press('Enter');
    assert(await page.isDisabled('#firstDepthSave'),'mono alone cannot complete depth');
    await page.click('#firstDepthStart');await page.waitForFunction(()=>!document.querySelector('#firstDepthSave').disabled);
    assert.equal(await page.inputValue('#liveRecipe'),'depth_reference');
    assert.match(await page.textContent('#firstDepthAuthority'),/accuracy unverified/);
    await page.fill('#pixelX','50');await page.fill('#pixelY','40');await page.locator('#pixelInspector button[type="submit"]').click();
    assert.match(await page.textContent('#pixelValue'),/Depth Z: .* m/);
    const downloadPromise=page.waitForEvent('download');await page.click('#firstDepthSave');const download=await downloadPromise;await download.saveAs(path.join(out,'sample.nuv'));
    const saved=fs.readFileSync(path.join(out,'sample.nuv')),meta=JSON.parse(saved.subarray(32,32+saved.readUInt32BE(12)));
    assert.equal(meta.calibration_authority,0);assert.equal(meta.calibration_sha256,metadataBase.calibration_sha256);assert(meta.products.some(p=>p.kind==='depth')&&meta.products.some(p=>p.kind==='validity'));
    checks.push('four destinations','keyboard first-depth disclosure','named depth with camera reference','saved numeric depth + validity and calibration identity');
    await page.screenshot({path:path.join(out,'first-use-console.png'),fullPage:false});
    invalid=true;await page.click('#firstDepthStart');await page.waitForFunction(()=>document.querySelector('#firstDepthError').textContent.includes('No valid depth'));assert(await page.isDisabled('#firstDepthSave'));
    invalid=false;await page.click('#firstDepthStart');await page.waitForFunction(()=>!document.querySelector('#firstDepthSave').disabled);
    await context.setOffline(true);await page.waitForFunction(()=>document.querySelector('#firstDepthSave').disabled);
    assert.doesNotMatch(await page.textContent('#viewerReceiving'),/^Receiving:/,'disconnected viewer must not claim active receipt');
    await context.setOffline(false);
    await page.waitForFunction(()=>!document.querySelector('#firstDepthSave').disabled);
    await page.click('#viewerPause');assert.match(await page.textContent('#viewerReceiving'),/Paused/);
    await page.click('#viewerPause');await page.waitForFunction(()=>document.querySelector('#viewerReceiving').textContent.includes('Depth with camera reference'));
    checks.push('all-invalid depth rejected','disconnect invalidates sample','reconnect receives fresh depth');

    await page.click('nav [data-page="network"]');
    assert.equal(await page.inputValue('#outputPreset'),'depth_reference','onboarding must remain a recognizable recipe');
    await page.check('[name="outputTask"][value="depth"]');await page.uncheck('#outputCameraReference');await page.click('#outputApply');
    await page.waitForFunction(()=>document.querySelector('#outputReceiving').textContent.includes('Depth values'));
    const beforeEdit=requests.raw.length;
    await page.check('[name="outputTask"][value="camera"]');
    assert.match(await page.textContent('#outputPending'),/Editing.*not requested/);
    const requestedBeforeNavigation=await page.evaluate(()=>JSON.stringify(outputSettings));
    await page.click('nav [data-page="overview"]');
    await page.click('nav [data-page="device"]');
    await page.click('nav [data-page="network"]');
    assert(await page.isChecked('[name="outputTask"][value="camera"]'),'navigation preserves the unapplied output draft');
    assert.match(await page.textContent('#outputPending'),/Editing.*not requested/);
    assert.equal(await page.evaluate(()=>JSON.stringify(outputSettings)),requestedBeforeNavigation,'navigation cannot apply a draft');
    checks.push('output draft survives Overview and Device navigation');
    await page.waitForFunction(()=>document.querySelector('#outputReceiving').textContent.includes('Depth values'));
    assert.match(await page.textContent('#outputRequested'),/Depth values/);
    assert.match(await page.textContent('#outputReceiving'),/Depth values/);
    await page.waitForFunction(()=>document.querySelector('#outputReceiving').textContent.includes('Depth values'));
    assert(requests.raw.slice(beforeEdit).every(value=>value.products==='depth,validity'),'editing must not change the active subscription');
    await page.click('#outputDiscard');assert(await page.isChecked('[name="outputTask"][value="depth"]'));
    holdFrames=true;
    await page.check('[name="outputTask"][value="camera"]');await page.click('#outputApply');
    assert.match(await page.textContent('#outputSaveState'),/Requested.*waiting/);
    assert.match(await page.textContent('#outputReceiving'),/waiting/,'a successful local change is not confirmed receipt');
    holdFrames=false;await page.waitForFunction(()=>document.querySelector('#outputReceiving').textContent.includes('Camera image'));
    checks.push('editing preserves requested and receiving selection','discard restores selection','requested waits for matching receipt');
    for(const [name,rate] of [['stereo','1'],['camera','5'],['depth','2']]){
      await page.check(`[name="outputTask"][value="${name}"]`);assert.equal(await page.inputValue('#outputCadence'),rate);
    }
    await page.locator('#outputAdvanced > summary').click();
    await page.click('[name="outputProduct"][value="validity"]');
    assert(await page.isChecked('[name="outputProduct"][value="validity"]'),'numeric depth always includes its valid-pixel mask');
    await page.selectOption('#outputDestination','app');
    await page.check('[name="outputPath"][value="ethernet"]');await page.selectOption('#outputPathAddress','192.0.2.10');
    const activeSelection=await page.evaluate(()=>JSON.stringify(outputSettings)),writes=requests.configurationWrites.length;
    const rawStart=requests.raw.length,encodedStart=requests.encoded;
    await page.click('#outputApply');assert(await page.isVisible('#outputRecipe'));
    assert.match(await page.textContent('#outputRecipe'),/client\.frames/);assert.match(await page.textContent('#outputRecipe'),/validity/);
    await page.evaluate(()=>refreshCapabilities());
    assert(await page.isVisible('#outputRecipe'),'unchanged background refresh must not hide generated code');
    assert.equal(await page.evaluate(()=>JSON.stringify(outputSettings)),activeSelection);
    assert.equal(requests.configurationWrites.length,writes);assert.equal(requests.encoded,encodedStart);
    assert(requests.raw.slice(rawStart).every(value=>value.products==='left_mono'),'connection code must not start its selected products');
    await page.fill('#outputCadence','3');assert(await page.isHidden('#outputRecipe'),'edited inputs invalidate old connection code');
    assert.match(await page.textContent('#outputRecipe'),/connect_address=.*192\.0\.2\.10/);
    checks.push('deterministic recipe rates','depth automatically includes validity','recipe does not change subscriptions or device settings','recipe survives refresh and invalidates on edit');

    await page.check('[name="outputPath"][value="usb_device"]');
    assert(await page.isDisabled('#outputApply'),'unprovisioned USB cannot offer a connection recipe');
    assert.equal(await page.locator('#outputPathAddress option[value]:not([value=""])').count(),0);
    assert.match(await page.textContent('#outputPathStatus'),/USB|setup|unavailable|controller/i);
    assert.match(await page.textContent('#outputConnection'),/eth0/,'choosing USB does not change the observed browser path');
    await page.check('[name="outputPath"][value="wifi"]');
    assert(await page.isChecked('[name="outputTask"][value="depth"]'),'Wi-Fi selection preserves numeric output');
    assert(await page.isChecked('[name="outputProduct"][value="validity"]'));
    assert(await page.isVisible('#outputUsePicture'));
    await page.click('#outputApply');
    assert.match(await page.textContent('#outputRecipe'),/connect_address=.*198\.51\.100\.20/);
    assert.match(await page.textContent('#outputRecipe'),/client\.frames/);
    await page.click('#outputUsePicture');
    assert.equal(await page.inputValue('#outputFormat'),'picture');
    assert(await page.isDisabled('#outputApply'),'picture code must explain that the shared encoder is off');
    assert.match(await page.textContent('#outputPathStatus'),/encoder|preview|picture/i);
    assert.equal(requests.configurationWrites.length,writes,'choosing a path or picture never changes shared configuration');
    assert.equal(requests.encoded,encodedStart,'app drafts never subscribe the browser to pictures');
    encoding.mode='both';encoding.codec='jpeg';await page.evaluate(()=>refreshEncoding());
    await page.click('#outputApply');
    assert.match(await page.textContent('#outputRecipe'),/latest_encoded|encoded_frames/);
    await page.check('[name="outputPath"][value="ethernet"]');
    await page.selectOption('#outputPathAddress','192.0.2.11');
    assert.equal(await page.inputValue('#outputFormat'),'picture','changing path preserves the explicit representation');
    await page.evaluate(()=>refreshTransport());
    await page.click('nav [data-page="overview"]');await page.click('nav [data-page="network"]');
    assert.equal(await page.inputValue('#outputPathAddress'),'192.0.2.11');
    assert.equal(await page.inputValue('#outputFormat'),'picture');
    assert.equal(await page.inputValue('#outputDestination'),'app');
    fixturePaths.ethernet.interfaces[0].addresses=fixturePaths.ethernet.interfaces[0].addresses.filter(item=>item.address!=='192.0.2.11');
    await page.evaluate(()=>refreshTransport());
    assert(await page.isDisabled('#outputApply'),'removed IP cannot remain a usable connection target');
    assert(await page.locator('#outputPathAddress').evaluate(select=>select.selectedOptions[0]?.disabled),'removed address must be marked unavailable without silently selecting another target');
    assert(await page.isHidden('#outputRecipe'),'inventory changes invalidate generated connection code');
    fixturePaths.usb_device={state:'available',interfaces:[fixtureInterface('usb0',['203.0.113.30'])],products_permitted:true,policy_kind:'usb_ethernet',provisioned:true,controller_present:true,bound:true,host_connected:true};
    await page.evaluate(()=>refreshTransport());await page.check('[name="outputPath"][value="usb_device"]');
    await page.selectOption('#outputPathAddress','203.0.113.30');await page.click('#outputApply');
    assert.match(await page.textContent('#outputRecipe'),/connect_address=.*203\.0\.113\.30/);
    assert.match(await page.textContent('#outputRecipe'),/usb_device/);
    fixturePaths=defaultPaths();fixtureConnection={...defaultConnection(),kind:'usb_ethernet',path:'usb_host_adapter',interface:'enxfixture',local_address:'192.0.2.99'};
    await page.evaluate(async()=>{await refreshTransport();await refreshCapabilities();});
    assert.match(await page.textContent('#outputConnection'),/adapter.*not USB-C device/i);
    assert(await page.isDisabled('#outputApply'),'USB host adapter cannot make USB-C device delivery available');
    fixtureConnection=defaultConnection();fixturePaths=defaultPaths();
    fixturePaths.ethernet.interfaces[0].addresses.forEach(address=>address.gateway_listening=false);
    await page.evaluate(async()=>{await refreshTransport();await refreshCapabilities();});
    await page.check('[name="outputPath"][value="ethernet"]');
    assert(await page.isDisabled('#outputApply'),'an interface address without a listening gateway is not a usable path');
    fixturePaths=defaultPaths();
    fixturePaths.ethernet.interfaces.push(
      {...fixtureInterface('eth1',['192.0.2.21']),admin_up:false},
      {...fixtureInterface('eth2',['192.0.2.22']),carrier:false},
      {...fixtureInterface('eth3',['192.0.2.23']),carrier:null},
      fixtureInterface('eth4',['fe80::20','fe90::21','fe80::22%eth4'])
    );
    fixturePaths.ethernet.interfaces[0].addresses.push({...fixtureAddress('2001:db8::20'),scope_required:true});
    await page.evaluate(()=>refreshTransport());
    assert.deepEqual(await page.locator('#outputPathAddress option:not(:disabled)').evaluateAll(options=>options.map(option=>option.value).filter(Boolean)),['192.0.2.10','192.0.2.11'],'each offered address needs its own active interface and must not need a client scope');
    await page.selectOption('#outputPathAddress','192.0.2.10');
    fixturePaths.ethernet.interfaces[0].addresses=[fixtureAddress('febf::25')];
    await page.evaluate(()=>refreshTransport());
    assert(await page.isDisabled('#outputApply'),'a ready path with only scoped or inactive addresses cannot generate code');
    assert.equal(await page.locator('#outputPathAddress option:not(:disabled)').evaluateAll(options=>options.filter(option=>option.value).length),0);
    assert.match(await page.textContent('#outputPathStatus'),/No usable address/);
    checks.push('each application address requires its own active link and no client scope');
    fixturePaths=undefined;await page.evaluate(()=>refreshTransport());
    await page.click('#outputApply');
    assert.match(await page.textContent('#outputRecipe'),/Path discovery unavailable/);
    assert.doesNotMatch(await page.textContent('#outputRecipe'),/connect_address=/,'older gateways must not invent a direct path address');
    checks.push('older gateway fallback explicitly lacks physical path selection');
    assert.equal(requests.configurationWrites.length,writes);
    assert.equal(await page.evaluate(()=>JSON.stringify(outputSettings)),activeSelection,'all path recipes leave this browser subscription unchanged');
    fixturePaths=defaultPaths();encoding.mode='raw';await page.evaluate(async()=>{await refreshTransport();await refreshEncoding();});
    checks.push('explicit Ethernet USB and Wi-Fi recipe targets','unavailable USB cannot masquerade as connected','Wi-Fi picture selection is explicit and requires an encoder','path and representation drafts survive navigation','removed addresses invalidate recipes','USB host adapter is not USB-C device mode','only listening addresses can generate path recipes');

    // A hash change on the old document still uses its last-known raw configuration.
    // Isolate cold startup and count all raw attempts made by the new document.
    await page.evaluate(()=>localStorage.removeItem('nuvo-output-selection'));
    await page.close();
    encoding.mode='encoded';encoding.codec='jpeg';encoding.width=0;encoding.height=0;
    page=await context.newPage();page.on('pageerror',error=>errors.push(error.stack||error.message));
    const pictureRawAttempts=[];
    const recordRawAttempt=(kind,address)=>{
      const route=new URL(address).pathname;
      if(route==='/api/v1/frame/latest'||route==='/api/v1/stream')pictureRawAttempts.push({kind,url:address});
    };
    const onPictureRequest=request=>recordRawAttempt('HTTP',request.url());
    const onPictureSocket=socket=>recordRawAttempt('WebSocket',socket.url());
    page.on('request',onPictureRequest);page.on('websocket',onPictureSocket);
    const pictureRequests=requests.encoded;
    await page.goto(url+'#viewer');
    await page.waitForFunction(()=>document.querySelector('#viewerReceiving').textContent.includes('JPEG'));
    assert(await page.isVisible('#viewerPicture'));assert(await page.isHidden('#pixelInspector'));
    assert(await page.isHidden('#viewerModeNotice'),'receiving the selected picture must not still ask the user to choose picture preview');
    assert.match(await page.textContent('#pictureMeasurementNotice'),/Numeric value unavailable/);
    await page.locator('#pixelInspector').evaluate(form=>form.requestSubmit());
    assert.doesNotMatch(await page.textContent('#pixelValue'),/Depth Z:/,'a hidden inspector must not reuse a previous numeric frame for a compressed picture');
    assert.equal(await page.locator('#encodedCanvas').evaluate(canvas=>[canvas.width,canvas.height].join('x')),'16x12');
    await page.evaluate(()=>refreshCapabilities());
    await page.waitForFunction(()=>document.querySelector('#viewerReceiving').textContent.includes('JPEG'));
    await page.waitForResponse(response=>response.url().includes('/api/v1/encoded/latest')&&response.ok());
    assert(requests.encoded>pictureRequests,'compressed-only startup must receive picture data');
    assert.deepEqual(pictureRawAttempts,[],'compressed-only Live view must not attempt raw HTTP or WebSocket requests');
    page.off('request',onPictureRequest);page.off('websocket',onPictureSocket);
    assert(await page.isDisabled('#firstDepthSave'));
    await page.screenshot({path:path.join(out,'compressed-only-live.png')});
    checks.push('compressed-only Live view decodes a real JPEG','picture cannot claim numeric measurement','compressed path does not also subscribe to raw products');
    encoding.mode='raw';await page.evaluate(()=>localStorage.removeItem('nuvo-output-selection'));await page.reload();
    await page.waitForFunction(()=>document.querySelector('#viewerReceiving').textContent.includes('Camera image'));

    for(const [legacy,destination] of Object.entries({exposure:'viewer',presets:'viewer',algorithm:'viewer',streaming:'device',calibration:'device',diagnostics:'device',recovery:'device'})){
      await page.goto(url+'#'+legacy);
      assert.equal(await page.locator('.page.active').getAttribute('id'),destination);
      assert(await page.locator('#'+legacy).evaluate(section=>section.open),`legacy #${legacy} opens its disclosed section`);
      assert.equal(await page.locator('nav [aria-current="page"]').getAttribute('data-page'),destination);
      await page.waitForFunction(id=>(()=>{const section=document.querySelector('#'+id+' > summary').getBoundingClientRect(),nav=document.querySelector('nav').getBoundingClientRect(),header=document.querySelector('header').getBoundingClientRect();return section.top>=(section.left<nav.right?nav.bottom:header.bottom)-1})(),legacy);
    }
    await page.click('nav [data-page="viewer"]');await page.locator('nav [data-page="viewer"]').focus();
    await page.keyboard.press('Tab');assert.equal(await page.evaluate(()=>document.activeElement.dataset.page),'network');
    await page.keyboard.press('Enter');assert.equal(await page.locator('.page.active').getAttribute('id'),'network');
    await page.locator('.skip-link').focus();await page.keyboard.press('Enter');assert.equal(await page.evaluate(()=>document.activeElement.id),'consoleMain');
    checks.push('all legacy deep links open the correct disclosure','keyboard navigation and skip link');

    for(const width of [320,390])for(const theme of ['light','dark']){
      await page.setViewportSize({width,height:844});await page.evaluate(theme=>applyTheme(theme,false),theme);
      for(const destination of ['overview','viewer','network','device']){
        await page.click(`nav [data-page="${destination}"]`);
        await page.waitForFunction(()=>window.scrollY<=1);
        assert(await page.locator('nav [data-page]').evaluateAll(items=>items.every(item=>{const r=item.getBoundingClientRect();return r.height>=44&&r.width>=44&&r.left>=0&&r.right<=innerWidth+1})),`all four mobile destinations must remain reachable at ${width}px`);
        if(destination==='network')await page.selectOption('#outputDestination','app');
        assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth+2),`${destination} must fit ${width}px in ${theme}`);
        await page.screenshot({path:path.join(out,`${destination}-${width}-${theme}.png`),fullPage:false});
        if(destination==='network')await page.locator('#outputApplicationDelivery').screenshot({path:path.join(out,`delivery-paths-${width}-${theme}.png`)});
      }
    }
    for(const width of [320,390]){
      await page.setViewportSize({width,height:844});
      for(const legacy of ['exposure','streaming','calibration']){
        await page.goto(url+'#'+legacy);
        await page.waitForFunction(id=>(()=>{const section=document.querySelector('#'+id+' > summary').getBoundingClientRect(),nav=document.querySelector('nav').getBoundingClientRect(),header=document.querySelector('header').getBoundingClientRect();return section.top>=(section.left<nav.right?nav.bottom:header.bottom)-1})(),legacy);
      }
    }
    checks.push('320px and 390px four-page layouts in both themes','legacy headings remain below sticky navigation at narrow widths');

    pending=true;await page.goto(url+'#overview');await page.reload();await page.waitForFunction(()=>document.querySelector('#setupState').textContent.includes('Calibration needed'));assert(await page.isVisible('#setupState'));assert(await page.isDisabled('#firstDepthSave'));
    readinessOverride={state:'calibration_accepted',label:'Calibration accepted',message:'Matching per-unit declared validation; camera disconnected.',next_action:'Inspect camera/service health and verify fresh output.',accepted:true};
    await page.reload();await page.waitForFunction(()=>document.querySelector('#calibrationState').textContent==='Calibration accepted');assert(await page.isDisabled('#firstDepthSave'));
    assert.equal(await page.textContent('#connection'),'Connected');
    readinessOverride={state:'unavailable',label:'Status unavailable/error',message:'Calibration belongs to another device.',next_action:'Import the original compatible bundle.',accepted:false};
    await page.reload();await page.waitForFunction(()=>document.querySelector('#calibrationState').textContent==='Status unavailable/error');
    await page.goto(url+'#calibration');assert.match(await page.textContent('#calibrationReadinessMessage'),/another device/);assert.match(await page.textContent('#calibrationReadinessAction'),/original compatible bundle/);
    const advanced=page.locator('details.calibration-board').filter({hasText:'Advanced geometry and qualification parameters'});assert.equal(await advanced.getAttribute('open'),null);
    checks.push('pending calibration','accepted calibration with unavailable camera','specific readiness recovery','advanced calibration collapsed');
    assert.deepEqual(errors,[]);fs.writeFileSync(path.join(out,'results.json'),JSON.stringify({fixture:true,network_policy:'synthetic path inventory for UI only; real gateway/Pi suites cover discovery and admission',checks,pageErrors:errors},null,2));
    console.log(`Browser unified-console and first-use journeys: ${checks.length} fixture checks passed; no page errors.`);
  }catch(error){
    await page?.screenshot({path:path.join(out,'failure.png'),fullPage:true}).catch(()=>{});
    fs.writeFileSync(path.join(out,'failure.json'),JSON.stringify({error:String(error).slice(0,4000),pageErrors:errors},null,2));throw error;
  }finally{
    await context?.tracing.stop({path:path.join(out,'trace.zip')}).catch(()=>{});
    await browser.close();await new Promise(resolve=>server.close(resolve));
  }
})().catch(error=>{console.error(error);server.close();process.exitCode=1;});
