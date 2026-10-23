/* Actual gateway, TLS pinned to this disposable fixture's public key only. */
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {chromium}=require(process.env.NUVO_PLAYWRIGHT_MODULE||'playwright');
const manifest=JSON.parse(fs.readFileSync(process.argv[2],'utf8'));assert.equal(manifest.fixture,true);
const local=p=>process.platform==='win32'?p.replace(/^\/mnt\/([a-z])\//,(_,drive)=>drive.toUpperCase()+':/'):p;
const out=path.resolve('output/gateway-browser');fs.mkdirSync(out,{recursive:true});
const update=value=>fs.writeFileSync(local(manifest.control_file),JSON.stringify(value));
(async()=>{
  const executable=process.env.BROWSER_EXECUTABLE||(process.platform==='win32'?'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe':undefined);
  const browser=await chromium.launch({headless:true,...(executable?{executablePath:executable}:{}),args:[`--ignore-certificate-errors-spki-list=${manifest.certificate_spki_sha256}`]});
  const context=await browser.newContext({acceptDownloads:true,viewport:{width:1440,height:1050}});
  await context.tracing.start({screenshots:true,snapshots:true,sources:true});
  const page=await context.newPage(),errors=[],checks=[];page.on('pageerror',error=>errors.push(error.stack||error.message));
  const get=route=>page.evaluate(async route=>{const response=await fetch(route,{credentials:'same-origin'});if(!response.ok)throw new Error(`${route}: ${response.status}`);return response.json()},route);
  const task=async name=>page.check(`[name="outputTask"][value="${name}"]`);
  const open=async section=>{await page.goto(manifest.origin+'/#'+section);};
  try{
    await page.goto(manifest.origin);await page.waitForFunction(()=>document.querySelector('#connection').textContent==='Connected');
    assert.match(await page.textContent('#fixtureBanner'),/TEST FIXTURE/);
    assert.deepEqual(await page.locator('nav [data-page]').evaluateAll(items=>items.map(item=>item.dataset.page)),['viewer','network','device']);
    await page.locator('#firstDepthGuide > summary').click();await page.click('#firstDepthStart');
    await page.waitForFunction(()=>!document.querySelector('#firstDepthSave').disabled);
    await page.screenshot({path:path.join(out,'real-gateway-first-depth.png')});
    assert.equal(await page.inputValue('#liveRecipe'),'depth_reference');
    await page.click('nav [data-page="network"]');
    for(const [preset,names,rate] of [['stereo',['left_rectified','right_rectified','disparity','validity'],'1'],['camera',['left_mono'],'5'],['depth',['depth','validity'],'2']]){
      await task(preset);assert.equal(await page.inputValue('#outputCadence'),rate);
      assert.equal(await page.locator('#outputAdvanced').evaluate(element=>element.open),false);
      await page.click('#outputApply');
      await page.waitForFunction(names=>names.every(name=>outputSettings.products.includes(name))&&outputSettings.products.length===names.length,names);
      await page.waitForFunction(()=>document.querySelector('#outputFrameSummary').textContent.includes('16 × 12'));
    }
    assert.match(await page.textContent('#outputBandwidth'),/image grid 16 × 12.*depth grid 16 × 12/);
    await page.locator('#outputAdvanced > summary').click();
    assert.equal(await page.locator('#outputAdvanced').evaluate(element=>element.open),true);
    assert.equal(await page.locator('[name="outputProduct"]').count(),14);
    await page.click('[name="outputProduct"][value="validity"]');
    assert(await page.isChecked('[name="outputProduct"][value="validity"]'));
    await page.click('#outputApply');
    await page.waitForFunction(()=>rawSocket?.readyState===WebSocket.OPEN);
    await page.evaluate(()=>{window.fixturePreviousSocket=rawSocket;rawSocket.close();});
    await page.waitForFunction(()=>rawSocket&&rawSocket!==window.fixturePreviousSocket&&rawSocket.readyState===WebSocket.OPEN&&!rawPolling);
    await page.waitForFunction(()=>document.querySelector('#outputFrameSummary').textContent.includes('Depth validity'));
    await page.screenshot({path:path.join(out,'real-gateway-output-presets.png')});
    checks.push('automatic session','three top-level destinations','named first depth','deterministic recipe rates','automatic validity','actual product grids','interrupted WebSocket recovery');

    const selection=await page.evaluate(()=>JSON.stringify(outputSettings));
    const encodingBefore=await get('/api/v1/encoding'),transportBefore=await get('/api/v1/transport');
    assert.equal(typeof transportBefore.wifi_enabled,'boolean','fixture must use the production Wi-Fi policy API');
    await page.waitForFunction(()=>!document.querySelector('#wifiEnabled').disabled);
    await page.evaluate(()=>window.fixturePreviousSocket=rawSocket);
    const beforeWifi=await page.evaluate(()=>lastSequence);
    await page.locator('#wifiEnabled').setChecked(!transportBefore.wifi_enabled);await page.click('#wifiApply');
    await page.waitForFunction(()=>!document.querySelector('#wifiConfirm').hidden);
    await page.waitForFunction(sequence=>lastSequence>sequence,beforeWifi);
    assert(await page.evaluate(()=>rawSocket===window.fixturePreviousSocket&&rawSocket.readyState===WebSocket.OPEN),'Wi-Fi-only policy must leave this non-WiFi stream open');
    assert.equal((await get('/api/v1/transport')).preference,transportBefore.preference);
    assert.equal((await get('/api/v1/transport')).wifi_enabled,!transportBefore.wifi_enabled);
    assert.equal(await page.evaluate(()=>JSON.stringify(outputSettings)),selection);
    assert.deepEqual(await get('/api/v1/encoding'),encodingBefore);
    await page.reload();await page.waitForFunction(()=>document.querySelector('#connection').textContent==='Connected');
    await page.waitForFunction(()=>!document.querySelector('#wifiConfirm').hidden);
    assert.match(await page.textContent('#wifiState'),/provisional|pending/i,'a provisional device policy remains visible after reload');
    await page.click('#wifiConfirm');await page.waitForFunction(()=>document.querySelector('#wifiConfirm').hidden);
    await page.locator('#wifiEnabled').setChecked(transportBefore.wifi_enabled);await page.click('#wifiApply');
    await page.waitForFunction(()=>!document.querySelector('#wifiConfirm').hidden);await page.click('#wifiConfirm');
    await page.waitForFunction(()=>document.querySelector('#wifiConfirm').hidden);
    assert.equal((await get('/api/v1/transport')).wifi_enabled,transportBefore.wifi_enabled);
    update({state:'preview_enabled',run:2,gateway_restart:0,transport_off:true});
    await page.waitForFunction(async()=>(await (await fetch('/api/v1/transport')).json()).preference==='off');
    await page.evaluate(()=>refreshCapabilities());
    await page.waitForFunction(()=>document.querySelector('#outputReceiving').textContent.startsWith('Blocked'));
    assert.doesNotMatch(await page.textContent('#viewerReceiving'),/^Receiving:/);
    assert.equal(await page.textContent('#connection'),'Connected','master product Off must leave the console accessible');
    update({state:'preview_enabled',run:2,gateway_restart:0,transport_off:false});
    await page.waitForFunction(async()=>(await (await fetch('/api/v1/transport')).json()).preference!=='off');
    await page.evaluate(()=>refreshCapabilities());
    await page.waitForFunction(()=>document.querySelector('#outputReceiving').textContent.startsWith('Receiving:'));
    checks.push('master Off reports Blocked while console remains connected');
    checks.push('real Wi-Fi policy preserves preference and wired stream','shared policy independent from products and encoder','pending Wi-Fi transaction survives reload','console remains accessible through policy changes');

    await task('camera');await page.selectOption('#outputDestination','app');
    const recipeSelection=await page.evaluate(()=>JSON.stringify(outputSettings));
    await page.evaluate(()=>window.fixturePreviousSocket=rawSocket);
    await page.click('#outputApply');assert(await page.isVisible('#outputRecipe'));
    await page.evaluate(()=>refreshCapabilities());assert(await page.isVisible('#outputRecipe'));
    assert.equal(await page.evaluate(()=>JSON.stringify(outputSettings)),recipeSelection);
    assert(await page.evaluate(()=>rawSocket===window.fixturePreviousSocket),'generating code must not restart the browser stream');
    assert.deepEqual(await get('/api/v1/encoding'),encodingBefore);
    await page.selectOption('#outputDestination','browser');await page.click('#outputDiscard');
    checks.push('application recipe has no subscription or shared-encoder side effects');

    await page.selectOption('#pictureQuality','compact');
    const desiredQuality=encodingBefore.codec==='jpeg'?45:1000000;
    assert.match(await page.textContent('#pictureQualityState'),/Editing.*not applied/);
    assert.deepEqual(await get('/api/v1/encoding'),encodingBefore,'quality editing requires an explicit apply');
    const savedQuality=page.waitForResponse(response=>response.url().endsWith('/api/v1/encoding')&&response.request().method()==='POST');
    await page.click('#pictureQualityApply');assert.equal((await savedQuality).status(),202);
    const afterQuality=await get('/api/v1/encoding'),qualityField=encodingBefore.codec==='jpeg'?'jpeg_quality':'bitrate_bps';
    assert.equal(afterQuality[qualityField],desiredQuality);
    assert.deepEqual({...afterQuality,[qualityField]:encodingBefore[qualityField]},encodingBefore,'quality preserves source, codec, dimensions, mode, rate and other shared controls');
    assert.equal(await page.evaluate(()=>JSON.stringify(outputSettings)),recipeSelection,'shared quality changes must not silently change local products');
    checks.push('shared quality requires apply and preserves source, codec, mode, rate and browser products');

    await open('streaming');
    assert.match(await page.textContent('#encodingForm'),/every network client.*one shared preview encoder/s);
    await page.locator('#streamRawEnabled').uncheck();
    const savedPictureMode=page.waitForResponse(response=>response.url().endsWith('/api/v1/encoding')&&response.request().method()==='POST');
    await page.locator('#encodingForm button[type="submit"]').click();assert.equal((await savedPictureMode).status(),202);
    await open('viewer');await page.selectOption('#liveRecipe','picture');
    await page.locator('#liveSelectionForm button[type="submit"]').click();
    await page.waitForFunction(()=>document.querySelector('#viewerReceiving').textContent.includes('JPEG'));
    assert(await page.isVisible('#viewerPicture'));assert(await page.isHidden('#pixelInspector'));
    assert(await page.isHidden('#viewerModeNotice'),'receiving the selected picture must not still ask the user to choose picture preview');
    assert.match(await page.textContent('#pictureMeasurementNotice'),/Numeric value unavailable/);
    assert(await page.locator('#encodedCanvas').evaluate(canvas=>canvas.width===16&&canvas.height===12));
    await page.screenshot({path:path.join(out,'real-gateway-compressed-live.png')});
    await page.click('#viewerPause');assert.match(await page.textContent('#viewerReceiving'),/Paused/);
    assert.doesNotMatch(await page.textContent('#viewerReceiving'),/Receiving:/);
    await page.click('#viewerPause');await page.waitForFunction(()=>document.querySelector('#viewerReceiving').textContent.includes('JPEG'));
    await open('streaming');await page.locator('#streamRawEnabled').check();
    const restoredData=page.waitForResponse(response=>response.url().endsWith('/api/v1/encoding')&&response.request().method()==='POST');
    await page.locator('#encodingForm button[type="submit"]').click();assert.equal((await restoredData).status(),202);
    await open('viewer');await page.selectOption('#liveRecipe','depth');await page.locator('#liveSelectionForm button[type="submit"]').click();
    await page.waitForFunction(()=>document.querySelector('#viewerReceiving').textContent.includes('Depth values'));
    checks.push('compressed-only Live view decodes real JPEG','compressed picture has no numeric inspector','pause and resume use truthful receive state');

    await open('diagnostics');
    await page.waitForFunction(()=>!document.querySelector('#downloadDiagnostics').disabled);
    const downloaded=page.waitForEvent('download');await page.click('#downloadDiagnostics');
    const download=await downloaded;await download.saveAs(path.join(out,'support.json'));
    const support=JSON.parse(fs.readFileSync(path.join(out,'support.json')));
    assert.equal(support.report_type,'nuvo-depth-support');assert.equal(support.privacy.raw_logs_included,false);
    assert(support.checks.every(check=>check.id&&check.status&&check.reason&&check.next_action));
    await open('recovery');await page.waitForFunction(()=>document.querySelector('#installedVersions').textContent.includes('nuvo-depth-core'));
    assert.match(await page.textContent('#upgradeNotice'),/not queried/);
    for(const [state,label] of [['calibration_needed','Calibration needed'],['unavailable','Status unavailable/error'],['calibration_accepted','Calibration accepted'],['preview_enabled','Preview enabled']]){
      update({state,run:2,gateway_restart:0});
      await page.waitForFunction(label=>document.querySelector('#calibrationState').textContent===label,label);
    }
    await open('viewer');
    await page.waitForFunction(()=>!document.querySelector('#depthProfileChoice').disabled);
    await page.selectOption('#depthProfileChoice','fast');
    assert.match(await page.textContent('#depthProfileState'),/Pending matcher changes/);
    await open('algorithm');
    await page.locator('#blockSize').fill('7');
    assert.equal(await page.inputValue('#depthProfileChoice'),'custom','a manual matcher edit must immediately stop claiming a named profile');
    assert.match(await page.textContent('#depthProfileState'),/Pending matcher changes.*Custom/);
    checks.push('manual matcher edits immediately show pending Custom');
    const current=await page.evaluate(async()=>{const response=await fetch('/api/v1/stereo',{credentials:'same-origin'});return {configuration:await response.json(),etag:response.headers.get('ETag')};}),configuration=current.configuration;
    configuration.uniqueness_ratio=(configuration.uniqueness_ratio+1)%20;
    const changed=await page.evaluate(async({configuration,etag})=>(await fetch('/api/v1/stereo',{method:'POST',credentials:'same-origin',headers:{'Content-Type':'application/json','X-Nuvo-CSRF':csrf,'If-Match':etag},body:JSON.stringify(configuration)})).status,{configuration,etag:current.etag});
    assert.equal(changed,202);
    await page.locator('#algorithmForm').evaluate(form=>form.requestSubmit());
    await page.waitForFunction(()=>document.querySelector('#algorithmError').textContent.length>0);
    assert.equal(await page.inputValue('#blockSize'),'7');
    update({state:'preview_enabled',run:3,gateway_restart:1});
    await page.waitForTimeout(1000);await page.reload();await page.waitForFunction(()=>document.querySelector('#connection').textContent==='Connected');
    for(const width of [320,390])for(const theme of ['light','dark']){
      await page.evaluate(theme=>applyTheme(theme,false),theme);await page.setViewportSize({width,height:844});
      await open('diagnostics');await page.waitForFunction(()=>!document.querySelector('#downloadDiagnostics').disabled);
      await page.keyboard.press('Tab');assert.notEqual(await page.evaluate(()=>document.activeElement.tagName),'BODY');
      for(const destination of ['viewer','network','device']){
        await page.click(`nav [data-page="${destination}"]`);
        await page.waitForFunction(()=>window.scrollY<=1);
        assert(await page.evaluate(()=>document.documentElement.scrollWidth<=window.innerWidth+2),`${destination} must fit ${width}px in ${theme}`);
        await page.screenshot({path:path.join(out,`${destination}-${width}-${theme}.png`),fullPage:false});
      }
    }
    checks.push('allowlisted support export','installed versions','four readiness states','revision conflict preserves edits','gateway restart/reconnect','320px and 390px light/dark layouts and keyboard');
    assert.deepEqual(errors,[]);
    fs.writeFileSync(path.join(out,'results.json'),JSON.stringify({fixture:true,actual_gateway:true,tls:'fixture-specific SPKI; SDK uses verified CA',network_boundary:'Actual host receiving interface; physical WiFi admission is tested separately on the Pi.',checks,pageErrors:errors},null,2));
    console.log('Actual gateway browser journeys passed.');
  }catch(error){await page.screenshot({path:path.join(out,'failure.png'),fullPage:true}).catch(()=>{});throw error;}
  finally{await context.tracing.stop({path:path.join(out,'trace.zip')});await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
