#!/usr/bin/env python3
"""Read-only analysis of user-owned RAM snapshots, never an external cheat list.

Example: --exact score0.ram=0 --exact score2.ram=100 --width 32
Or: --changed before.ram after.ram --min 0 --max 100000
Snapshots may contain game data. Keep them outside the repository and APK.
Candidate matches alone do not establish a working cheat.
"""
import argparse
from pathlib import Path
import struct


def exact_candidates(samples, width=32):
    size=width//8
    if not samples or any(len(data)!=len(samples[0][0]) for data,_ in samples):
        raise ValueError('Equal-sized snapshots required')
    fmt={8:'<B',16:'<H',32:'<I'}[width]
    return [offset for offset in range(0,len(samples[0][0])-size+1,size)
            if all(struct.unpack_from(fmt,data,offset)[0]==value for data,value in samples)]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exact',action='append',default=[],metavar='PATH=VALUE')
    parser.add_argument('--changed',nargs=2,type=Path)
    parser.add_argument('--width',type=int,choices=(8,16,32),default=32)
    parser.add_argument('--min',type=int,default=0)
    parser.add_argument('--max',type=int,default=0xFFFFFFFF)
    parser.add_argument('--limit',type=int,default=100)
    args=parser.parse_args()
    if args.exact:
        samples=[]
        for argument in args.exact:
            path,value=argument.rsplit('=',1)
            samples.append((Path(path).read_bytes(),int(value,0)))
        offsets=exact_candidates(samples,args.width)
        print(f'{len(offsets)} candidates, aligned {args.width}-bit little endian')
        for offset in offsets[:args.limit]:print(f'RAM offset 0x{offset:06X}')
    elif args.changed:
        first,second=(path.read_bytes() for path in args.changed)
        if len(first)!=len(second):parser.error('Equal-sized snapshots required')
        fmt={8:'<B',16:'<H',32:'<I'}[args.width];size=args.width//8
        matches=[]
        for offset in range(0,len(first)-size+1,size):
            before=struct.unpack_from(fmt,first,offset)[0]
            after=struct.unpack_from(fmt,second,offset)[0]
            if before!=after and args.min<=before<=args.max and args.min<=after<=args.max:
                matches.append((offset,before,after))
        print(f'{len(matches)} changed candidates')
        for offset,before,after in matches[:args.limit]:print(f'0x{offset:06X}: {before} -> {after} ({after-before:+})')
    else:parser.error('Specify --exact or --changed')


if __name__=='__main__':main()
