"""Runnable depth/validity and preview examples, with optional exact capture pairing."""
from __future__ import annotations
import argparse
import getpass
import math
from pathlib import Path
import struct
import zlib
from nuvo_depth import NuvoClient, RecordingReader, RecordingWriter
from nuvo_depth.first_use import describe

def pixel(frame,x,y):
    depth=frame.products.get('depth'); validity=frame.products.get('validity')
    if depth is None or validity is None or depth.format!='float32_le' or validity.format!='uint8':
        raise ValueError('This example requires float32 depth and uint8 validity')
    if (depth.width,depth.height)!=(validity.width,validity.height): raise ValueError('Associated product dimensions disagree')
    if not 0<=x<depth.width or not 0<=y<depth.height: raise ValueError('Selected pixel is outside the depth grid')
    index=y*depth.width+x; value=struct.unpack_from('<f',depth.data,index*4)[0]; code=validity.data[index]
    return value if code==1 and math.isfinite(value) and value>0 else None,code

def display(frame,path,near=.3,far=6.):
    if not math.isfinite(near) or not math.isfinite(far) or not 0<near<far: raise ValueError('Use finite 0 < near < far visualization bounds')
    depth=frame.products.get('depth')
    if depth is None: raise ValueError('Depth product is missing')
    rows=bytearray()
    for y in range(depth.height):
        rows.append(0)  # PNG no-filter scanline.
        for x in range(depth.width):
            value,_=pixel(frame,x,y)
            rows.append(0 if value is None else 1+round(254*max(0,min(1,(value-near)/(far-near)))))
    def chunk(kind,data): return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff)
    data=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',depth.width,depth.height,8,0,0,0,0))+chunk(b'IDAT',zlib.compress(bytes(rows)))+chunk(b'IEND',b'')
    with Path(path).open('xb') as output: output.write(data)
    print(f'Visualization only: {path}; black=invalid; valid grey scale {near:g} to {far:g} metres. Original depth and validity are unchanged.')

def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('example',choices=('single','pixel','display','stream','record','replay','preview','paired'))
    parser.add_argument('--url'); parser.add_argument('--ca-file',type=Path); parser.add_argument('--token-file',type=Path)
    parser.add_argument('--input',type=Path,help='NVR1 recording; always labeled saved/replay data')
    parser.add_argument('--output',type=Path)
    parser.add_argument('--frames',type=int,default=3)
    parser.add_argument('--x',type=int,default=0); parser.add_argument('--y',type=int,default=0)
    parser.add_argument('--near',type=float,default=.3); parser.add_argument('--far',type=float,default=6.)
    args=parser.parse_args(argv)
    if not 1<=args.frames<=10000: parser.error('--frames must be 1..10000')
    if args.input and (args.url or args.token_file): parser.error('saved input does not take a URL or token')
    if args.input and args.example in ('preview','paired'): parser.error('preview/paired require live streams')
    if args.example=='replay' and not args.input: parser.error('replay needs --input recording.nvr')
    if args.example in ('record','display') and args.output is None: parser.error('record/display needs a new --output filename')
    manager=None; writer=None; client=None
    try:
        if args.input:
            print('SAVED REPLAY DATA — live freshness and current device readiness are not checked.')
            manager=RecordingReader(args.input)
        else:
            if not args.url or not args.ca_file: parser.error('live acquisition needs --url and --ca-file')
            client=NuvoClient(args.url,ca_file=str(args.ca_file),timeout=10)
            token=args.token_file.read_text().strip() if args.token_file else getpass.getpass('Device administrator token (hidden): ')
            client.claim(token)
            if args.example=='preview':
                manager=client.encoded_frames(cadence_hz=5)
            elif args.example=='paired':
                manager=client.paired_frames(products=('depth','validity'),cadence_hz=2,preview_cadence_hz=2)
                print('Exact capture pairs only; unmatched captures are dropped. A match is not guaranteed within the timeout.')
            else:
                manager=client.frames(products=('depth','validity'),cadence_hz=1,delivery='best_effort')
        count=0
        target=args.frames if args.example in ('stream','record','replay','preview','paired') else 1
        if args.example=='record': writer=RecordingWriter(args.output)
        with manager as source:
            for frame in source:
                count+=1
                if args.example in ('preview','paired'):
                    picture=frame.encoded if args.example=='paired' else frame
                    print(f'{picture.identity}: {picture.codec} compressed preview, {len(picture.data)} bytes; not numeric depth.')
                    if args.example=='preview':
                        if count>=target: break
                        continue
                    frame=frame.frame
                if writer: writer.write(frame)
                if args.example=='pixel':
                    value,code=pixel(frame,args.x,args.y)
                    print(f'{frame.identity}: pixel ({args.x},{args.y}) '+(f'{value:.4f} m optical-axis Z' if value is not None else f'invalid (validity code {code}); do not consume NaN as zero'))
                else:
                    try: print(describe(frame))
                    except ValueError as error: print(f'{frame.identity}: {error}')
                if args.example=='display': display(frame,args.output,args.near,args.far)
                if args.example=='single' and args.output:
                    with args.output.open('xb') as output: output.write(frame.to_bytes())
                if count>=target: break
        if count==0 or (not args.input and count<target): raise RuntimeError(f'Stream ended after {count} of {target} requested frames; complete recorded frames remain readable. No reconnect or gap filling was attempted.')
        print(f'Completed {count} received item(s). Calibration authority is declared validation, not independent accuracy evidence.')
    except (OSError,ValueError,RuntimeError) as error:
        parser.exit(1,f'Example failed: {error}\nVerify certificate hostname/trust, SDK credentials, required raw/preview outputs and capture readiness.\n')
    finally:
        if writer: writer.close()
        if client is not None: client.close()

if __name__=='__main__': main()
