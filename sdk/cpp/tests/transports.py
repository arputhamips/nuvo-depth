"""Real TLS/WebSocket and SCM_RIGHTS fixtures; hostile framing and NVR interoperability."""
import array, base64, copy, hashlib, http.server, json, math, os, pathlib, socket, ssl, struct, subprocess, sys, tempfile, threading, time
HEADER=struct.Struct('>4sHHHHIQQ')
def frame(sequence=1):
    p=[100.,0.,1.,0.,0.,100.,0.,0.,0.,0.,1.,0.];r=p.copy();r[3]=-10.
    geometry={'left_projection':p,'right_projection':r,'reprojection':[1,0,0,-1,0,1,0,0,0,0,0,100,0,0,10,0],'baseline_m':.1}
    j={'schema_version':1,'capture_epoch':1,'capture_sequence':sequence,'configuration_epoch':1,'capture_time_ns':1000000000+sequence*100000000,'capture_clock':'boottime','device_uuid':'1'*32,'run_uuid':'2'*32,'calibration_sha256':'3'*64,'calibration_authority':1,'depth_unit':'metres','depth_convention':'optical_axis_z','image_grid':{'width':2,'height':1},'matching_grid':{'width':2,'height':1},'calibration_geometry':{'image':geometry,'matching':geometry},'selected_products':40,'available_products':40,'products':[{'kind':'depth','format':'float32_le','width':2,'height':1,'offset':0,'length':8},{'kind':'validity','format':'uint8','width':2,'height':1,'offset':8,'length':2}]}
    for name in ('image_grid','matching_grid'):j[name].update(reference_width=2,reference_height=1,x_scale_to_reference=1.,y_scale_to_reference=1.,x_offset_to_reference=0.,y_offset_to_reference=0.)
    return encode(j,struct.pack('<ffBB',2.,math.nan,1,6),sequence)
def encode(j,payload,sequence=1):
    meta=json.dumps(j,separators=(',',':')).encode();return HEADER.pack(b'NUV1',1,0,5,0,len(meta),len(payload),sequence)+meta+payload
def run(*args,ok=True,environment=None,memory_limit=None):
    def limit_memory():
        import resource
        resource.setrlimit(resource.RLIMIT_AS,(memory_limit,memory_limit))
    p=subprocess.run([sys.argv[1],*map(str,args)],capture_output=True,text=True,timeout=12,env=environment,preexec_fn=limit_memory if memory_limit else None)
    assert (p.returncode==0)==ok,(args,p.returncode,p.stdout,p.stderr)
    return p
def local(root,malformed=False,cancel=False,invalidated=False,interrupt=False,terminal=False,empty=False,partial=False):
    path=root/('local-'+str(malformed)+str(cancel)+str(invalidated)+str(interrupt)+str(terminal)+str(empty)+str(partial));server=socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET);server.bind(str(path));server.listen()
    def serve():
        c,_=server.accept()
        try:
            assert c.recv(48)[:4]==b'NVS2';c.send(b'NVA2')
            if empty:
                assert c.recv(48)[:4]==b'NVW2';c.send(b'NVN2')
            for sequence in (1,2):
                assert c.recv(48)[:4]==b'NVW2'
                if invalidated:c.send(b'NVI2');return
                if partial:c.send(b'NVR2');return
                if cancel or interrupt:
                    c.recv(48);return
                wire=frame(sequence);fd=os.memfd_create('fixture',os.MFD_ALLOW_SEALING);os.write(fd,wire)
                import fcntl
                if not malformed:fcntl.fcntl(fd,fcntl.F_ADD_SEALS,fcntl.F_SEAL_GROW|fcntl.F_SEAL_SHRINK|getattr(fcntl,'F_SEAL_FUTURE_WRITE',0x10))
                ro=os.open('/proc/self/fd/'+str(fd),os.O_RDONLY);os.close(fd)
                try:c.sendmsg([b'NVR2'+bytes(4)+struct.pack('>Q',len(wire))+bytes.fromhex('2'*32)+struct.pack('>QQ',1,sequence)],[(socket.SOL_SOCKET,socket.SCM_RIGHTS,array.array('i',[ro]))])
                finally:os.close(ro)
                if malformed:return
        except (BrokenPipeError,ConnectionResetError):pass
        finally:c.close();server.close()
    t=threading.Thread(target=serve,daemon=True);t.start();result=run('terminal-local' if terminal else 'cancel-local' if cancel else 'interrupt-local' if interrupt else 'local',path,ok=terminal or not (malformed or invalidated));t.join(3);assert not t.is_alive();return result
