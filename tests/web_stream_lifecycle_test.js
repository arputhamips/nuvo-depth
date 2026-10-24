const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const source=fs.readFileSync(require('node:path').join(__dirname,'../web/app.js'),'utf8');
const tick=()=>new Promise(resolve=>setImmediate(resolve));
(async()=>{
  const elements=new Map(),shown=[],bitmaps=[],decoders=[];
  class FakeDecoder{
    constructor(callbacks){this.callbacks=callbacks;this.state='unconfigured';this.decodeQueueSize=0;this.decoded=[];decoders.push(this);}
    configure(){this.state='configured';}
    decode(value){this.decoded.push(value);}
    close(){this.state='closed';}
  }
  const context={window:{VideoDecoder:FakeDecoder},VideoDecoder:FakeDecoder,EncodedVideoChunk:class{constructor(data){Object.assign(this,data)}},
    decoder:null,decoderCodec:'',encodedLastSequence:0,encodedWaitingForKeyframe:true,encodedGeneration:1,encodedLastPresented:0,
    jpegDecodeBusy:false,pendingJpeg:null,encodedMetadata:new Map(),encodedFrameAt:0,encodingPreviewPending:false,encodingFormDirty:false,
    encodedFrameMatchesConfiguration:()=>true,Date,Blob,Math,selectedPage:'network',outputSettings:{preview:true},pictureQualityDirty:false,
    setSampleCanvasLive:()=>{},recordReceiving:()=>{},
    $:id=>{if(!elements.has(id))elements.set(id,{textContent:''});return elements.get(id)},
    createImageBitmap:()=>new Promise(resolve=>bitmaps.push(resolve))};
  vm.createContext(context);
  vm.runInContext(source.slice(source.indexOf('function parseEncoded('),source.indexOf('async function pollEncoded(')),context);
  const present=context.drawEncodedSource;
  context.drawEncodedSource=(bitmap,...details)=>{shown.push(bitmap.id);present(bitmap,...details);};
  const video=(id,timestamp)=>({id,timestamp,displayWidth:2,displayHeight:1,close(){}});
  const frame=(sequence,codec='h264',keyframe=true)=>({streamSequence:sequence,payload:new Uint8Array([sequence]),metadata:{codec,codec_string:'avc1.42c01f',width:2,height:1,keyframe,source:'left_mono',capture_time_ns:sequence*1000000,capture_sequence:sequence,encode_ms:1}});
  await context.queueEncoded(frame(1),1,1);await context.queueEncoded(frame(5),1,2);
  assert.equal(decoders.length,1,'normal latest-frame gaps must reuse the configured decoder');
  assert.equal(decoders[0].decoded.length,2);
  assert.equal(elements.has('encodedSequence'),false,'submitting H264 cannot relabel the displayed picture');
  context.encodingPreviewPending=true;
  decoders[0].callbacks.output(video(5,5000));
  assert.equal(elements.get('encodedSequence').textContent,'Frame 5');
  assert.equal(context.encodingPreviewPending,false,'only a presented picture confirms the saved preview');
  decoders[0].callbacks.output(video(1,1000));
  assert.deepEqual(shown,[5],'late decoder output cannot move the viewer back to an older capture');
  assert.equal(elements.get('encodedSequence').textContent,'Frame 5','captions follow the actual displayed frame');
  decoders[0].decodeQueueSize=2;await context.queueEncoded(frame(6),1,3);
  assert.equal(decoders[0].decoded.length,2,'slow H264 decoding must bound queued work');
  decoders[0].decodeQueueSize=0;
  await context.queueEncoded(frame(10,'h264',false),1,4);
  assert.equal(decoders.length,2,'a dependent frame after a gap must resynchronize');
  assert.equal(decoders[1].decoded.length,0,'do not decode a delta without its reference');
  await context.queueEncoded(frame(11),1,5);assert.equal(decoders[1].decoded.length,1);
  decoders[0].callbacks.output(video(99,11000));
  assert.deepEqual(shown,[5],'replaced decoders cannot paint into the new stream or consume its metadata');
  decoders[1].callbacks.output(video(11,11000));
  assert.deepEqual(shown,[5,11]);
  assert.equal(elements.get('encodedSequence').textContent,'Frame 11');
  shown.length=0;
  const first=context.queueEncoded(frame(12,'jpeg'),1,6);
  await context.queueEncoded(frame(13,'jpeg'),1,7);await context.queueEncoded(frame(14,'jpeg'),1,8);
  assert.equal(bitmaps.length,1,'only one JPEG decode may run at once');
  bitmaps[0]({id:12,width:2,height:1,close(){}});await tick();
  assert.equal(bitmaps.length,2,'only the latest pending JPEG survives');
  bitmaps[1]({id:14,width:2,height:1,close(){}});await first;
  assert.deepEqual(shown,[12,14]);
  const stale=context.queueEncoded(frame(15,'jpeg'),1,9);context.encodedGeneration=2;
  bitmaps[2]({id:15,width:2,height:1,close(){}});await stale;
  assert.deepEqual(shown,[12,14],'old stream generations must not paint after restart');

  const sockets=[],timers=[];
  const transport={rawSocket:null,rawPolling:false,rawGeneration:1,rawPollToken:0,rawRetryTimer:null,rawRetryDelay:1000,
    encodedSocket:null,encodedPolling:false,encodedGeneration:1,encodedPollToken:0,encodedRetryTimer:null,encodedRetryDelay:1000,encodedArrival:0,
    rawPreviewPage:()=>true,encodedPreviewPage:()=>true,encoding:{mode:'both'},outputSettings:{preview_cadence_hz:5},
    outputSubscriptionPath:path=>path,socketUrl:path=>path,Math,$:()=>({}),
    setTimeout:(callback,delay)=>{timers.push({callback,delay});return timers.length},
    WebSocket:class{constructor(){sockets.push(this)}},parseFrame:x=>x,renderFrame:()=>{},parseEncoded:x=>x,queueEncoded:async()=>{}};
  vm.createContext(transport);
  vm.runInContext(source.slice(source.indexOf('function startRawViewer('),source.indexOf('function parseEncoded(')),transport);
  vm.runInContext(source.slice(source.indexOf('function startEncodedViewer('),source.indexOf('function restartStreams(')),transport);
  for(const [start,poll,token,active] of [['startRawViewer','pollFrame','rawPollToken','rawPolling'],['startEncodedViewer','pollEncoded','encodedPollToken','encodedPolling']]){
    const calls=[];transport[poll]=(...args)=>calls.push(args);
    transport[start]();const socket=sockets.at(-1);socket.onopen();socket.onclose();
    assert.equal(calls.length,1);assert.equal(transport[active],true);
    const before=transport[token];assert.equal(timers.at(-1).delay,1000);timers.at(-1).callback();
    const retry=sockets.at(-1);assert.notEqual(retry,socket);retry.onopen();
    assert.equal(transport[active],false,'reconnected stream stops fallback polling');
    assert(transport[token]>before,'in-flight fallback responses are invalidated');
    retry.onclose();assert.equal(timers.at(-1).delay,2000,'failed connections back off');
  }
  // HTTP fallback requests can finish after a WebSocket has already recovered.
  // Their errors must not replace current state, even if a later fallback began.
  transport.Date=Date;
  const errorLabels={frameError:{textContent:''},encodingError:{textContent:''}};
  transport.$=id=>errorLabels[id];
  vm.runInContext(source.slice(source.indexOf('async function pollFrame('),source.indexOf('function socketUrl(')),transport);
  vm.runInContext(source.slice(source.indexOf('async function pollEncoded('),source.indexOf('function encodedPreviewPage(')),transport);
  for(const [start,poll,token,active,errorId] of [['startRawViewer','pollFrame','rawPollToken','rawPolling','frameError'],['startEncodedViewer','pollEncoded','encodedPollToken','encodedPolling','encodingError']]){
    transport[start]();const recovered=sockets.at(-1);
    for(const newerFallback of [false,true]){
      let reject;
      transport.api=()=>new Promise((_,fail)=>{reject=fail});
      transport[active]=true;
      const pending=transport[poll](1,transport[token]);
      recovered.onopen();
      if(newerFallback){transport[active]=true;++transport[token];}
      errorLabels[errorId].textContent='Current connection state';
      const scheduled=timers.length;
      reject(new Error('Obsolete HTTP failure'));await pending;
      assert.equal(errorLabels[errorId].textContent,'Current connection state','late fallback errors cannot overwrite recovered connection state');
      assert.equal(timers.length,scheduled,'obsolete fallback cannot schedule more polls');
    }
  }
  console.log('Browser bounded decoding, independent-frame gaps and reconnect recovery passed');
})().catch(error=>{console.error(error);process.exitCode=1});
