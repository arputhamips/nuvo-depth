/* Render the actual console against deterministic API fixtures; no Pi is touched. */
const assert=require('node:assert/strict'),http=require('node:http'),fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const {chromium}=require(process.env.NUVO_PLAYWRIGHT_MODULE||'playwright-core');
const root=path.resolve(__dirname,'..'),out=path.join(root,'output/first-use-browser');fs.mkdirSync(out,{recursive:true});
let sequence=0,invalid=false,pending=false,readinessOverride=null,holdFrames=false,externalDemand=false,pairedSupported=true,encodingPostDelay=0;
const requests={raw:[],encoded:0,configurationWrites:[]};
// Synthetic inventory for UI state coverage only; the real-gateway suite uses host discovery.
const fixtureAddress=address=>({family:address.includes(':')?'ipv6':'ipv4',address,gateway_listening:true,scope_required:false});
const fixtureInterface=(name,addresses)=>({name,admin_up:true,carrier:true,addresses:addresses.map(fixtureAddress)});
const defaultPaths=()=>({
  ethernet:{state:'available',interfaces:[fixtureInterface('eth0',['192.0.2.10','192.0.2.11'])],products_permitted:true,raw_products_permitted:true,encoded_products_permitted:true,policy_kind:'ethernet'},
  usb_device:{state:'unavailable',interfaces:[],products_permitted:true,raw_products_permitted:true,encoded_products_permitted:true,policy_kind:'usb_ethernet',provisioned:false,controller_present:false,bound:false,host_connected:false},
  wifi:{state:'available',interfaces:[fixtureInterface('wlan0',['198.51.100.20'])],products_permitted:true,raw_products_permitted:true,encoded_products_permitted:true,policy_kind:'wifi'}
});
let fixturePaths=defaultPaths();
const defaultConnection=()=>({kind:'ethernet',path:'ethernet',interface:'eth0',local_address:'192.0.2.10',interface_scope:'camera',products_permitted:true,raw_products_permitted:true,encoded_products_permitted:true,qualified_link:false,budget_is_measured_capacity:false,resource_budget_bytes_per_second:20000000});
let fixtureConnection=defaultConnection();
const encoding=JSON.parse(fs.readFileSync(path.join(root,'config/encoding.json')));encoding.mode='both';encoding.codec='jpeg';
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
  if(url.pathname==='/api/v1/health')return pending?json({error:'capture waiting for calibration'},503):json({...metadataBase,status:'running',capture_sequence:sequence,image_width:728,image_height:544,depth_width:728,depth_height:544,capture_target_hz:10,processing_ms:42,depth_published:sequence,exposure_mode:'manual',stereo_backend:'cpu_sgbm',encoding_active:externalDemand,preview_demand_cadence_hz:externalDemand?2:0});
  if(url.pathname==='/api/v1/capabilities')return json({paired_stream:{supported:pairedSupported},products:['left_rectified','right_rectified','depth','disparity','validity','left_mono'],connection:fixtureConnection});
  if(url.pathname==='/api/v1/encoding'){
    if(request.method==='POST'){
      requests.configurationWrites.push(url.pathname);
      if(request.headers['if-match']!==`"${encodingRevision()}"`)return json({error:'Configuration changed; reload before saving.'},412);
      let body='';request.on('data',chunk=>{body+=chunk;if(body.length>65536)request.destroy()});
      request.on('end',()=>{const apply=()=>{try{Object.assign(encoding,JSON.parse(body));json(encoding,202,encodingRevision())}catch(_){json({error:'Invalid fixture configuration'},400)}};if(encodingPostDelay)setTimeout(apply,encodingPostDelay);else apply();});return;
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
    const rawAttempts=[];
    page.on('request',request=>{if(new URL(request.url()).pathname==='/api/v1/frame/latest')rawAttempts.push(request.url())});
    page.on('websocket',socket=>{if(new URL(socket.url()).pathname==='/api/v1/stream')rawAttempts.push(socket.url())});
    await page.evaluate(()=>localStorage.setItem('nuvo-output-selection',JSON.stringify({enabled:true,products:['depth','validity'],cadence_hz:30,delivery:'auto',preview:true,preview_cadence_hz:30})));
    await page.goto(url+'#viewer');await page.reload();
    await page.waitForFunction(()=>document.querySelector('#viewerReceiving').textContent.includes('JPEG picture'));
    assert.equal(await page.locator('#pixelInspector').count(),0);assert.equal(await page.locator('#liveRecipe').count(),0);
    assert.equal(await page.inputValue('#liveCadence'),'5');
    assert(await page.locator('#encodedCanvas').evaluate(canvas=>canvas.getBoundingClientRect().height>=220));
    assert.match(await page.textContent('#viewerConnection'),/Ethernet/);
    await page.screenshot({path:path.join(out,'preview-live.png')});
    checks.push('legacy saved numeric selection never creates raw browser demand','Live defaults to decoded JPEG','permanent large preview rectangle','actual camera connection beside preview');
    await page.click('#viewerPause');const pausedRequests=requests.encoded;
    await page.waitForTimeout(800);assert.equal(requests.encoded,pausedRequests,'local pause stops encoded fallback polling');
    assert(await page.isVisible('#encodedCanvas'));assert.match(await page.textContent('#encodedEmptyState'),/Paused/);
    externalDemand=true;await page.evaluate(()=>refreshHealth());assert.match(await page.textContent('#sharedEncoderActivity'),/active.*2 frames\/s.*other clients/);
    await page.click('#viewerPause');await page.waitForFunction(()=>document.querySelector('#viewerReceiving').textContent.includes('JPEG picture'));
    checks.push('local pause releases only browser demand','paused rectangle stays visible','top-level shared activity remains truthful for external clients','resume preview');
    await page.evaluate(()=>{Object.defineProperty(document,'hidden',{configurable:true,value:true});document.dispatchEvent(new Event('visibilitychange'));});const hiddenRequests=requests.encoded;await page.waitForTimeout(800);assert.equal(requests.encoded,hiddenRequests,'visibility handler releases encoded demand');
    await page.evaluate(()=>{delete document.hidden;document.dispatchEvent(new Event('visibilitychange'));});await page.waitForFunction(()=>document.querySelector('#viewerReceiving').textContent.includes('JPEG picture'));checks.push('hidden-tab visibility event stops demand and visible event resumes');
    for(const destination of ['overview','network']){
      await page.click(`nav [data-page="${destination}"]`);const before=requests.encoded;await page.waitForTimeout(800);assert.equal(requests.encoded,before,`${destination} must have no preview demand`);
    }
    const writes=requests.configurationWrites.length,encodedBeforeRecipe=requests.encoded;
    assert.equal(await page.locator('#outputDestination').count(),0,'raw output choices belong only to My application');
    assert.deepEqual(await page.locator('[name="outputPath"]').evaluateAll(items=>items.map(item=>item.parentElement.querySelector('span').childNodes[0].textContent)),['Ethernet','USB 2.0','WLAN']);
    assert.equal(await page.locator('#outputConnectionAdvanced').evaluate(element=>element.open),false);
    for(const [preset,rate] of [['camera','5'],['stereo','1'],['depth','2']]){await page.check(`[name="outputTask"][value="${preset}"]`);assert.equal(await page.inputValue('#outputCadence'),rate);}
    await page.locator('#outputAdvanced > summary').click();await page.click('[name="outputProduct"][value="validity"]');assert(await page.isChecked('[name="outputProduct"][value="validity"]'));
    await page.check('#outputPictureEnabled');assert.equal(await page.inputValue('#outputPictureCadence'),'2');
    await page.selectOption('#outputDelivery','exact');assert(await page.isDisabled('#outputApply'));assert.match(await page.textContent('#outputWarning'),/best-effort capture association/);await page.selectOption('#outputDelivery','best_effort');
    assert(await page.locator('#outputPictureCadence').evaluate(input=>input.readOnly));assert.match(await page.textContent('#outputPictureRateNote'),/same requested rate/);
    await page.click('#outputApply');assert.match(await page.textContent('#outputRecipe'),/client.paired_frames\(products=\("depth", "validity"\)/);
    assert.match(await page.textContent('#outputRecipe'),/client.claim\(os.environ\["NUVO_TOKEN"\]\)/);
    assert.match(await page.textContent('#outputRecipe'),/connect_address="192.0.2.10"/);
    assert.match(await page.textContent('#outputRecipe'),/connection.get\("local_address"\)/);
    await page.evaluate(()=>refreshCapabilities());assert(await page.isVisible('#outputRecipe'),'unchanged capability refresh keeps generated code');
    assert.equal(requests.configurationWrites.length,writes);assert.equal(requests.encoded,encodedBeforeRecipe);
    assert.match(await page.textContent('#outputScope'),/My application only/);
    pairedSupported=false;await page.evaluate(()=>refreshCapabilities());assert(await page.isDisabled('#outputApply'));assert.match(await page.textContent('#outputWarning'),/Paired outputs are unavailable/);pairedSupported=true;await page.evaluate(()=>refreshCapabilities());assert(!(await page.isDisabled('#outputApply')));checks.push('legacy gateway cannot generate unsupported paired subscription');
    checks.push('exact three physical delivery names','advanced details collapsed','deterministic application recipe rates','depth always includes validity','simultaneous picture plus numeric recipe','authenticated selected-endpoint validation','recipe has no subscription or settings side effects','recipe survives unchanged refresh');
    await page.check('[name="outputPath"][value="usb_device"]');assert(await page.isDisabled('#outputApply'));assert.match(await page.textContent('#outputPathStatus'),/setup required/);
    await page.check('[name="outputPath"][value="wifi"]');fixturePaths.wifi.raw_products_permitted=false;fixturePaths.wifi.products_permitted=false;await page.evaluate(()=>refreshTransport());
    assert(await page.isDisabled('#outputApply'),'numeric WLAN permission is independently enforced');
    await page.uncheck('#outputDataEnabled');assert(!(await page.isDisabled('#outputApply')),'encoded WLAN remains permitted');
    await page.click('#outputApply');assert.match(await page.textContent('#outputRecipe'),/client.encoded_frames\(cadence_hz=2\)/);
    await page.check('#outputDataEnabled');await page.uncheck('#outputPictureEnabled');await page.click('#outputUsePicture');assert(await page.isChecked('#outputDataEnabled'),'adding picture cannot silently remove numeric data');assert(await page.isChecked('#outputPictureEnabled'));
    fixturePaths.wifi.raw_products_permitted=true;fixturePaths.wifi.products_permitted=true;await page.evaluate(()=>refreshTransport());
    await page.check('[name="outputPath"][value="ethernet"]');await page.fill('#outputCadence','3');await page.click('nav [data-page="overview"]');await page.click('nav [data-page="network"]');assert.equal(await page.inputValue('#outputCadence'),'3');await page.click('#outputDiscard');assert.equal(await page.inputValue('#outputCadence'),'2');
    checks.push('unavailable USB is honest','WLAN picture allowed with raw permission off','adding encoded picture preserves numeric selection','application drafts survive navigation and discard');
    fixturePaths.ethernet.interfaces.push({name:'down0',admin_up:false,carrier:true,addresses:[fixtureAddress('192.0.2.30')]},{name:'idle0',admin_up:true,carrier:false,addresses:[fixtureAddress('192.0.2.31')]});
    fixturePaths.ethernet.interfaces[0].addresses.push(fixtureAddress('fe80::1'),fixtureAddress('fe90::2'),fixtureAddress('febf::3'),fixtureAddress('fe80::4%eth0'));
    await page.evaluate(()=>refreshTransport());assert.deepEqual(await page.locator('#outputPathAddress option').evaluateAll(items=>items.map(item=>item.value)),['192.0.2.10','192.0.2.11']);
    fixturePaths.ethernet.interfaces[0].addresses=fixturePaths.ethernet.interfaces[0].addresses.filter(item=>item.address!=='192.0.2.10');await page.evaluate(()=>refreshTransport());assert(await page.isDisabled('#outputApply'));assert.match(await page.locator('#outputPathAddress option:checked').textContent(),/no longer reported/);
    fixturePaths=defaultPaths();fixtureConnection={...defaultConnection(),path:'usb_host_adapter',interface:'usb-host0'};await page.evaluate(()=>refreshCapabilities());assert.match(await page.textContent('#outputConnection'),/adapter.*not USB-C/);fixtureConnection=defaultConnection();await page.evaluate(()=>refreshCapabilities());
    checks.push('only active unscoped served addresses offered','stale selected IP disables code instead of rerouting','USB host adapter is not called USB-C device');
    await page.goto(url+'#calibration');await page.locator('#calibrationCanvas').scrollIntoViewIfNeeded();await page.waitForFunction(()=>document.querySelector('#calibrationCanvas').closest('.sample-canvas').classList.contains('live'));
    assert(await page.isVisible('#calibrationCanvas'),'positioning preview exists without a calibration session');
    const beforePositioning=requests.configurationWrites.length,originalSource=encoding.source;await page.selectOption('#calibrationPreviewSide','right_color');assert.equal(requests.configurationWrites.length,beforePositioning);await page.click('#calibrationEnablePreview');await page.waitForFunction(()=>document.querySelector('#calibrationPreviewSideLabel').textContent.includes('right color'));assert.equal(encoding.codec,'jpeg');
    encoding.jpeg_quality=41;await page.evaluate(()=>refreshEncoding());await page.click('#calibrationRestorePreview');await page.waitForFunction(source=>document.querySelector('#calibrationPreviewSideLabel').textContent.includes(source.replaceAll('_',' ')),originalSource);assert.equal(encoding.jpeg_quality,41);checks.push('calibration source changes require apply and preserve codec','restoring positioning source preserves newer quality settings');
    await page.click('#calibrationPause');const calibrationBefore=requests.encoded;await page.waitForTimeout(800);assert.equal(requests.encoded,calibrationBefore);assert(await page.isVisible('#calibrationCanvas'));
    await page.click('#calibrationPause');await page.waitForFunction(()=>document.querySelector('#calibrationCanvas').closest('.sample-canvas').classList.contains('live'));
    await page.locator('#calibration > summary').click();const collapsed=requests.encoded;await page.waitForTimeout(800);assert.equal(requests.encoded,collapsed);
    checks.push('calibration preview permanently sized before session','calibration pause and collapse release preview demand');
    for(const width of [1440,390]){
      await page.setViewportSize({width,height:844});await page.goto(url+'#calibration');await page.locator('#calibrationCanvas').scrollIntoViewIfNeeded();await page.waitForFunction(()=>encodedPreviewPage());
      await page.goto(url+'#streaming');await page.waitForFunction(()=>!encodedPreviewPage());const offscreenRequests=requests.encoded;await page.waitForTimeout(800);assert.equal(requests.encoded,offscreenRequests);
      await page.locator('#calibrationCanvas').scrollIntoViewIfNeeded();await page.waitForFunction(()=>document.querySelector('#calibrationCanvas').closest('.sample-canvas').classList.contains('live'));
    }
    await page.setViewportSize({width:1440,height:1050});checks.push('desktop and mobile offscreen calibration releases demand; scrolling back resumes');
    encoding.mode='raw';await page.goto(url+'#viewer');await page.evaluate(()=>refreshEncoding());assert(await page.isVisible('#encodedCanvas'));assert.match(await page.textContent('#encodedEmptyState'),/paused globally/);
    encoding.mode='both';await page.evaluate(()=>refreshEncoding());await page.waitForFunction(()=>document.querySelector('#viewerReceiving').textContent.includes('JPEG picture'));
    const sourceWrites=requests.configurationWrites.length;await page.selectOption('#previewSource','depth');await page.locator('#previewSourceApply').focus();await page.evaluate(()=>refreshEncoding());assert.equal(await page.inputValue('#previewSource'),'depth','polling preserves an unfocused source draft');assert.match(await page.textContent('#previewSourceState'),/not applied/);assert.equal(requests.configurationWrites.length,sourceWrites,'source draft never reconfigures encoder');encodingPostDelay=1200;await page.click('#previewSourceApply');assert(await page.isDisabled('#previewSource'));assert(await page.isDisabled('#previewSourceReset'));await page.evaluate(()=>refreshEncoding());assert(await page.isDisabled('#previewSourceApply'),'polling cannot unlock a pending source write');await page.waitForFunction(()=>!previewSourceBusy);encodingPostDelay=0;await page.locator('#encodedCanvas').scrollIntoViewIfNeeded();await page.waitForFunction(()=>document.querySelector('#encodedFormat').textContent.includes('depth visualization'));assert.match(await page.textContent('#previewSourceState'),/all encoded clients/);
    checks.push('globally paused encoder keeps preview rectangle','shared source changes require explicit Apply and label actual visualization','source controls stay locked during save and refresh');
    assert.deepEqual(rawAttempts,[]);assert.deepEqual(requests.raw,[]);
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

    pending=true;await page.goto(url+'#overview');await page.reload();await page.waitForFunction(()=>document.querySelector('#setupState').textContent.includes('Calibration needed'));assert(await page.isVisible('#setupState'));
    readinessOverride={state:'calibration_accepted',label:'Calibration accepted',message:'Matching per-unit declared validation; camera disconnected.',next_action:'Inspect camera/service health and verify fresh output.',accepted:true};
    await page.reload();await page.waitForFunction(()=>document.querySelector('#calibrationState').textContent==='Calibration accepted');
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
