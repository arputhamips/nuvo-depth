/* Bounded browser routing model. The device's processing topology is immutable. */
(function(root,factory){
  const model=factory();
  if(typeof module==='object'&&module.exports)module.exports=model;
  else root.NuvoPipeline=model;
})(typeof globalThis!=='undefined'?globalThis:this,function(){
  'use strict';
  const MAX_OUTPUTS=14;
  const STAGES=Object.freeze([
    Object.freeze({id:'cameras',title:'Stereo cameras',ports:Object.freeze(['left_mono','right_mono','left_rgb','right_rgb'])}),
    Object.freeze({id:'rectification',title:'Rectification',ports:Object.freeze(['left_rectified','right_rectified','left_rectified_rgb','right_rectified_rgb','rectified_stereo','rectified_stereo_rgb'])}),
    Object.freeze({id:'matcher',title:'Stereo matcher',ports:Object.freeze([])}),
    Object.freeze({id:'filter',title:'Disparity & validity filters',ports:Object.freeze(['disparity','confidence'])}),
    Object.freeze({id:'depth',title:'Depth conversion',ports:Object.freeze(['depth','validity'])})
  ]);
  const definitions=[
    ['left_mono','Left mono','cameras',1,'Gray8'],['right_mono','Right mono','cameras',1,'Gray8'],
    ['left_rgb','Left RGB','cameras',3,'RGB8'],['right_rgb','Right RGB','cameras',3,'RGB8'],
    ['left_rectified','Left mono · rectified','rectification',1,'Gray8'],['right_rectified','Right mono · rectified','rectification',1,'Gray8'],
    ['left_rectified_rgb','Left RGB · rectified','rectification',3,'RGB8'],['right_rectified_rgb','Right RGB · rectified','rectification',3,'RGB8'],
    ['rectified_stereo','Stereo pair · rectified mono','rectification',2,'Gray8 · side by side'],['rectified_stereo_rgb','Stereo pair · rectified RGB','rectification',6,'RGB8 · side by side'],
    ['disparity','Disparity','filter',4,'Float32 · px'],['confidence','Confidence','filter',1,'Uint8'],
    ['depth','Depth','depth',4,'Float32 · m'],['validity','Validity','depth',1,'Uint8 · codes']
  ];
  const PRODUCTS=Object.freeze(Object.fromEntries(definitions.map(([id,label,stage,bytes,format])=>[id,Object.freeze({id,label,stage,bytes,format})])));
  const own=(object,key)=>Object.prototype.hasOwnProperty.call(object,key);
  function object(value,keys){return value&&typeof value==='object'&&!Array.isArray(value)&&Object.keys(value).every(key=>keys.includes(key));}
  function normalize(value){
    if(!object(value,['version','outputs','cadence','delivery','preview','previewCadence'])||value.version!==1)throw new Error('Unsupported pipeline layout. Core nodes and connections cannot be edited.');
    if(!Number.isFinite(value.cadence)||value.cadence<=0||value.cadence>60)throw new Error('Output cadence must be greater than 0 and at most 60 Hz.');
    if(!['auto','best_effort','exact'].includes(value.delivery))throw new Error('Choose a supported delivery policy.');
    if(typeof value.preview!=='boolean'||!Number.isFinite(value.previewCadence)||value.previewCadence<=0||value.previewCadence>60)throw new Error('Use a valid monitoring state and cadence from 0–60 Hz, greater than zero.');
    if(!Array.isArray(value.outputs)||value.outputs.length>MAX_OUTPUTS)throw new Error(`Use at most ${MAX_OUTPUTS} output nodes.`);
    const ids=new Set();
    const outputs=value.outputs.map(output=>{
      if(!object(output,['id','name','product','enabled'])||!/^out-(?:[1-9]|1[0-4])$/.test(output.id)||ids.has(output.id))throw new Error('Output IDs must be unique. Core nodes are protected.');
      if(typeof output.name!=='string'||!output.name.trim()||output.name.length>40||/[\x00-\x1f]/.test(output.name))throw new Error('Output names must contain 1–40 printable characters.');
      if(output.product!==null&&(typeof output.product!=='string'||!own(PRODUCTS,output.product)))throw new Error('This source is not a supported camera product.');
      if(typeof output.enabled!=='boolean')throw new Error('Output enabled state must be true or false.');
      ids.add(output.id);return {id:output.id,name:output.name.trim(),product:output.product,enabled:output.enabled};
    });
    return {version:1,cadence:value.cadence,delivery:value.delivery,outputs,preview:value.preview,previewCadence:value.previewCadence};
  }
  function fromSettings(settings){
    const products=[...new Set(settings.products)];
    return normalize({version:1,cadence:settings.cadence_hz,delivery:settings.delivery,outputs:products.map((product,index)=>({id:'out-'+(index+1),name:'Output '+(index+1),product,enabled:!!settings.enabled})),preview:settings.preview!==false,previewCadence:settings.preview_cadence_hz||5});
  }
  function addOutput(graph,product=null){
    const next=normalize(graph);
    if(next.outputs.length>=MAX_OUTPUTS)throw new Error(`The pipeline is limited to ${MAX_OUTPUTS} output nodes.`);
    const index=Array.from({length:MAX_OUTPUTS},(_,i)=>i+1).find(i=>!next.outputs.some(o=>o.id==='out-'+i));
    next.outputs.push({id:'out-'+index,name:'Output '+index,product,enabled:true});return normalize(next);
  }
  function connect(graph,target,product){
    const next=normalize(graph),output=next.outputs.find(o=>o.id===target);
    if(!output)throw new Error('Only output nodes accept new connections. Core wiring is protected.');
    output.product=product;return normalize(next);
  }
  function disconnect(graph,target){
    const next=connect(graph,target,null);
    next.outputs.find(o=>o.id===target).enabled=false;
    return next;
  }
  function reconnect(graph,previous,target,product){
    const next=normalize(graph);
    if(!next.outputs.some(o=>o.id===previous)||!next.outputs.some(o=>o.id===target))throw new Error('Only output wires can be moved. Core wiring is protected.');
    if(!own(PRODUCTS,product))throw new Error('This source is not a supported camera product.');
    const old=next.outputs.find(o=>o.id===previous),destination=next.outputs.find(o=>o.id===target);
    if(!old.product)throw new Error('This wire no longer exists. Choose a current connection.');
    if(previous!==target){
      if(destination.product)throw new Error('The destination already has a wire. Disconnect it first, or change its source.');
      destination.enabled=old.enabled;old.product=null;old.enabled=false;
    }
    destination.product=product;return next;
  }
  function removeOutput(graph,id){
    const next=normalize(graph);
    if(!next.outputs.some(o=>o.id===id))throw new Error('Required processing nodes cannot be removed.');
    next.outputs=next.outputs.filter(o=>o.id!==id);return next;
  }
  function products(graph){return [...new Set(normalize(graph).outputs.filter(o=>o.enabled&&o.product).map(o=>o.product))];}
  function requirements(graph){
    const needed=new Set();
    for(const product of products(graph)){
      const stage=PRODUCTS[product].stage;needed.add('cameras');
      if(stage!=='cameras')needed.add('rectification');
      if(stage==='filter'||stage==='depth'){needed.add('matcher');needed.add('filter');}
      if(stage==='depth')needed.add('depth');
    }
    return needed;
  }
  function compile(graph,limits={}){
    const next=normalize(graph);
    const dangling=next.outputs.find(o=>o.enabled&&!o.product);
    if(dangling)throw new Error(`Connect ${dangling.name}, switch it off, or remove it before applying.`);
    const selected=products(next),enabled=selected.length>0;
    const monitor={preview:next.preview,preview_cadence_hz:next.previewCadence};
    if(!enabled)return {enabled:false,products:[],cadence_hz:next.cadence,delivery:next.delivery,frameBytes:0,bytesPerSecond:0,...monitor};
    if(Array.isArray(limits.supportedProducts)&&selected.some(p=>!limits.supportedProducts.includes(p)))throw new Error('A selected product is not supported by this device.');
    if(next.delivery==='exact'&&!limits.qualified)throw new Error('Exact delivery requires a measured, qualified connection. Choose Auto or Best effort.');
    if(limits.rawEnabled===false)throw new Error('Raw delivery is off. Enable it on Streaming before applying these outputs.');
    const width=limits.width??728,height=limits.height??544;
    if(!Number.isSafeInteger(width)||!Number.isSafeInteger(height)||width<=0||height<=0||width>8192||height>8192)throw new Error('Device dimensions are unavailable or outside supported bounds.');
    const frameBytes=65568+selected.reduce((sum,p)=>sum+width*height*PRODUCTS[p].bytes,0);
    const maxFrame=Number.isFinite(limits.maxFrameBytes)&&limits.maxFrameBytes>0?limits.maxFrameBytes:16777216;
    const budget=Number.isFinite(limits.bytesPerSecond)&&limits.bytesPerSecond>0?limits.bytesPerSecond:8*1048576;
    if(frameBytes>maxFrame)throw new Error('Selected products exceed the raw frame size limit. Choose fewer products.');
    const bytesPerSecond=frameBytes*next.cadence;
    if(bytesPerSecond>budget){const safe=Math.floor(budget/frameBytes*10)/10;throw new Error(`Outputs exceed the connection resource budget. Use ${safe>0?safe+' Hz or less':'fewer products'}, or choose fewer products.`);}
    return {enabled,products:selected,cadence_hz:next.cadence,delivery:next.delivery,frameBytes,bytesPerSecond,...monitor};
  }
  return Object.freeze({MAX_OUTPUTS,STAGES,PRODUCTS,normalize,fromSettings,addOutput,connect,disconnect,reconnect,removeOutput,products,requirements,compile,signature:graph=>JSON.stringify(normalize(graph))});
});
