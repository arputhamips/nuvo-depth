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
  const page=await context.newPage(),errors=[];page.on('pageerror',error=>errors.push(error.message));
  try{
    await page.goto(manifest.origin);await page.waitForFunction(()=>document.querySelector('#connection').textContent==='Connected');
    assert.match(await page.textContent('#fixtureBanner'),/TEST FIXTURE/);
    await page.click('[data-page="viewer"]');await page.click('#firstDepthStart');
    await page.waitForFunction(()=>!document.querySelector('#firstDepthSave').disabled);
    await page.screenshot({path:path.join(out,'real-gateway-first-depth.png')});
    await page.click('[data-page="network"]');
    for(const [preset,names] of [['camera',['left_mono']],['depth',['depth','validity']],['stereo',['left_rectified','right_rectified','disparity','validity']]]){
      await page.selectOption('#outputPreset',preset);
      assert.equal(await page.locator('#outputAdvanced').evaluate(element=>element.open),false);
      await page.locator('#outputForm').evaluate(form=>form.requestSubmit());
      await page.waitForFunction(names=>names.every(name=>outputSettings.products.includes(name))&&outputSettings.products.length===names.length,names);
      await page.waitForFunction(()=>document.querySelector('#outputFrameSummary').textContent.includes('16 × 12'));
    }
    assert.match(await page.textContent('#outputBandwidth'),/images 16 × 12.*matcher 16 × 12/);
    await page.selectOption('#outputPreset','custom');
    assert.equal(await page.locator('#outputAdvanced').evaluate(element=>element.open),true);
    assert.equal(await page.locator('[name="outputProduct"]').count(),14);
    await page.selectOption('#outputPreset','depth');
    await page.locator('#outputForm').evaluate(form=>form.requestSubmit());
    await page.waitForFunction(()=>rawSocket?.readyState===WebSocket.OPEN);
    await page.evaluate(()=>{window.fixturePreviousSocket=rawSocket;rawSocket.close();});
    await page.waitForFunction(()=>rawSocket&&rawSocket!==window.fixturePreviousSocket&&rawSocket.readyState===WebSocket.OPEN&&!rawPolling);
    await page.waitForFunction(()=>document.querySelector('#outputFrameSummary').textContent.includes('Depth validity'));
    await page.screenshot({path:path.join(out,'real-gateway-output-presets.png')});
    await page.click('[data-page="diagnostics"]');
    await page.waitForFunction(()=>!document.querySelector('#downloadDiagnostics').disabled);
    const downloaded=page.waitForEvent('download');await page.click('#downloadDiagnostics');
    const download=await downloaded;await download.saveAs(path.join(out,'support.json'));
    const support=JSON.parse(fs.readFileSync(path.join(out,'support.json')));
    assert.equal(support.report_type,'nuvo-depth-support');assert.equal(support.privacy.raw_logs_included,false);
    assert(support.checks.every(check=>check.id&&check.status&&check.reason&&check.next_action));
    await page.click('[data-page="recovery"]');await page.waitForFunction(()=>document.querySelector('#installedVersions').textContent.includes('nuvo-depth-core'));
    assert.match(await page.textContent('#upgradeNotice'),/not queried/);
    for(const [state,label] of [['calibration_needed','Calibration needed'],['unavailable','Status unavailable/error'],['calibration_accepted','Calibration accepted'],['preview_enabled','Preview enabled']]){
      update({state,run:2,gateway_restart:0});
      await page.waitForFunction(label=>document.querySelector('#calibrationState').textContent===label,label);
    }
    await page.click('[data-page="algorithm"]');
    await page.locator('#blockSize').fill('7');
    const current=await page.evaluate(async()=>{const response=await fetch('/api/v1/stereo',{credentials:'same-origin'});return {configuration:await response.json(),etag:response.headers.get('ETag')};}),configuration=current.configuration;
    configuration.uniqueness_ratio=(configuration.uniqueness_ratio+1)%20;
    const changed=await page.evaluate(async({configuration,etag})=>(await fetch('/api/v1/stereo',{method:'POST',credentials:'same-origin',headers:{'Content-Type':'application/json','X-Nuvo-CSRF':csrf,'If-Match':etag},body:JSON.stringify(configuration)})).status,{configuration,etag:current.etag});
    assert.equal(changed,202);
    await page.locator('#algorithmForm').evaluate(form=>form.requestSubmit());
    await page.waitForFunction(()=>document.querySelector('#algorithmError').textContent.length>0);
    assert.equal(await page.inputValue('#blockSize'),'7');
    update({state:'preview_enabled',run:3,gateway_restart:1});
    await page.waitForTimeout(1000);await page.reload();await page.waitForFunction(()=>document.querySelector('#connection').textContent==='Connected');
    for(const theme of ['light','dark']){
      await page.evaluate(theme=>applyTheme(theme,false),theme);await page.setViewportSize({width:390,height:844});
      await page.click('[data-page="diagnostics"]');await page.waitForFunction(()=>!document.querySelector('#downloadDiagnostics').disabled);
      await page.keyboard.press('Tab');assert.notEqual(await page.evaluate(()=>document.activeElement.tagName),'BODY');
      await page.screenshot({path:path.join(out,`mobile-${theme}.png`),fullPage:true});
      await page.click('[data-page="network"]');
      assert(await page.evaluate(()=>document.documentElement.scrollWidth<=window.innerWidth+2),'Outputs must fit the mobile viewport');
      await page.screenshot({path:path.join(out,`mobile-outputs-${theme}.png`),fullPage:true});
    }
    assert.deepEqual(errors,[]);
    fs.writeFileSync(path.join(out,'results.json'),JSON.stringify({fixture:true,actual_gateway:true,tls:'fixture-specific SPKI; SDK uses verified CA',checks:['automatic session','fresh associated frame','task presets and advanced selection','actual product grids','interrupted WebSocket reconnects without reload','allowlisted export','installed versions','four readiness states','revision conflict preserves edits','gateway restart/reconnect','390px keyboard light/dark and Outputs layout'],pageErrors:errors},null,2));
    console.log('Actual gateway browser journeys passed.');
  }catch(error){await page.screenshot({path:path.join(out,'failure.png'),fullPage:true}).catch(()=>{});throw error;}
  finally{await context.tracing.stop({path:path.join(out,'trace.zip')});await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
