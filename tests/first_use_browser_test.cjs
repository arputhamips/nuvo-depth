/* Render the actual console against deterministic API fixtures; no Pi is touched. */
const assert=require('node:assert/strict'),http=require('node:http'),fs=require('node:fs'),path=require('node:path');
const {chromium}=require(process.env.NUVO_PLAYWRIGHT_MODULE||'playwright-core');
const root=path.resolve(__dirname,'..'),out=path.join(root,'output/first-use-browser');fs.mkdirSync(out,{recursive:true});
let sequence=0,invalid=false,pending=false,readinessOverride=null;
const encoding=JSON.parse(fs.readFileSync(path.join(root,'config/encoding.json')));encoding.mode='raw';
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
const server=http.createServer((request,response)=>{
  const url=new URL(request.url,'http://localhost');const json=(value,status=200)=>{response.writeHead(status,{'content-type':'application/json',ETag:'"'+'a'.repeat(64)+'"'});response.end(JSON.stringify(value));};
  if(url.pathname==='/api/v1/session')return json({csrf:'f'.repeat(64)});
  if(url.pathname==='/api/v1/setup/status')return json({result:{schema_version:1,display_name:'First-use test fixture',phase:'installed',calibration_state:'pending',engineering:!pending,calibration_readiness:readinessOverride||{state:pending?'calibration_needed':'preview_enabled',label:pending?'Calibration needed':'Preview enabled',message:pending?'Capture is waiting for calibration or explicit preview.':'Distance accuracy remains unverified.',next_action:pending?'Import calibration or deliberately enable preview.':'Continue evaluation or import accepted calibration.',accepted:false}}});
  if(url.pathname==='/api/v1/health')return pending?json({error:'capture waiting for calibration'},503):json({...metadataBase,status:'running',capture_sequence:sequence,image_width:728,image_height:544,depth_width:728,depth_height:544,capture_target_hz:10,processing_ms:42,depth_published:sequence,exposure_mode:'manual',stereo_backend:'cpu_sgbm'});
  if(url.pathname==='/api/v1/capabilities')return json({products:['left_rectified','right_rectified','depth','validity','left_mono'],connection:{qualified_link:false,resource_budget_bytes_per_second:20000000}});
  if(url.pathname==='/api/v1/encoding')return json(encoding);
  if(url.pathname==='/api/v1/stereo'||url.pathname==='/api/v1/exposure')return json(JSON.parse(fs.readFileSync(path.join(root,'config',url.pathname.endsWith('stereo')?'stereo.json':'exposure.json'))));
  if(url.pathname==='/api/v1/transport')return json({preference:'auto',active:'ethernet'});
  if(url.pathname==='/api/v1/frame/latest'){if(pending)return json({error:'capture waiting for calibration'},503);response.writeHead(200,{'content-type':'application/octet-stream'});return response.end(packet((url.searchParams.get('products')||'left_mono').split(',')));}
  if(url.pathname.startsWith('/api/'))return json({},404);
  const name=url.pathname==='/'?'index.html':url.pathname.slice(1),file=path.join(root,'web',name);
  if(!file.startsWith(path.join(root,'web')+path.sep)||!fs.existsSync(file)){response.writeHead(404);return response.end();}
  response.setHeader('content-type',name.endsWith('.js')?'text/javascript':name.endsWith('.css')?'text/css':name.endsWith('.json')?'application/json':name.endsWith('.ttf')?'font/ttf':'text/html');response.end(fs.readFileSync(file));
});
server.on('upgrade',(request,socket)=>{socket.on('error',()=>{});socket.end('HTTP/1.1 426 Upgrade Required\r\nConnection: close\r\n\r\n');});
(async()=>{
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
  const browser=await chromium.launch({headless:true,executablePath:process.env.BROWSER_EXECUTABLE||'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe'});
  try{
    const context=await browser.newContext({acceptDownloads:true,viewport:{width:1440,height:1050}}),page=await context.newPage(),errors=[];
    page.on('pageerror',error=>errors.push(error.message));const url=`http://127.0.0.1:${server.address().port}/`;
    await page.goto(url);await page.waitForFunction(()=>document.querySelector('#connection').textContent==='Connected');
    await page.click('[data-page="viewer"]');assert(await page.isDisabled('#firstDepthSave'),'mono alone cannot complete depth');
    await page.click('#firstDepthStart');await page.waitForFunction(()=>!document.querySelector('#firstDepthSave').disabled);
    assert.match(await page.textContent('#firstDepthAuthority'),/accuracy unverified/);
    const downloadPromise=page.waitForEvent('download');await page.click('#firstDepthSave');const download=await downloadPromise;await download.saveAs(path.join(out,'sample.nuv'));
    const saved=fs.readFileSync(path.join(out,'sample.nuv')),meta=JSON.parse(saved.subarray(32,32+saved.readUInt32BE(12)));
    assert.equal(meta.calibration_authority,0);assert.equal(meta.calibration_sha256,metadataBase.calibration_sha256);assert(meta.products.some(p=>p.kind==='depth')&&meta.products.some(p=>p.kind==='validity'));
    await page.screenshot({path:path.join(out,'first-use-console.png'),fullPage:false});
    invalid=true;await page.click('#firstDepthStart');await page.waitForFunction(()=>document.querySelector('#firstDepthError').textContent.includes('No valid depth'));assert(await page.isDisabled('#firstDepthSave'));
    invalid=false;await page.click('#firstDepthStart');await page.waitForFunction(()=>!document.querySelector('#firstDepthSave').disabled);
    await context.setOffline(true);await page.waitForFunction(()=>document.querySelector('#firstDepthSave').disabled);await context.setOffline(false);
    await page.waitForFunction(()=>!document.querySelector('#firstDepthSave').disabled);
    pending=true;await page.reload();await page.waitForFunction(()=>document.querySelector('#setupState').textContent.includes('Calibration needed'));assert(await page.isDisabled('#firstDepthSave'));
    readinessOverride={state:'calibration_accepted',label:'Calibration accepted',message:'Matching per-unit declared validation; camera disconnected.',next_action:'Inspect camera/service health and verify fresh output.',accepted:true};
    await page.reload();await page.waitForFunction(()=>document.querySelector('#calibrationState').textContent==='Calibration accepted');assert(await page.isDisabled('#firstDepthSave'));
    assert.equal(await page.textContent('#connection'),'Connected');
    readinessOverride={state:'unavailable',label:'Status unavailable/error',message:'Calibration belongs to another device.',next_action:'Import the original compatible bundle.',accepted:false};
    await page.reload();await page.waitForFunction(()=>document.querySelector('#calibrationState').textContent==='Status unavailable/error');
    await page.click('[data-page="calibration"]');assert.match(await page.textContent('#calibrationReadinessMessage'),/another device/);assert.match(await page.textContent('#calibrationReadinessAction'),/original compatible bundle/);
    const advanced=page.locator('details').filter({hasText:'Advanced geometry and qualification parameters'});assert.equal(await advanced.getAttribute('open'),null);
    assert.deepEqual(errors,[]);fs.writeFileSync(path.join(out,'results.json'),JSON.stringify({fixture:true,checks:['mono is insufficient','fresh stereo/depth/validity','saved raw metadata','all-invalid rejection','disconnect','reconnect','pending calibration','accepted with unavailable camera','specific readiness error and recovery','advanced calibration collapsed'],pageErrors:errors},null,2));
    console.log('Browser first-use and readiness journeys: 10 fixture checks passed; raw metadata preserved; no page errors.');
  }finally{await browser.close();await new Promise(resolve=>server.close(resolve));}
})().catch(error=>{console.error(error);server.close();process.exitCode=1;});
