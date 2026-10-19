/* Visual editor for existing device controls and browser subscriptions. No graph executor. */
(() => {
  'use strict';
  const root=document.getElementById('pipeline'),P=window.NuvoPipeline,H=window.NuvoPipelineHost;
  if(!root||!P||!H)return;
  const $=id=>document.getElementById(id),storageKey='nuvo-image-pipeline-v1';
  const nodeIds={exposure:'pipelineExposureNode',cameras:'pipelineCamerasNode',rectification:'pipelineRectificationNode',matcher:'pipelineMatcherNode',filter:'pipelineFilterNode',depth:'pipelineDepthNode',encoder:'pipelineEncoderNode'};
  const cards=new Map(),expandedNodes=new Set();let workspace=null;
  let applied=P.fromSettings(H.context().settings),draft=applied,undo=[],redo=[],connecting=null,editingWire=null,draggingProduct=null,dragging=false,gestureCancelled=false,toolNode='pipelineCamerasNode',connectionOrigin=null,selected=applied.outputs[0]?.id||'monitor';
  let lastFrame=null,lastPaint=0,lastSeen=0,frameRequest=0,routeMessage='',streamMessage='';
  const sameSettings=(graph,settings)=>{
    try {const result=P.compile(graph,{...H.context().limits,rawEnabled:true,qualified:true,bytesPerSecond:Number.MAX_SAFE_INTEGER,maxFrameBytes:Number.MAX_SAFE_INTEGER,supportedProducts:undefined});
      return result.enabled===settings.enabled&&(!result.enabled||[...result.products].sort().join() === [...settings.products].sort().join())&&result.cadence_hz===settings.cadence_hz&&result.delivery===settings.delivery&&result.preview===settings.preview&&result.preview_cadence_hz===settings.preview_cadence_hz;
    }catch(_){return false;}
  };
  try {const text=localStorage.getItem(storageKey);if(text){if(text.length>16384)throw new Error('Saved layout too large');const saved=P.normalize(JSON.parse(text));if(sameSettings(saved,H.context().settings))draft=applied=saved;}}
  catch(_){routeMessage='Current outputs loaded; the saved layout was invalid.';}
  const el=(tag,className,text)=>{const node=document.createElement(tag);if(className)node.className=className;if(text!==undefined)node.textContent=text;return node;};
  const dirty=()=>P.signature(draft)!==P.signature(applied);
  function announce(text){$('pipelineAnnouncement').textContent=text;}
  function persist(){try{localStorage.setItem(storageKey,JSON.stringify(applied));return true;}catch(_){return false;}}
  function change(next,{history=true,project=true}={}){
    next=P.normalize(next);if(P.signature(next)===P.signature(draft))return;
    if(history){undo.push(draft);undo=undo.slice(-20);redo=[];}draft=next;routeMessage='';streamMessage='';announce('');
    if(project)H.editOutputDraft(draft);renderRoutes();
  }
  function badge(text,tone='neutral'){return el('span','chip chip-'+tone,text);}
  function sourceSelect(value,id){
    const select=el('select');select.id=id;select.append(new Option('Not connected',''));
    for(const stage of P.STAGES){if(!stage.ports.length)continue;const group=el('optgroup');group.label=stage.title;stage.ports.forEach(product=>group.append(new Option(P.PRODUCTS[product].label,product)));select.append(group);}
    select.value=value||'';return select;
  }
  function selectPreview(id,{scroll=false}={}){
    closeNodeEditor({focus:false});const wasMonitor=selected==='monitor';selected=id;$('pipelinePreviewSelect').value=id;lastPaint=0;
    root.querySelectorAll('[data-output-id]').forEach(node=>node.classList.toggle('selected-output',node.dataset.outputId===id));
    refresh();paintPreview();scheduleWires();if(wasMonitor!==(id==='monitor'))H.restartPreview();
    if(scroll)showViewer(true);
  }
  const compactTools=matchMedia('(max-width:760px)');$('pipelineToolsDisclosure').open=!compactTools.matches;compactTools.addEventListener('change',event=>{$('pipelineToolsDisclosure').open=!event.matches;if(!event.matches)closeNodeEditor();});
  function focusWorkspace(){if(compactTools.matches){$('pipelineToolsDisclosure').open=false;$('pipelineWorkspace').scrollIntoView({block:'start'});}}
  let nodeEditor=null;
  function closeNodeEditor({focus=true}={}){
    if(!nodeEditor)return;const state=nodeEditor;nodeEditor=null;
    if($('pipelineNodeDialog').open)$('pipelineNodeDialog').close();
    for(const [element,marker] of state.items){marker.replaceWith(element);}
    state.body.hidden=true;state.card.classList.remove('expanded');expandedNodes.delete(state.card.id);
    state.toggle.setAttribute('aria-expanded','false');state.toggle.setAttribute('aria-label','Expand '+nodeTitle(state.card.id)+' settings');
    updateNodeTools();scheduleWires();if(focus){focusWorkspace();requestAnimationFrame(()=>{workspace?.reveal(state.card.id);state.toggle.focus({preventScroll:true});});}
  }
  function openNodeEditor(card,body,actions,toggle){
    closeNodeEditor({focus:false});const items=[body,...actions].map(element=>{const marker=document.createComment('node editor');element.before(marker);return[element,marker];});
    nodeEditor={card,body,items,toggle};$('pipelineNodeDialogTitle').textContent=nodeTitle(card.id)+' settings';
    body.hidden=false;$('pipelineNodeDialogContent').replaceChildren(body);$('pipelineNodeDialogActions').replaceChildren(...actions);
    $('pipelineNodeDialog').showModal();$('pipelineNodeDialogClose').focus();
  }
  $('pipelineNodeDialogClose').onclick=()=>closeNodeEditor();
  $('pipelineNodeDialog').addEventListener('cancel',event=>{event.preventDefault();closeNodeEditor();});
  $('pipelineNodeDialog').addEventListener('close',()=>{if(!$('pipelineNodeDialog').open)closeNodeEditor();});
  function encoderAccepts(product){const state=H.form('encoding');return !state.fields.streamSource.disabled&&[...$('streamSource').options].some(option=>option.value===product);}
  function finishConnection(message,{focus=true}={}){
    connecting=null;editingWire=null;draggingProduct=null;updateConnection();refreshTools();syncCanvas();if(message)announce(message);
    focusWorkspace();if(focus)(connectionOrigin?.isConnected&&!connectionOrigin.disabled&&connectionOrigin.getClientRects().length?connectionOrigin:$('pipelineWireSelect')).focus({preventScroll:true});
  }
  function beginConnection(product,target=null,edit=null,{focus=false}={}){
    if(!P.PRODUCTS[product])return;closeNodeEditor({focus:false});$('pipelineMoveBar').hidden=true;$('pipelineNodeMove').setAttribute('aria-expanded','false');connectionOrigin=document.activeElement;connecting=product;editingWire=edit;
    updateConnection();const destination=target||(!edit&&draft.outputs.length<P.MAX_OUTPUTS?'new':edit);if(destination&&[...$('pipelineConnectTarget').options].some(option=>option.value===destination))$('pipelineConnectTarget').value=destination;
    updateConnection();refreshTools();syncCanvas();if(compactTools.matches)$('pipelineConnectBar').scrollIntoView({block:'start'});if(focus)$('pipelineConnectSource').focus({preventScroll:true});
    announce(edit?'Edit this wire using Source and Destination, or drag a wire end.':'Choose the product, then tap an IN port or choose a destination and Connect.');
  }
  function editWire(key){
    if(key==='encoder'){const product={left_color:'left_rgb',right_color:'right_rgb'}[$('streamSource').value]||$('streamSource').value;if(P.PRODUCTS[product])beginConnection(product,'encoder','encoder');return;}
    const output=draft.outputs.find(o=>o.id===key);if(output)beginConnection(output.product||'left_mono',key,output.product?key:null);
  }
  function connectTo(target){
    if(!connecting)return;
    try{
      let message;
      if(target==='encoder'){
        if(editingWire&&editingWire!=='encoder')throw new Error('Output wires can only move between output nodes. Start a new connection for the encoder.');
        if(!encoderAccepts(connecting))throw new Error('Enable the encoder and choose a supported source in its Settings.');
        H.editForm('encoding','streamSource',connecting);message='Encoder source changed. Apply Streaming changes in the encoder node to save.';
      }else{
        if(editingWire==='encoder')throw new Error('The encoder wire stays connected to its encoder. Change its source here.');
        const next=target==='new'?P.addOutput(draft,connecting):editingWire?P.reconnect(draft,editingWire,target,connecting):P.connect(draft,target,connecting);
        const output=next.outputs.find(o=>o.id===target);change(next);
        message='Wire connected.'+(output?.enabled===false?' This output is off; enable it when needed.':'')+' Apply outputs to update this browser.';
      }
      finishConnection(message);
    }catch(error){announce(error.message);$('pipelineConnectHint').textContent=error.message;}
  }
  function updateOptions(select,options){const signature=JSON.stringify(options.map(o=>[o.value,o.textContent,o.disabled]));if(select.dataset.optionsSignature===signature)return;const previous=select.value;select.replaceChildren(...options);select.dataset.optionsSignature=signature;if(options.some(o=>o.value===previous&&!o.disabled))select.value=previous;}
  function updateConnection(){
    $('pipelineConnectBar').hidden=!connecting;$('pipelineNodeTools').hidden=!!connecting||!$('pipelineMoveBar').hidden;$('pipelineToolsDisclosure').hidden=$('pipelineNodeTools').hidden;
    root.querySelectorAll('[data-source-connect]').forEach(button=>button.setAttribute('aria-pressed',String(button.dataset.sourceConnect===connecting)));
    root.querySelectorAll('.pipeline-output-input').forEach(button=>{button.textContent=connecting?'Connect here':'Edit connection';button.classList.toggle('connecting',!!connecting);});
    if(!connecting)return;
    $('pipelineConnectLabel').textContent=editingWire?'Edit wire':'Connect ports';
    const product=$('pipelineConnectSource');if(!product.options.length)for(const stage of P.STAGES){if(!stage.ports.length)continue;const group=el('optgroup');group.label=stage.title;stage.ports.forEach(id=>group.append(new Option(P.PRODUCTS[id].label,id)));product.append(group);}product.value=connecting;
    const target=$('pipelineConnectTarget'),previous=target.value,options=[];
    draft.outputs.forEach(o=>{const option=new Option(o.name+(o.product?' · connected':''),o.id);option.disabled=editingWire==='encoder'||!!(editingWire&&o.id!==editingWire&&o.product);options.push(option);});
    const encoder=new Option('Monitoring encoder · shared device setting','encoder');encoder.disabled=!!(editingWire&&editingWire!=='encoder')||!encoderAccepts(connecting);options.push(encoder);
    if(!editingWire&&draft.outputs.length<P.MAX_OUTPUTS)options.push(new Option('New output node','new'));
    updateOptions(target,options);
    if([...target.options].some(o=>o.value===previous&&!o.disabled))target.value=previous;
    else target.value=editingWire==='encoder'?'encoder':editingWire||[...target.options].find(o=>!o.disabled)?.value||'';
    $('pipelineConnectConfirm').textContent=editingWire?'Update wire':'Connect';
    $('pipelineConnectConfirm').disabled=!target.value||target.selectedOptions[0]?.disabled;
    $('pipelineDisconnect').hidden=!editingWire||editingWire==='encoder';
    $('pipelineConnectHint').textContent=editingWire==='encoder'?'Encoder source is shared with Streaming. Its input is required while enabled; turn off Monitoring encoder in its Settings to stop it.':'Changes stay pending until Apply outputs. Disconnect keeps the node and switches its output off. Escape cancels.';
  }
  function nodeTitle(id){return cards.get(id)?.dataset.nodeTitle||'Node';}
  function updateNodeTools(){
    if(!cards.has(toolNode))toolNode=cards.keys().next().value;
    $('pipelineNodeSelect').value=toolNode;const expanded=cards.get(toolNode)?.classList.contains('expanded');
    $('pipelineNodeSettings').textContent=expanded?'Collapse settings':'Settings';$('pipelineNodeSettings').setAttribute('aria-label',(expanded?'Collapse ':'Expand ')+nodeTitle(toolNode)+' settings from node controls');
    $('pipelineNodeMove').setAttribute('aria-label','Move '+nodeTitle(toolNode)+' without dragging');$('pipelineMoveLabel').textContent='Move '+nodeTitle(toolNode);
    const position=workspace?.position(toolNode);if(position)for(const [id,value] of [['pipelinePositionX',position.x],['pipelinePositionY',position.y]])if(document.activeElement!==$(id))$(id).value=Math.round(value);
  }
  function refreshTools(){
    updateOptions($('pipelineNodeSelect'),[...cards.keys()].map(id=>new Option(nodeTitle(id),id)));updateNodeTools();
    const wires=$('pipelineWireSelect'),previous=editingWire||wires.value,options=[new Option('Choose a connection','')];
    draft.outputs.filter(output=>output.product).forEach(output=>options.push(new Option(P.PRODUCTS[output.product].label+' → '+output.name+(output.enabled?'':' · off'),output.id)));
    const source={left_color:'left_rgb',right_color:'right_rgb'}[$('streamSource').value]||$('streamSource').value;if(P.PRODUCTS[source])options.push(new Option(P.PRODUCTS[source].label+' → Monitoring encoder','encoder'));updateOptions(wires,options);
    if([...wires.options].some(option=>option.value===previous))wires.value=previous;$('pipelineWireEdit').disabled=!wires.value;
  }
  function selectNodeTools(id,{move=false}={}){if(!cards.has(id))return;toolNode=id;updateNodeTools();if(move){if(compactTools.matches)showViewer(false);connecting=null;editingWire=null;$('pipelineConnectBar').hidden=true;$('pipelineNodeTools').hidden=true;$('pipelineToolsDisclosure').hidden=true;$('pipelineMoveBar').hidden=false;$('pipelineNodeMove').setAttribute('aria-expanded','true');workspace?.reveal(id);}}
  function buildPorts(){
    root.querySelectorAll('[data-pipeline-port]').forEach(container=>{
      const stage=P.STAGES.find(s=>s.id===container.dataset.pipelinePort),row=el('div','pipeline-port-row'),label=el('label',null,'Output port'),select=el('select');
      select.id='pipeline-port-'+stage.id;label.htmlFor=select.id;stage.ports.forEach(id=>select.append(new Option(P.PRODUCTS[id].label,id)));
      const button=el('button','secondary','Connect');button.type='button';button.dataset.sourceConnect=select.value;button.setAttribute('aria-label','Connect from '+stage.title);button.setAttribute('aria-pressed','false');
      select.addEventListener('change',()=>{button.dataset.sourceConnect=select.value;if(connecting){connecting=select.value;updateConnection();}});
      button.addEventListener('click',()=>beginConnection(select.value,null,null,{focus:true}));
      row.append(label,select,button);container.append(row);
    });
  }
  function renderRoutes(){
    closeNodeEditor({focus:false});
    const focus=document.activeElement?.id;
    const column=$('pipelineOutputs');column.replaceChildren();for(const id of [...cards.keys()])if(id.startsWith('pipeline-out-')||id==='pipeline-monitor-output')cards.delete(id);
    for(const output of draft.outputs){
      const card=el('article','panel pipeline-node pipeline-output');card.id='pipeline-'+output.id;card.dataset.outputId=output.id;
      card.classList.toggle('selected-output',selected===output.id);card.classList.toggle('output-needs-connection',output.enabled&&!output.product);
      const head=el('div','pipeline-output-name'),heading=el('h3',null,output.name),view=el('button','pipeline-node-preview nodrag','Preview output');view.type='button';view.id='pipeline-view-'+output.id;view.setAttribute('aria-label','Preview '+output.name);view.onclick=()=>selectPreview(output.id,{scroll:true});
      head.append(heading,badge(!output.enabled?'Off':output.product?P.PRODUCTS[output.product].format:'Not connected',!output.enabled?'neutral':output.product?'cyan':'amber'));
      const input=el('button','pipeline-output-input',connecting?'Connect here':'Edit connection');input.type='button';input.id='pipeline-input-'+output.id;input.onclick=()=>connecting?connectTo(output.id):editWire(output.id);
      const label=el('label',null,'Connected source'),select=sourceSelect(output.product,'pipeline-source-'+output.id);label.htmlFor=select.id;label.append(select);select.onchange=()=>{change(select.value?P.connect(draft,output.id,select.value):P.disconnect(draft,output.id));announce('Route changed. Apply outputs when ready.');};
      const actions=el('div','pipeline-output-actions'),enabledLabel=el('label','pipeline-enabled'),enabled=el('input');enabled.type='checkbox';enabled.checked=output.enabled;enabled.setAttribute('aria-label','Enable '+output.name);enabledLabel.append(enabled,document.createTextNode('Enabled'));
      enabled.id='pipeline-enabled-'+output.id;enabled.onchange=()=>{const next=P.normalize(draft);next.outputs.find(o=>o.id===output.id).enabled=enabled.checked;change(next);};
      const remove=el('button','secondary pipeline-remove','Remove');remove.type='button';remove.setAttribute('aria-label','Remove '+output.name);remove.onclick=()=>{change(P.removeOutput(draft,output.id));announce(output.name+' removed from the draft. Undo is available.');};
      actions.append(enabledLabel,remove);card.append(head,input,label,actions,view);card.onclick=event=>{if(!event.target.closest('button,input,select,label'))selectPreview(output.id,{scroll:true});};column.append(card);foldNode(card);
    }
    if(draft.preview){
      const card=el('article','panel pipeline-node pipeline-output');card.id='pipeline-monitor-output';card.dataset.outputId='monitor';
      const heading=el('h3',null,'Monitoring output'),button=el('button','pipeline-node-preview nodrag','Preview output');button.type='button';button.onclick=()=>selectPreview('monitor',{scroll:true});
      const detail=el('p','pipeline-hint','Monitoring encoder → compressed preview');
      const actions=el('div','pipeline-output-actions'),view=el('button','secondary','Preview'),remove=el('button','secondary pipeline-remove','Remove');view.type=remove.type='button';view.onclick=button.onclick;remove.onclick=()=>change({...draft,preview:false});remove.setAttribute('aria-label','Remove monitoring output');
      actions.append(view,remove);card.append(heading,detail,actions,button);column.append(card);foldNode(card);
    }
    if(!draft.outputs.length&&!draft.preview)column.append(el('p','pipeline-hint','No output nodes. Add an output to request a product. Cameras remain protected.'));
    $('pipelineCadence').value=draft.cadence;$('pipelineDelivery').value=draft.delivery;$('pipelineMonitorCadence').value=draft.previewCadence;
    const preview=$('pipelinePreviewSelect');preview.replaceChildren();draft.outputs.forEach(o=>preview.append(new Option(o.name+' · '+(o.product?P.PRODUCTS[o.product].label:'not connected'),o.id)));
    if(draft.preview)preview.append(new Option('Monitoring output · encoded','monitor'));
    if(![...preview.options].some(o=>o.value===selected))selected=preview.options[0]?.value||'';
    if(!preview.options.length)preview.append(new Option('No outputs',''));preview.value=selected;preview.disabled=!selected;
    $('pipelineAdd').disabled=draft.outputs.length>=P.MAX_OUTPUTS;$('pipelineAddMonitor').disabled=draft.preview;
    $('pipelineNodeCount').textContent=draft.outputs.length+' / '+P.MAX_OUTPUTS+' raw outputs';
    $('pipelineUndo').disabled=!undo.length;$('pipelineRedo').disabled=!redo.length;
    refreshTools();syncCanvas();if(focus)(document.getElementById(focus)||$('pipelineAdd')).focus({preventScroll:true});updateConnection();refresh();paintPreview();scheduleWires();
  }
  function routeStatus(){
    let error='',result=null;const ctx=H.context(),nativeValid=$('pipelineCadence').validity.valid&&$('pipelineMonitorCadence').validity.valid;
    try{if(!nativeValid)throw new Error('Choose output cadences from 0.1 to 60 Hz.');if(ctx.ready)result=P.compile(draft,ctx.limits);}catch(e){error=e.message;}
    $('pipelineError').textContent=error||streamMessage;$('pipelineError').hidden=!error&&!streamMessage;
    $('pipelineBudget').textContent=result?(result.enabled?(result.bytesPerSecond/1048576).toFixed(2)+' MiB/s raw estimate · '+result.products.length+' unique products':'No raw output demand')+' · encoder traffic admitted separately':!ctx.ready?'Waiting for device capabilities':'Routes need attention · '+error;
    const pending=dirty()||!nativeValid;$('pipelineApply').disabled=!pending||!!error||!ctx.ready;$('pipelineDiscard').disabled=!pending;
    $('pipelineSaveState').textContent=!ctx.ready?'Waiting for device capabilities':error?'Routes need attention':pending?'Unsaved routes':routeMessage||'No pending routes';
    $('pipelineSaveState').className=pending?'chip chip-amber':routeMessage?'chip chip-green':'';
  }
  function syncExposure(){
    const state=H.exposure(),automatic=state.mode==='auto';
    for(const [id,value] of [['pipelineExposureMode',state.mode],['pipelineExposureTime',automatic?state.commandTime??'':state.time],['pipelineExposureGain',automatic?state.commandGain??'':state.gain]]){
      const input=$(id);if(input.value!==String(value))input.value=value;input.disabled=!state.loaded||state.busy||(automatic&&id!=='pipelineExposureMode');
    }
    $('pipelineTimeLabel').textContent=automatic?'Exposure · commanded':'Exposure time';$('pipelineGainLabel').textContent=automatic?'Gain · commanded':'Analogue gain';$('pipelineExposureTime').max=state.max;
    $('pipelineExposureApply').disabled=!state.loaded||state.busy||!state.pending;$('pipelineExposureDiscard').disabled=!state.loaded||state.busy||!state.pending;
    const actions=root.querySelector('.pipeline-node-actions[data-owner-node="pipelineExposureNode"]');if(actions)actions.hidden=!state.pending&&!state.busy;
    $('pipelineExposureState').textContent=state.status;$('pipelineExposureState').className=state.pending?'chip chip-amber':'support-text';
    const error=state.error||state.errors.join(' ');$('pipelineExposureError').textContent=error;$('pipelineExposureError').hidden=!error;
  }
  const nodeForms=[];
  function neededStages(){
    const needed=P.requirements(draft),encoder=H.form('encoding'),source={left_color:'left_rgb',right_color:'right_rgb'}[encoder.fields.streamSource.value]||encoder.fields.streamSource.value;
    if(draft.preview&&selected==='monitor'&&!$('pipelinePreviewBody').hidden&&encoder.fields.streamEncoderEnabled.value&&P.PRODUCTS[source])P.requirements({...draft,outputs:[{id:'out-1',name:'Monitor',enabled:true,product:source}]}).forEach(id=>needed.add(id));
    return needed;
  }
  function settings(nodeId,kind,fields,label='Settings'){
    const details=el('details','pipeline-node-settings'),summary=el('summary',null,label),form=el('form');form.id='pipeline-form-'+nodeId;form.noValidate=true;details.append(summary,form);
    for(const [id,title] of fields){const original=$(id),field=original.cloneNode(true);field.id='pipeline-node-'+id;field.removeAttribute('aria-labelledby');field.removeAttribute('aria-describedby');field.dataset.nodeField=id;
      const wrapper=el('label');wrapper.htmlFor=field.id;if(field.type==='checkbox'){wrapper.className='pipeline-enabled';wrapper.append(field,document.createTextNode(title));}else wrapper.append(document.createTextNode(title),field);
      field.addEventListener(field.type==='checkbox'?'change':'input',()=>{H.editForm(kind,id,field.type==='checkbox'?field.checked:field.value);syncNodeForms();});form.append(wrapper);
    }
    const actions=el('div','button-row pipeline-node-actions'),apply=el('button','primary',kind==='algorithm'?'Apply matcher changes':'Apply Streaming changes'),discard=el('button','secondary','Discard');apply.type='submit';apply.setAttribute('form',form.id);discard.type='button';discard.onclick=()=>{H.discardForm(kind);syncNodeForms();};actions.append(apply,discard);
    const status=el('p','support-text'),error=el('p','field-error');status.setAttribute('role','status');error.setAttribute('role','alert');form.append(el('p','pipeline-hint',kind==='algorithm'?'Applies the shared Matcher draft, including filter settings.':'Applies the shared Streaming draft.'));$(nodeId).append(details,actions,status,error);form.onsubmit=event=>{event.preventDefault();H.applyForm(kind);syncNodeForms();};nodeForms.push({kind,form,status,error,actions,apply,discard});
  }
  function syncNodeForms(){for(const view of nodeForms){const state=H.form(view.kind);view.form.querySelectorAll('[data-node-field]').forEach(input=>{const field=state.fields[input.dataset.nodeField];if(input.type==='checkbox')input.checked=field.value;else if(input.value!==String(field.value))input.value=field.value;input.disabled=!state.loaded||field.disabled;if(field.min)input.min=field.min;if(field.max)input.max=field.max;if(field.step)input.step=field.step;});view.actions.hidden=!state.pending&&!state.busy;view.apply.disabled=view.discard.disabled=!state.loaded||state.busy||!state.pending;view.status.textContent=state.status;view.status.className=state.pending?'chip chip-amber':'support-text';view.error.textContent=state.error||state.errors.join(' ');view.error.hidden=!view.error.textContent;}}
  function refresh(){
    syncExposure();syncNodeForms();const ctx=H.context(),needed=neededStages(),pending=dirty()||(selected==='monitor'&&H.form('encoding').pending);
    $('pipelineCameraState').textContent=ctx.connected?`${ctx.health?.image_width||ctx.health?.depth_width||'—'} × ${ctx.health?.image_height||ctx.health?.depth_height||'—'} · ${ctx.health?.capture_target_hz??'—'} Hz target`:'Waiting for device';
    $('pipelineRates').textContent=ctx.connected?`Capture target ${ctx.health?.capture_target_hz??'—'} Hz · Depth output ${ctx.depthRate==null?'—':ctx.depthRate.toFixed(1)} Hz`:'Device disconnected';
    $('pipelineMatcherState').textContent=ctx.stereo?`${ctx.stereo.algorithm==='bm'?'StereoBM':'SGBM · '+(ctx.stereo.sgbm_mode||'3way').toUpperCase()} · ${ctx.stereo.search_range} px · ${ctx.stereo.block_size} px block`:'Waiting for matcher settings';
    $('pipelineCalibrationState').textContent=!ctx.health?'Waiting for calibration state':Number(ctx.health.calibration_authority)>0?'Validated calibration':'Provisional / awaiting validation';
    const encoder=H.form('encoding'),source=encoder.fields.streamSource.value,monitoring=encoder.fields.streamEncoderEnabled.value;
    $('pipelineEncoderState').textContent=encoder.loaded?(monitoring?'Enabled':'Off')+' · '+String(encoder.fields.streamCodec.value).toUpperCase()+' · '+String(source).replaceAll('_',' '):'Waiting for stream settings';
    const exposure=H.exposure();for(const [id,text] of Object.entries({exposure:exposure.loaded?`${exposure.mode==='auto'?'Auto':'Manual'} · ${Number(exposure.time).toLocaleString()} µs · ${exposure.gain}×`:'Loading settings',cameras:ctx.connected?`${ctx.health?.image_width||ctx.health?.depth_width||'—'} × ${ctx.health?.image_height||ctx.health?.depth_height||'—'} · stereo`:'Waiting for device',rectification:ctx.health?Number(ctx.health.calibration_authority)>0?'Calibration validated':'Provisional calibration':'Waiting for calibration',matcher:ctx.stereo?`${ctx.stereo.algorithm.toUpperCase()} · ${ctx.stereo.search_range} px`:'Loading settings',filter:'Disparity + confidence',depth:'Float32 · metres',encoder:encoder.loaded?(monitoring?'Monitoring enabled':'Monitoring off'):'Loading settings'})){
      const summary=$(nodeIds[id])?.querySelector('.pipeline-node-summary');if(summary)summary.textContent=text;
    }
    root.querySelectorAll('[data-pipeline-demand]').forEach(node=>{const active=needed.has(node.dataset.pipelineDemand);node.textContent=active?(pending?'Needed after apply':'Requested here'):'Not requested here';node.className='chip '+(active?'chip-blue':'chip-neutral');});
    routeStatus();refreshTools();updateConnection();scheduleWires();
  }
  function emptyPreview(message,tone='neutral',state='Waiting'){$('pipelinePreviewCanvas').hidden=true;$('pipelinePreviewEmpty').hidden=false;$('pipelinePreviewEmpty').textContent=message;$('pipelinePreviewState').textContent=state;$('pipelinePreviewState').className='chip chip-'+tone;const node=draft.outputs.find(o=>o.id===selected);$('pipelinePreviewCaption').textContent=selected==='monitor'?'Monitoring output':node?.product?P.PRODUCTS[node.product].label:'Select an output';$('pipelinePreviewMeta').textContent='';$('pipelinePreviewCalibration').textContent='';}
  function paintPreview(){
    if($('pipelinePreviewBody').hidden)return emptyPreview('Viewer paused. Expand it to resume; applied raw output routes remain active.','neutral','Paused');
    if(selected==='monitor'){
      if(!draft.preview||!applied.preview)return emptyPreview('Apply the monitoring output to preview it.','amber','Pending');
      if(!H.context().encoding||H.context().encoding.mode==='raw')return emptyPreview('Enable the monitoring encoder in its Settings and apply Streaming changes.','neutral','Encoder off');
      if(!lastSeen)emptyPreview('Waiting for encoded output.');return;
    }
    const node=draft.outputs.find(o=>o.id===selected),saved=applied.outputs.find(o=>o.id===selected);
    if(!node||!node.enabled||!node.product)return emptyPreview('Select an enabled, connected output.');
    if(!saved||!saved.enabled||saved.product!==node.product)return emptyPreview('Apply this route to preview its output.','amber','Pending');
    const descriptor=lastFrame?.metadata.products.find(p=>p.kind===node.product);
    if(!descriptor)return emptyPreview(streamMessage||'Waiting for '+P.PRODUCTS[node.product].label+'.');
    if(performance.now()-lastPaint<200)return;lastPaint=performance.now();
    const canvas=$('pipelinePreviewCanvas');
    if(descriptor.kind==='validity'){
      canvas.width=descriptor.width;canvas.height=descriptor.height;canvas.style.aspectRatio=`${descriptor.width} / ${descriptor.height}`;
      const context=canvas.getContext('2d'),image=context.createImageData(canvas.width,canvas.height),data=new Uint8Array(lastFrame.buffer,lastFrame.payloadOffset+descriptor.offset,descriptor.length);
      data.forEach((value,index)=>{const j=index*4;image.data[j]=value===1?90:30;image.data[j+1]=value===1?210:30;image.data[j+2]=value===1?150:30;image.data[j+3]=255;});context.putImageData(image,0,0);
    }else H.draw(canvas,lastFrame,descriptor);
    canvas.hidden=false;$('pipelinePreviewEmpty').hidden=true;$('pipelinePreviewState').textContent='Live';$('pipelinePreviewState').className='chip chip-green';
    $('pipelinePreviewCaption').textContent=P.PRODUCTS[node.product].label+' · '+P.PRODUCTS[node.product].format;
    $('pipelinePreviewMeta').textContent=`${descriptor.width} × ${descriptor.height} · frame ${lastFrame.metadata.capture_sequence??lastFrame.sequence}`;
    $('pipelinePreviewCalibration').textContent=node.product==='validity'?'Green: valid · dark: invalid':node.product==='depth'?(Number(lastFrame.metadata.calibration_authority)>0?'Metric depth · metres':'Provisional calibration · metric accuracy unverified'):node.product==='disparity'?'Colour-mapped disparity · pixels':'';
  }
  function acceptFrame(frame){lastFrame=frame;if(selected!=='monitor'){lastSeen=Date.now();streamMessage='';paintPreview();}}
  function acceptEncoded(source,width,height){if(selected!=='monitor'||!applied.preview||performance.now()-lastPaint<200)return;lastPaint=performance.now();lastSeen=Date.now();const canvas=$('pipelinePreviewCanvas');canvas.width=width;canvas.height=height;canvas.style.aspectRatio=`${width} / ${height}`;canvas.getContext('2d').drawImage(source,0,0);canvas.hidden=false;$('pipelinePreviewEmpty').hidden=true;$('pipelinePreviewState').textContent='Live';$('pipelinePreviewState').className='chip chip-green';$('pipelinePreviewCaption').textContent='Monitoring encoder · compressed preview';$('pipelinePreviewMeta').textContent=width+' × '+height;$('pipelinePreviewCalibration').textContent='';}
  function resetPreview(){lastFrame=null;lastPaint=0;lastSeen=0;streamMessage='';emptyPreview('Waiting for applied output.');$('pipelinePreviewMeta').textContent='';$('pipelinePreviewCalibration').textContent='';}
  function scheduleWires(){if(frameRequest)return;frameRequest=requestAnimationFrame(()=>{frameRequest=0;drawWires();});}
  function drawWires(){syncCanvas();}
  function defaultPosition(id){const key=Object.keys(nodeIds).find(key=>nodeIds[key]===id),positions={exposure:{x:40,y:30},cameras:{x:40,y:270},rectification:{x:360,y:270},matcher:{x:680,y:270},filter:{x:1000,y:270},depth:{x:1320,y:270},encoder:{x:680,y:570}};if(key)return positions[key];if(id==='pipeline-monitor-output')return{x:1000,y:570};return{x:1640,y:60+Math.max(0,draft.outputs.findIndex(output=>'pipeline-'+output.id===id))*250};}
  function foldNode(card){
    const id=card.id,head=card.querySelector(':scope > .pipeline-node-head,:scope > .pipeline-output-name')||el('div','pipeline-node-head');
    if(!head.parentElement){const title=card.querySelector('h3');if(title)head.append(title);card.prepend(head);}head.classList.add('pipeline-node-head');
    const heading=head.querySelector('h2,h3'),title=heading?.textContent||'Output';card.dataset.nodeTitle=title;
    const body=el('div','pipeline-node-body nodrag nowheel'),summary=el('p','pipeline-node-summary');body.id=id+'-body';
    const actions=[...card.children].filter(child=>child.classList.contains('pipeline-node-actions')||child.classList.contains('field-error')||child.classList.contains('pipeline-node-preview'));
    [...card.children].filter(child=>child!==head&&!actions.includes(child)).forEach(child=>body.append(child));
    head.querySelectorAll('.chip').forEach(chip=>body.prepend(chip));
    const grip=el('button','pipeline-node-grip'),name=el('span','pipeline-node-name',title);grip.type='button';grip.id='pipeline-move-'+id;grip.setAttribute('aria-label','Move '+title);grip.title='Drag heading to move, or tap for movement controls'+(card.dataset.coreNode?'. Required stage; cannot be deleted.':'');
    const stageIcon=heading.querySelector('svg');if(stageIcon)grip.append(stageIcon);grip.append(name);heading.replaceChildren(grip);
    grip.onclick=()=>selectNodeTools(id,{move:true});grip.onkeydown=event=>{const delta={ArrowLeft:[-5,0],ArrowRight:[5,0],ArrowUp:[0,-5],ArrowDown:[0,5]}[event.key];if(delta){event.preventDefault();event.stopPropagation();const p=workspace.position(id);workspace.move(id,p.x+delta[0],p.y+delta[1]);}};
    body.querySelectorAll('.pipeline-node-settings').forEach(details=>details.open=true);
    if(id==='pipelineExposureNode'){const action=body.querySelector('.button-row');action.classList.add('pipeline-node-actions');body.querySelector('#pipelineExposureApply').setAttribute('form','pipelineExposureForm');actions.push(action);}
    const toggle=el('button','pipeline-node-toggle nodrag'),chevron=el('span','pipeline-node-chevron','›');chevron.setAttribute('aria-hidden','true');toggle.type='button';toggle.id='pipeline-settings-'+id;toggle.append(chevron);toggle.setAttribute('aria-controls',body.id);head.append(toggle);
    actions.forEach(action=>action.dataset.ownerNode=id);
    const setOpen=open=>{if(compactTools.matches){if(open){showViewer(false);openNodeEditor(card,body,actions,toggle);expandedNodes.add(id);card.classList.add('expanded');toggle.setAttribute('aria-expanded','true');toggle.setAttribute('aria-label','Collapse '+title+' settings');updateNodeTools();}else{if(nodeEditor?.card===card)closeNodeEditor();body.hidden=true;card.classList.remove('expanded');toggle.setAttribute('aria-expanded','false');toggle.setAttribute('aria-label','Expand '+title+' settings');}return;}body.hidden=!open;card.classList.toggle('expanded',open);toggle.setAttribute('aria-expanded',String(open));toggle.setAttribute('aria-label',(open?'Collapse ':'Expand ')+title+' settings');if(open){expandedNodes.add(id);requestAnimationFrame(()=>requestAnimationFrame(()=>workspace?.reveal(id)));}else expandedNodes.delete(id);if(cards.has(toolNode))updateNodeTools();scheduleWires();};toggle.onclick=()=>{selectNodeTools(id);setOpen(body.hidden);};card.append(summary,body,...actions);cards.set(id,card);setOpen(expandedNodes.has(id));
    const output=draft.outputs.find(output=>'pipeline-'+output.id===id);if(output)summary.textContent=(output.enabled?'':'Off · ')+(output.product?P.PRODUCTS[output.product].label:'Not connected');else if(id==='pipeline-monitor-output')summary.textContent='Compressed preview';
  }
  function sourceProduct(id){const stage=Object.entries(nodeIds).find(([,node])=>node===id)?.[0];return root.querySelector(`[data-pipeline-port="${stage}"] select`)?.value;}
  function connectionProduct(connection,edge){return edge?.data?.product&&edge.data.handleType!=='source'&&connection.source===edge.source?edge.data.product:sourceProduct(connection.source);}
  function validConnection(connection,edge){
    if(gestureCancelled)return false;const product=connectionProduct(connection,edge);if(!product||connection.sourceHandle!=='products'||connection.targetHandle!=='input')return false;
    if(edge?.data?.kind==='encoder'&&connection.target!==nodeIds.encoder)return false;
    if(connection.target===nodeIds.encoder)return edge?.data?.kind!=='output'&&encoderAccepts(product);
    const output=draft.outputs.find(output=>'pipeline-'+output.id===connection.target);if(!output)return false;
    return !(edge?.data?.kind==='output'&&output.id!==edge.data.key&&output.product);
  }
  function reconnectWire(edge,connection){
    if(!edge.data?.editable||!validConnection(connection,edge)){announce('Connection cancelled. Existing wire kept.');return;}
    const product=connectionProduct(connection,edge);
    try{if(edge.data.kind==='encoder'){H.editForm('encoding','streamSource',product);finishConnection('Encoder source changed. Apply Streaming changes to save.');}
      else{change(P.reconnect(draft,edge.data.key,connection.target.replace('pipeline-',''),product));finishConnection('Wire moved. Apply outputs to update this browser.');}
    }catch(error){announce(error.message);}
  }
  function portAction(type,id){
    selectNodeTools(id);
    if(type==='source'){beginConnection(sourceProduct(id));return;}
    const target=id===nodeIds.encoder?'encoder':id.replace('pipeline-','');
    if(connecting)connectTo(target);else editWire(target);
  }
  function syncCanvas(){if(!workspace)return;const needed=neededStages(),edges=[];
    const edge=(source,target,sourceHandle,targetHandle,className,data={})=>edges.push({id:data.key?'wire-'+data.key:source+'-'+target,source,target,sourceHandle,targetHandle,className,data,selected:!!data.key&&editingWire===data.key});
    edge(nodeIds.exposure,nodeIds.cameras,'fixed-out','fixed-in','pipeline-wire-control');
    for(const [from,to] of [['cameras','rectification'],['rectification','matcher'],['matcher','filter'],['filter','depth']])edge(nodeIds[from],nodeIds[to],'fixed-out','fixed-in',needed.has(to)?'pipeline-wire-active':'pipeline-wire-core');
    for(const output of draft.outputs.filter(output=>output.product))edge(nodeIds[P.PRODUCTS[output.product].stage],'pipeline-'+output.id,'products','input',(!output.enabled?'pipeline-wire-disabled':P.PRODUCTS[output.product].stage==='depth'?'pipeline-wire-depth':'pipeline-wire-active')+(selected===output.id?' pipeline-wire-selected':'')+(dirty()?' pipeline-wire-pending':''),{editable:true,key:output.id,kind:'output',product:output.product,label:P.PRODUCTS[output.product].label+' to '+output.name+'. Editable wire; press Enter for connection controls.'});
    const encoder=H.form('encoding'),source={left_color:'left_rgb',right_color:'right_rgb'}[encoder.fields.streamSource.value]||encoder.fields.streamSource.value,stage=P.PRODUCTS[source]?.stage;if(stage)edge(nodeIds[stage],nodeIds.encoder,'products','input',selected==='monitor'&&draft.preview&&!$('pipelinePreviewBody').hidden?'pipeline-wire-active':'pipeline-wire-core',{editable:true,key:'encoder',kind:'encoder',product:source,label:P.PRODUCTS[source].label+' to Monitoring encoder. Source can change; encoder input is required.'});if(draft.preview)edge(nodeIds.encoder,'pipeline-monitor-output','fixed-out','fixed-in',selected==='monitor'&&!$('pipelinePreviewBody').hidden?'pipeline-wire-active':'pipeline-wire-core');
    const activeProduct=draggingProduct||connecting;
    workspace.sync([...cards.values()].map(card=>({id:card.id,card,title:nodeTitle(card.id),position:defaultPosition(card.id),input:card.id===nodeIds.exposure?false:card.dataset.outputId&&card.dataset.outputId!=='monitor'||card.id===nodeIds.encoder?'public':'fixed',fixedOut:[nodeIds.exposure,nodeIds.cameras,nodeIds.rectification,nodeIds.matcher,nodeIds.filter,nodeIds.encoder].includes(card.id),publicOut:!!sourceProduct(card.id),productLabel:P.PRODUCTS[sourceProduct(card.id)]?.label,sourceActive:!!activeProduct&&nodeIds[P.PRODUCTS[activeProduct]?.stage]===card.id,targetReady:!!activeProduct&&(card.id===nodeIds.encoder?encoderAccepts(activeProduct):!!card.dataset.outputId&&card.dataset.outputId!=='monitor'),port:portAction})),edges);
  }
  function showViewer(open){const changed=$('pipelinePreviewBody').hidden===open;if(open&&compactTools.matches)cards.forEach(card=>{const toggle=card.querySelector('.pipeline-node-toggle');if(toggle?.getAttribute('aria-expanded')==='true')toggle.click();});$('pipelinePreviewBody').hidden=!open;$('pipelinePreviewToggle').setAttribute('aria-expanded',String(open));$('pipelinePreviewToggle').setAttribute('aria-label',open?'Minimize output viewer':'Expand output viewer');$('pipelinePreviewToggle').textContent=open?'−':'+';if(changed)H.restartPreview();if(!open)paintPreview();if(open&&matchMedia('(max-width:760px)').matches)requestAnimationFrame(()=>$('pipelinePreview').scrollIntoView({block:'center',behavior:'auto'}));}
  function adoptOutputs(settings){
    if(sameSettings(draft,settings))applied=P.normalize(draft);else draft=applied=P.fromSettings(settings);
    undo=[];redo=[];routeMessage='Outputs applied to this browser';announce(routeMessage);persist();renderRoutes();
  }
  window.NuvoPipelineUI=Object.freeze({refresh,acceptFrame,acceptEncoded,resetPreview,adoptOutputs,wantsEncoded:()=>!$('pipelinePreviewBody').hidden&&selected==='monitor'&&applied.preview,
    streamError(message,channel='raw'){if((selected==='monitor')!==(channel==='encoded'))return;streamMessage=message;emptyPreview(message,'amber','Unavailable');routeStatus();}});
  buildPorts();
  settings('pipelineMatcherNode','algorithm',[['algorithmType','Matcher'],['sgbmMode','Path aggregation'],['searchRange','Search range (px)'],['blockSize','Block size (px)'],['minDisparity','Minimum disparity (px)'],['maxDisparity','Maximum accepted disparity (px)']]);
  settings('pipelineFilterNode','algorithm',[['uniquenessRatio','Uniqueness (%)'],['speckleWindow','Speckle window (pixels)'],['speckleRange','Speckle range (px)'],['medianFilterSize','Median filter']]);
  settings('pipelineEncoderNode','encoding',[['streamEncoderEnabled','Monitoring encoder'],['streamRawEnabled','Raw product delivery'],['streamCodec','Codec'],['streamSource','Image source'],['streamFps','Preview ceiling (Hz)']]);
  Object.values(nodeIds).forEach(id=>foldNode($(id)));
  workspace=window.NuvoPipelineCanvas.mount($('pipelineCanvas'),{defaultPosition,valid:validConnection,activeNode:()=>toolNode,
    connect:connection=>{if(!validConnection(connection))return;const product=sourceProduct(connection.source);beginConnection(product,connection.target===nodeIds.encoder?'encoder':connection.target.replace('pipeline-',''));connectTo(connection.target===nodeIds.encoder?'encoder':connection.target.replace('pipeline-',''));},
    reconnect:reconnectWire,select:selectNodeTools,editWire,positionChanged:id=>{if(id===toolNode)updateNodeTools();},
    dragStart:(id,type,edge)=>{dragging=true;gestureCancelled=false;draggingProduct=edge?.data?.product||(type==='source'?sourceProduct(id):draft.outputs.find(o=>'pipeline-'+o.id===id)?.product);syncCanvas();},
    dragEnd:valid=>{const cancelled=gestureCancelled;dragging=false;draggingProduct=null;gestureCancelled=false;syncCanvas();if(!valid&&!cancelled)announce('No valid port selected. Existing routes kept.');},
  });
  $('pipelineNodeSelect').onchange=()=>selectNodeTools($('pipelineNodeSelect').value);
  $('pipelineNodeFocus').onclick=()=>{focusWorkspace();workspace.reveal(toolNode);};
  $('pipelineNodeSettings').onclick=()=>{cards.get(toolNode)?.querySelector('.pipeline-node-toggle').click();focusWorkspace();};
  $('pipelineNodeMove').onclick=()=>selectNodeTools(toolNode,{move:true});
  $('pipelineMoveDone').onclick=()=>{$('pipelineMoveBar').hidden=true;$('pipelineNodeTools').hidden=false;$('pipelineToolsDisclosure').hidden=false;$('pipelineNodeMove').setAttribute('aria-expanded','false');$('pipelineNodeMove').focus();};
  for(const [id,x,y] of [['Left',-20,0],['Right',20,0],['Up',0,-20],['Down',0,20]])$('pipelineMove'+id).onclick=()=>{const p=workspace.position(toolNode);workspace.move(toolNode,p.x+x,p.y+y);announce(nodeTitle(toolNode)+' moved '+id.toLowerCase()+'.');};
  $('pipelinePositionApply').onclick=()=>{if(!$('pipelinePositionX').reportValidity()||!$('pipelinePositionY').reportValidity())return;workspace.move(toolNode,Number($('pipelinePositionX').value),Number($('pipelinePositionY').value));announce(nodeTitle(toolNode)+' position updated. Device settings are unchanged.');};
  $('pipelineWireSelect').onchange=()=>{$('pipelineWireEdit').disabled=!$('pipelineWireSelect').value;};
  $('pipelineWireEdit').onclick=()=>{editWire($('pipelineWireSelect').value);$('pipelineConnectSource').focus({preventScroll:true});};
  $('pipelineWireNew').onclick=()=>beginConnection(sourceProduct(toolNode)||'left_mono',null,null,{focus:true});
  $('pipelineDisconnect').onclick=()=>{if(!editingWire||editingWire==='encoder')return;const name=draft.outputs.find(o=>o.id===editingWire)?.name;change(P.disconnect(draft,editingWire));finishConnection(name+' disconnected and switched off. Node preserved. Apply outputs to stop this subscription.');};
  $('pipelineExposureMode').onchange=()=>{H.editExposure({mode:$('pipelineExposureMode').value});syncExposure();};
  $('pipelineExposureTime').oninput=()=>{H.editExposure({time:$('pipelineExposureTime').value});syncExposure();};
  $('pipelineExposureGain').oninput=()=>{H.editExposure({gain:$('pipelineExposureGain').value});syncExposure();};
  $('pipelineExposureForm').onsubmit=event=>{event.preventDefault();H.applyExposure();syncExposure();};$('pipelineExposureDiscard').onclick=()=>{H.discardExposure();syncExposure();};
  root.querySelectorAll('[data-pipeline-settings]').forEach(button=>button.onclick=()=>H.openSettings(button.dataset.pipelineSettings,button.dataset.pipelineGroup));
  $('pipelineAdd').onclick=()=>change(P.addOutput(draft,connecting));$('pipelineAddMonitor').onclick=()=>change({...draft,preview:true});
  $('pipelineUndo').onclick=()=>{if(!undo.length)return;redo.push(draft);change(undo.pop(),{history:false});};$('pipelineRedo').onclick=()=>{if(!redo.length)return;undo.push(draft);change(redo.pop(),{history:false});};
  $('pipelineExample').onclick=()=>change({...draft,outputs:[{id:'out-1',name:'Rectified output',product:'rectified_stereo',enabled:true},{id:'out-2',name:'Depth output',product:'depth',enabled:true}]});
  $('pipelineView').onclick=()=>{showViewer(true);$('pipelinePreviewSelect').focus({preventScroll:true});};
  $('pipelinePreviewToggle').onclick=()=>showViewer($('pipelinePreviewBody').hidden);
  if(matchMedia('(max-width:600px), (max-height:800px)').matches)showViewer(false);
  $('pipelineZoomOut').onclick=()=>workspace.zoom(-1);$('pipelineZoomIn').onclick=()=>workspace.zoom(1);$('pipelineFit').onclick=()=>workspace.fit();$('pipelineArrange').onclick=()=>workspace.arrange();
  $('pipelineCollapse').onclick=()=>{cards.forEach(card=>{const toggle=card.querySelector('.pipeline-node-toggle');if(toggle.getAttribute('aria-expanded')==='true')toggle.click();});};
  $('pipelineConnectSource').onchange=()=>{connecting=$('pipelineConnectSource').value;const stage=P.PRODUCTS[connecting].stage,select=root.querySelector(`[data-pipeline-port="${stage}"] select`);if(select){select.value=connecting;select.parentElement.querySelector('[data-source-connect]').dataset.sourceConnect=connecting;}updateConnection();syncCanvas();};
  $('pipelineConnectConfirm').onclick=()=>connectTo($('pipelineConnectTarget').value);$('pipelineConnectCancel').onclick=()=>finishConnection('Connection cancelled. Existing routes kept.');
  $('pipelinePreviewSelect').onchange=()=>selectPreview($('pipelinePreviewSelect').value);
  for(const [id,key] of [['pipelineCadence','cadence'],['pipelineMonitorCadence','previewCadence'],['pipelineDelivery','delivery']])$(id).addEventListener('input',()=>{if(!$(id).validity.valid){routeStatus();return;}change({...draft,[key]:key==='delivery'?$(id).value:Number($(id).value)});});
  $('pipelineOutputForm').onsubmit=event=>{event.preventDefault();try{P.compile(draft,H.context().limits);const persistent=H.applyOutputs(draft);applied=P.normalize(draft);routeMessage=persistent&&persist()?'Outputs applied to this browser':'Outputs applied for this session';undo=[];redo=[];renderRoutes();}catch(error){$('pipelineError').hidden=false;$('pipelineError').textContent=error.message;announce(error.message);}};
  $('pipelineDiscard').onclick=()=>{undo=[];redo=[];draft=P.normalize(applied);H.editOutputDraft(draft);routeMessage='Routes restored';announce('Route changes discarded.');connecting=null;editingWire=null;renderRoutes();};
  $('outputForm').addEventListener('input',event=>{if(event.detail?.pipeline)return;try{change(P.fromSettings(H.outputDraft()),{project:false});}catch(error){announce(error.message);}});
  const observer=new MutationObserver(()=>{syncExposure();syncNodeForms();});for(const id of ['exposureForm','exposureError','algorithmForm','algorithmError','encodingForm','encodingError'])observer.observe($(id),{subtree:true,childList:true,characterData:true,attributes:true,attributeFilter:['disabled','value','aria-invalid']});
  root.addEventListener('keydown',event=>{if(event.key==='Escape'){if(dragging)gestureCancelled=true;if(connecting||editingWire||dragging)finishConnection('Connection cancelled. Existing routes kept.');if(!$('pipelineMoveBar').hidden){$('pipelineMoveBar').hidden=true;$('pipelineNodeTools').hidden=false;$('pipelineToolsDisclosure').hidden=false;$('pipelineNodeMove').setAttribute('aria-expanded','false');$('pipelineNodeMove').focus();}}},true);
  setInterval(()=>{const cadence=selected==='monitor'?applied.previewCadence:applied.cadence;if(root.classList.contains('active')&&!$('pipelinePreviewCanvas').hidden&&lastSeen&&Date.now()-lastSeen>Math.max(2000,3000/cadence)){$('pipelinePreviewState').textContent='Stale';$('pipelinePreviewState').className='chip chip-amber';}},1000);
  H.editOutputDraft(draft);renderRoutes();
})();
