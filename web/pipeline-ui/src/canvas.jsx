import React,{useState,useLayoutEffect,useRef,useCallback} from 'react';
import {createRoot} from 'react-dom/client';
import {flushSync} from 'react-dom';
import {ReactFlow,ReactFlowProvider,Handle,Position,Background,applyNodeChanges,MarkerType,useNodesInitialized,getNodesBounds,getViewportForBounds} from '@xyflow/react';
import '@xyflow/react/dist/style.css';

const KEY='nuvo-pipeline-canvas-v2';
const validId=id=>/^pipeline(?:ExposureNode|CamerasNode|RectificationNode|MatcherNode|FilterNode|DepthNode|EncoderNode|-out-(?:[1-9]|1[0-4])|-monitor-output)$/.test(id);
const bound=(v,min,max)=>Number.isFinite(v)?Math.max(min,Math.min(max,v)):min;
function readLayout(){try{const text=localStorage.getItem(KEY);if(!text||text.length>16384)return {};const value=JSON.parse(text),positions={};if(value.version!==1)return {};for(const [id,p] of Object.entries(value.positions||{})){if(validId(id)&&Number.isFinite(p.x)&&Number.isFinite(p.y))positions[id]={x:bound(p.x,-8000,8000),y:bound(p.y,-4000,4000)};}const v=value.viewport;return {positions,canvas:value.canvas,viewport:v&&[v.x,v.y,v.zoom].every(Number.isFinite)?{x:bound(v.x,-20000,20000),y:bound(v.y,-20000,20000),zoom:bound(v.zoom,.25,2)}:null};}catch(_){return {};}}
function saveLayout(nodes,viewport){try{const canvas=document.getElementById('pipelineCanvas');localStorage.setItem(KEY,JSON.stringify({version:1,positions:Object.fromEntries(nodes.filter(n=>validId(n.id)).map(n=>[n.id,{x:bound(n.position.x,-8000,8000),y:bound(n.position.y,-4000,4000)}])),viewport,canvas:{width:canvas.clientWidth,height:canvas.clientHeight}}));}catch(_){}}