class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version='HTTP/1.1'
    def log_message(self,*args):pass
    def reply(self,document,cookie=False):
        data=json.dumps(document).encode();self.send_response(200);self.send_header('Content-Length',str(len(data)))
        if self.server.closing:self.send_header('Connection','close');self.close_connection=True
        if cookie:self.send_header('Set-Cookie','nuvo_session='+'a'*64+'; Path=/; Secure; HttpOnly')
        self.end_headers();self.wfile.write(data)
    def do_POST(self):
        assert self.headers['Authorization']=='Bearer '+'c'*64
        self.reply({'csrf':'b'*64,'role':'admin'},True)
    def do_DELETE(self):
        self.server.revoked=True;self.reply({'closed':True})
    def do_GET(self):
        assert self.headers['Cookie']=='nuvo_session='+'a'*64
        if self.path=='/api/v1/capabilities':return self.reply({'protocol':'NuvoStream/1.0','products':['depth','validity']})
        assert self.path.startswith('/api/v1/stream?')
        accept=base64.b64encode(hashlib.sha1((self.headers['Sec-WebSocket-Key']+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
        self.send_response(101);self.send_header('Upgrade','websocket');self.send_header('Connection','Upgrade');self.send_header('Sec-WebSocket-Accept',accept);self.end_headers()
        if self.server.idle:
            try:self.connection.recv(1024)
            except OSError:pass
            return
        for sequence in (1,2):
            wire=frame(sequence)
            if self.server.malformed and sequence==1:wire=wire[:-1]
            if self.server.partial:
                self.wfile.write(b'\x82\x7e'+struct.pack('>H',len(wire))+wire[:64]);self.wfile.flush();self.close_connection=True;return
            self.wfile.write(b'\x82\x7e'+struct.pack('>H',len(wire))+wire);self.wfile.flush()
        self.close_connection=True
def main():
    with tempfile.TemporaryDirectory() as directory:
        root=pathlib.Path(directory);packet=root/'frame.nuv';packet.write_bytes(frame());run('decode',packet)
        recording=root/'capture.nvr';recording.write_bytes(b'NVR1\0\0\0\1'+b''.join(struct.pack('>Q',len(frame(i)))+frame(i) for i in (1,2)));assert run('replay',recording).stdout.split()==['1','2']
        assert run('eof-replay',recording).stdout.split()==['2']
        broken=root/'broken.nvr'
        for tail in (b'',struct.pack('>Q',len(frame()))+frame()):
            broken.write_bytes(b'NVR1\0\0\0\1'+tail)
            assert run('eof-replay',broken).stdout.split()==['1' if tail else '0']
        for tail in (b'\0',struct.pack('>Q',len(frame()))+frame()[:-1],struct.pack('>Q',31)+bytes(31),struct.pack('>Q',len(frame()))+bytes(len(frame()))+struct.pack('>Q',len(frame()))+frame()):
            broken.write_bytes(b'NVR1\0\0\0\1'+tail);run('terminal-replay',broken)
        broken.write_bytes(b'NVR1\0\0\0\1'+struct.pack('>Q',32+65536+64*1024*1024))
        assert 'truncated NVR1 frame' in run('replay',broken,ok=False,memory_limit=64*1024*1024).stderr
        # Canonical checkout additionally tests against the maintained Python
        # reader/writer; exported SDK fixtures remain independently runnable.
        python_sdk=pathlib.Path(__file__).resolve().parents[3]/'sdk/python'
        if python_sdk.is_dir():
            sys.path.insert(0,str(python_sdk))
            from nuvo_depth import RecordingReader,RecordingWriter
            with RecordingReader(recording) as reader:frames=list(reader)
            assert [f.capture_sequence for f in frames]==[1,2]
            python_recording=root/'python.nvr'
            with RecordingWriter(python_recording) as writer:
                for f in frames:writer.write(f)
            assert run('replay',python_recording).stdout.split()==['1','2']
        original=frame();meta_size=HEADER.unpack_from(original)[5];j=json.loads(original[32:32+meta_size]);payload=original[32+meta_size:]
        for bad in (original[:-1],original[:16]+b'\xff'*8+original[24:]):packet.write_bytes(bad);run('decode',packet,ok=False)
        for field,value in [('capture_time_ns',True),('capture_sequence',-1),('calibration_sha256','0'*64)]:
            invalid=dict(j);invalid[field]=value;packet.write_bytes(encode(invalid,payload));run('decode',packet,ok=False)
        for kind in ('product_grid','matching_baseline','pixel_scale','image_product_grid'):
            invalid=copy.deepcopy(j)
            if kind=='product_grid':invalid['matching_grid']['width']=3
            elif kind=='matching_baseline':
                g=invalid['calibration_geometry']['matching'];g['baseline_m']=.2;g['right_projection'][3]=-20;g['reprojection'][14]=5
            elif kind=='pixel_scale':invalid['matching_grid']['x_scale_to_reference']=2.
            else:
                invalid['products'][1].update(kind='left_mono',format='gray8');invalid['selected_products']=72;invalid['available_products']=72;invalid['image_grid']['width']=3
            packet.write_bytes(encode(invalid,payload));run('decode',packet,ok=False)
        invalid=copy.deepcopy(j)
        for g in invalid['calibration_geometry'].values():
            g['baseline_m']=float.fromhex('0x0.0000000000001p-1022');g['right_projection'][3]=-100*g['baseline_m']
        packet.write_bytes(encode(invalid,payload));run('decode',packet,ok=False)
        # Small but self-consistent geometry is valid; a zero or mismatched
        # encoded baseline must not hide inside an absolute matrix tolerance.
        small=copy.deepcopy(j)
        for g in small['calibration_geometry'].values():
            g['baseline_m']=1e-9;g['right_projection'][3]=-1e-7;g['reprojection'][14]=1e9
        packet.write_bytes(encode(small,payload));run('decode',packet)
        for translation in (0.,-5e-8):
            invalid=copy.deepcopy(small)
            for g in invalid['calibration_geometry'].values():g['right_projection'][3]=translation
            packet.write_bytes(encode(invalid,payload));run('decode',packet,ok=False)
        invalid=copy.deepcopy(small);g=invalid['calibration_geometry']['matching'];g['baseline_m']=1.5e-9;g['right_projection'][3]=-1.5e-7;g['reprojection'][14]=1/g['baseline_m']
        packet.write_bytes(encode(invalid,payload));run('decode',packet,ok=False)
        for focal in (0.,-1e-9):
            invalid=copy.deepcopy(j)
            for g in invalid['calibration_geometry'].values():
                g['left_projection'][0]=1e-9;g['right_projection'][0]=focal;g['right_projection'][3]=-1e-10;g['reprojection'][11]=1e-9
            packet.write_bytes(encode(invalid,payload));run('decode',packet,ok=False)
        invalid=copy.deepcopy(j);invalid['products'][1]['offset']=0;packet.write_bytes(encode(invalid,payload));run('decode',packet,ok=False)
        # Old beta-8-style transport needs deliberate, identity-bound geometry.
        legacy=copy.deepcopy(j)
        for key in ('calibration_geometry','image_grid','matching_grid'):legacy.pop(key)
        legacy['calibration_authority']=0;packet.write_bytes(encode(legacy,payload))
        fallback=root/'calibration.json';calibration={'calibration_sha256':j['calibration_sha256'],'matching_grid':j['matching_grid'],'matching':j['calibration_geometry']['matching']}
        fallback.write_text(json.dumps(calibration));run('override',packet,fallback)
        for translation in (0.,-5e-8):
            wrong=copy.deepcopy(calibration);wrong['matching']=copy.deepcopy(small['calibration_geometry']['matching']);wrong['matching']['right_projection'][3]=translation
            fallback.write_text(json.dumps(wrong));run('override',packet,fallback,ok=False)
        wrong=copy.deepcopy(calibration);wrong['matching']['baseline_m']=float.fromhex('0x0.0000000000001p-1022');wrong['matching']['right_projection'][3]=-100*wrong['matching']['baseline_m']
        fallback.write_text(json.dumps(wrong));run('override',packet,fallback,ok=False)
        wrong=copy.deepcopy(calibration);wrong['calibration_sha256']='4'*64;fallback.write_text(json.dumps(wrong));run('override',packet,fallback,ok=False)
        wrong=copy.deepcopy(calibration);wrong['matching_grid']['width']=3;fallback.write_text(json.dumps(wrong));run('override',packet,fallback,ok=False)
        fallback.write_text(json.dumps(calibration));packet.write_bytes(original);run('override',packet,fallback,ok=False)
        local(root);local(root,malformed=True);local(root,cancel=True);local(root,interrupt=True)
        local(root,empty=True);local(root,malformed=True,terminal=True);local(root,invalidated=True,terminal=True)
        local(root,partial=True,terminal=True)
        assert 'invalidated' in local(root,invalidated=True).stderr
        fifo=root/'blocked.nvr';os.mkfifo(fifo);run('replay',fifo,ok=False)
        # A saturated local listen queue must not hang source construction.
        backlog=root/'backlog.sock';server=socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET);server.bind(str(backlog));server.listen(0)
        queued=socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET);queued.connect(str(backlog))
        try:
            started=time.monotonic();run('local',backlog,ok=False);assert time.monotonic()-started<2.5
        finally:queued.close();server.close()
        if os.environ.get('NUVO_CLIENT_WITH_HTTPS')=='OFF':return
        key=root/'key.pem';cert=root/'cert.pem';token=root/'token';token.write_text('c'*64);token.chmod(0o600)
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(key),'-out',str(cert),'-days','1','-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost'],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler);server.daemon_threads=True;server.revoked=False;server.idle=False;server.closing=False;server.malformed=False;server.partial=False
        context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);context.load_cert_chain(cert,key);server.socket=context.wrap_socket(server.socket,server_side=True)
        t=threading.Thread(target=server.serve_forever,daemon=True);t.start();url='https://localhost:'+str(server.server_port)
        try:
            assert run('https',url,cert,token).stdout.split()==['1','2'];assert server.revoked
            server.closing=True;server.revoked=False
            assert run('https',url,cert,token).stdout.split()==['1','2'];assert server.revoked
            run('https',url.replace('localhost','127.0.0.1'),cert,token,ok=False)
            run('https',url,'',token,ok=False)
            assert run('https',url,cert,token,'127.0.0.1').stdout.split()==['1','2']
            run('https',url.replace('localhost','wrong.invalid'),cert,token,'127.0.0.1',ok=False)
            server.malformed=True;run('terminal-https',url,cert,token);server.malformed=False
            server.partial=True;run('terminal-https',url,cert,token);server.partial=False
            server.idle=True;run('cancel-https',url,cert,token)
            # Force an outstanding async query independently of external DNS.
            shim=root/'dns.c';library=root/'dns.so'
            shim.write_text('#include <ares.h>\nvoid ares_getaddrinfo(ares_channel c,const char*n,const char*s,const struct ares_addrinfo_hints*h,ares_addrinfo_callback cb,void*a){(void)c;(void)n;(void)s;(void)h;(void)cb;(void)a;}\n')
            subprocess.run(['cc','-shared','-fPIC',str(shim),'-o',str(library)],check=True)
            environment=dict(os.environ,LD_PRELOAD=str(library))
            start=time.monotonic();run('cancel-https','https://stalled.invalid:7443',cert,token,environment=environment);assert time.monotonic()-start<1.5
            start=time.monotonic();run('https','https://stalled.invalid:7443',cert,token,ok=False,environment=environment);assert time.monotonic()-start<2.5
        finally:server.shutdown();server.server_close();t.join(3)
if __name__=='__main__':main()
