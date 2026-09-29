#!/usr/bin/env python3
"""Export exact little-endian FP32 v06 weights and golden matrices, stdlib only."""
import ast, hashlib, json, math, pathlib, struct, sys, zipfile

def npy(data):
    assert data[:8] == b'\x93NUMPY\x01\x00'
    n=struct.unpack_from('<H',data,8)[0]
    meta=ast.literal_eval(data[10:10+n].decode('ascii'))
    assert meta['descr']=='<f4' and not meta['fortran_order']
    body=data[10+n:]
    assert len(body)==math.prod(meta['shape'])*4
    assert all(math.isfinite(v[0]) for v in struct.iter_unpack('<f',body))
    return meta['shape'],body

def export(root,out):
    root=pathlib.Path(root);out=pathlib.Path(out);out.mkdir(parents=True,exist_ok=True)
    names=['input_norm.weight','input_norm.bias','input_proj.weight','input_proj.bias']
    shapes=[(50,),(50,),(128,50),(128,)]
    for layer in range(2):
        names += ['gru.%s_l%d'%(key,layer) for key in ('weight_ih','weight_hh','bias_ih','bias_hh')]
        shapes += [(384,128),(384,128),(384,),(384,)]
    names+=['head.weight','head.bias'];shapes += [(4,128),(4,)]
    with zipfile.ZipFile(root/'model/state_dict.npz') as z,open(out/'v06.bin','wb') as f:
        assert set(z.namelist())==set(n+'.npy' for n in names)
        f.write(b'SSEV06M1')
        for name,shape in zip(names,shapes):
            actual,body=npy(z.read(name+'.npy'));assert actual==shape,(name,actual,shape)
            f.write(body)
    with zipfile.ZipFile(root/'golden/600000.SH_20260401/model_stage_outputs.npz') as z:
        for name in z.namelist():
            shape,body=npy(z.read(name));(out/(name[:-4]+'.f32')).write_bytes(body)
    (out/'factors.txt').write_bytes((root/'factors/factors.txt').read_bytes())
    manifest={'model_version':'v0.6','model_id':'v06-b15-mh4-s43-sse-20260910',
        'heads':[15,30,60,120],'units':'permille','rows':4017,'external_scaler':None,
        'weights_sha256':hashlib.sha256((out/'v06.bin').read_bytes()).hexdigest()}
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(json.dumps(manifest))
if __name__=='__main__':export(*sys.argv[1:])