const Node=React.memo(function Node({data}){
  const mount=useRef(null);
  useLayoutEffect(()=>{mount.current.replaceChildren(data.card);},[data.card]);
  const port=(type)=>({role:'button',tabIndex:0,
    onClick:event=>{event.stopPropagation();data.port(type,data.id);},
    onKeyDown:event=>{if(event.key==='Enter'||event.key===' '){event.stopPropagation();event.preventDefault();data.port(type,data.id);}else if(event.key.startsWith('Arrow'))event.stopPropagation();},
  });
  return <>
    {data.input&&<Handle type="target" position={data.id==='pipelineCamerasNode'?Position.Top:Position.Left} id={data.input==='public'?'input':'fixed-in'} isConnectable={data.input==='public'}
      {...(data.input==='public'?port('target'):{'aria-hidden':true,tabIndex:-1})}
      aria-label={data.input==='public'?'IN: connect or edit '+data.title:undefined}
      title={data.input==='public'?'Connect or edit input: '+data.title:undefined}
      className={data.input==='public'?'product-handle input-handle'+(data.targetReady?' connection-ready':''):'fixed-handle'}>
    </Handle>}
    <div ref={mount}/>
    {data.fixedOut&&<Handle type="source" position={data.id==='pipelineExposureNode'?Position.Bottom:Position.Right} id="fixed-out" isConnectable={false} aria-hidden="true" tabIndex={-1} style={{top:data.publicOut?'34%':undefined}} className="fixed-handle"/>}
    {data.publicOut&&<Handle type="source" position={Position.Right} id="products" style={{top:'calc(100% - 24px)'}}
      {...port('source')} aria-label={'OUT: connect '+data.productLabel+' from '+data.title}
      title={'Connect output: '+data.productLabel+' from '+data.title}
      className={'product-handle output-handle'+(data.sourceActive?' connection-active':'')}/>}
  </>;
});
const nodeTypes={nuvo:Node};
function Canvas({bridge,initial,onReady}){
  const [nodes,setNodes]=useState([]),[edges,setEdges]=useState([]),[theme,setTheme]=useState(document.documentElement.dataset.theme||'dark');
  const instance=useRef(null),latest=useRef(nodes),saved=useRef(initial),reconnecting=useRef(null);latest.current=nodes;
  const initialized=useNodesInitialized(),didInit=useRef(false);
  const duration=matchMedia('(prefers-reduced-motion: reduce)').matches?0:120;
  const fitTop=useCallback((duration=120)=>{if(!instance.current)return;const bounds=getNodesBounds(instance.current.getNodes()),area=bridge.container.getBoundingClientRect(),viewport=getViewportForBounds(bounds,area.width,area.height,.25,1,.13);viewport.y=24-bounds.y*viewport.zoom;instance.current.setViewport(viewport,{duration});},[]);
  const revealNode=useCallback(id=>{const node=instance.current?.getNode(id);if(node){const area=bridge.container.getBoundingClientRect(),width=node.measured?.width||236,height=node.measured?.height||150,left=node.data.input==='public'?40:0,right=node.data.publicOut?40:0,usableHeight=area.height-(area.width<700?Math.min(96,(document.getElementById('pipelinePreview')?.offsetHeight||68)+24):0);instance.current.setViewport({x:area.width*(area.width>700?.35:.5)-(node.position.x+(width+right-left)/2),y:(height>usableHeight?12:(usableHeight-height)/2)-node.position.y,zoom:1},{duration});}},[]);
  useLayoutEffect(()=>{if(initialized&&!didInit.current&&instance.current){didInit.current=true;const area=bridge.container.getBoundingClientRect(),size=saved.current.canvas;if(saved.current.viewport&&Number.isFinite(size?.width)&&Math.abs(size.width-area.width)<64&&Math.abs(size.height-area.height)<100)instance.current.setViewport(saved.current.viewport);else revealNode('pipelineCamerasNode');}},[initialized]);
  useLayoutEffect(()=>{let previousWidth;const observer=new ResizeObserver(()=>{const area=bridge.container.getBoundingClientRect();bridge.container.style.setProperty('--pipeline-canvas-height',(area.height-(area.width<700?96:0))+'px');if(previousWidth&&Math.abs(previousWidth-area.width)>120&&didInit.current)revealNode(bridge.activeNode());previousWidth=area.width;});observer.observe(bridge.container);return()=>observer.disconnect();},[]);
  const persist=useCallback(()=>{if(instance.current)saveLayout(latest.current,instance.current.getViewport());},[]);
  useLayoutEffect(()=>{const observer=new MutationObserver(()=>setTheme(document.documentElement.dataset.theme||'dark'));observer.observe(document.documentElement,{attributes:true,attributeFilter:['data-theme']});return()=>observer.disconnect();},[]);
  useLayoutEffect(()=>{onReady({
    sync(cards,nextEdges){flushSync(()=>{setNodes(previous=>cards.map(card=>{const old=previous.find(node=>node.id===card.id);return {id:card.id,type:'nuvo',position:old?.position||saved.current.positions?.[card.id]||card.position,data:card,ariaLabel:card.title,dragHandle:'.pipeline-node-grip',deletable:false,selected:old?.selected||false};}));setEdges(nextEdges.map(edge=>({...edge,deletable:false,reconnectable:edge.data?.editable?(edge.data.kind==='encoder'?'source':true):false,selectable:!!edge.data?.editable,focusable:!!edge.data?.editable,ariaRole:edge.data?.editable?'button':'img',ariaLabel:edge.data?.label||(edge.data?.editable?'Editable output connection':'Fixed stage connection'),interactionWidth:44,markerEnd:{type:MarkerType.ArrowClosed,width:14,height:14},type:'default'})));});},
    fit(){fitTop(duration);},
    arrange(){saved.current={};flushSync(()=>setNodes(previous=>previous.map(node=>({...node,position:bridge.defaultPosition(node.id)}))));requestAnimationFrame(()=>{fitTop(duration);persist();});},
    zoom(delta){if(delta>0)instance.current?.zoomIn({duration});else instance.current?.zoomOut({duration});},
    reveal:revealNode,
    position(id){const n=instance.current?.getNode(id);return n?{...n.position}:null;},
    move(id,x,y){if(!validId(id))return;flushSync(()=>setNodes(previous=>previous.map(node=>node.id===id?{...node,position:{x:bound(x,-8000,8000),y:bound(y,-4000,4000)}}:node)));requestAnimationFrame(()=>{persist();bridge.positionChanged(id);});},
    persist,
  });},[]);
  return <ReactFlow nodes={nodes} edges={edges} nodeTypes={nodeTypes} colorMode={theme} minZoom={.25} maxZoom={2} nodeExtent={[[-8000,-4000],[8000,4000]]}
    onInit={value=>{instance.current=value;}}
    onNodesChange={changes=>{setNodes(previous=>applyNodeChanges(changes.filter(c=>c.type!=='remove'),previous));if(changes.some(c=>c.type==='position'&&!c.dragging))requestAnimationFrame(persist);}}
    nodeDragThreshold={8} connectionDragThreshold={8} onNodeDragStart={(_event,node)=>bridge.select(node.id)} onNodeDragStop={(_event,node)=>{persist();bridge.positionChanged(node.id);}} onMoveEnd={(_event,viewport)=>{saveLayout(latest.current,viewport);document.getElementById('pipelineZoom').textContent=Math.round(viewport.zoom*100)+'%';}}
    onConnect={bridge.connect} isValidConnection={connection=>bridge.valid(connection,reconnecting.current)} onNodeClick={(_event,node)=>bridge.select(node.id)}
    connectOnClick={false} onConnectStart={(_event,params)=>bridge.dragStart(params.nodeId,params.handleType)} onConnectEnd={(_event,state)=>bridge.dragEnd(!!state?.isValid)}
    onReconnectStart={(_event,edge,handleType)=>{reconnecting.current={...edge,data:{...edge.data,handleType}};bridge.dragStart(edge.source,'source',edge);}}
    onReconnect={(edge,connection)=>{bridge.reconnect(reconnecting.current||edge,connection);}}
    onReconnectEnd={(_event,_edge,_type,state)=>{reconnecting.current=null;bridge.dragEnd(!!state?.isValid);}}
    onEdgeClick={(_event,edge)=>{if(edge.data?.editable)bridge.editWire(edge.data.key);}}
    onEdgesChange={changes=>{const chosen=changes.find(change=>change.type==='select'&&change.selected);if(chosen){const edge=edges.find(edge=>edge.id===chosen.id);if(edge?.data?.editable)bridge.editWire(edge.data.key);}}}
    deleteKeyCode={null} edgesReconnectable reconnectRadius={24} elementsSelectable panOnScroll={false} zoomOnScroll zoomOnPinch zoomOnDoubleClick={false} preventScrolling
    ariaLabelConfig={{'node.a11yDescription.default':'Press Enter to select, then arrow keys to move. Drag the heading to move; use its arrow button to expand settings. Node controls above the canvas offer the same actions without dragging.','edge.a11yDescription.default':'Press Enter to edit this connection using the full-size connection controls. Escape cancels. Fixed wiring cannot be edited.','controls.fitView.ariaLabel':'Fit pipeline to view'}}>
    <Background gap={24} size={1} color="var(--input-line)"/>
  </ReactFlow>;
}
export function mount(container,bridge){let control;const initial=readLayout();flushSync(()=>createRoot(container).render(<ReactFlowProvider><Canvas bridge={{...bridge,container}} initial={initial} onReady={value=>{control=value;}}/></ReactFlowProvider>));return control;}
