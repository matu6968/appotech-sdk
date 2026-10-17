#!/usr/bin/env python3
"""Create/upload separate baseline camera test copies; preserve card originals."""
import argparse,hashlib,json
from pathlib import Path
from PIL import Image
from apxtool.debug import DebugClient,USBTransport
from apxtool.storage import StorageClient
from apxtool.errors import ApxError
p=argparse.ArgumentParser();p.add_argument('--port',required=True);p.add_argument('--directory',type=Path,required=True)
p.add_argument('--upload',action='store_true');a=p.parse_args();a.directory.mkdir(parents=True,exist_ok=True)
report=[]
with USBTransport(port=a.port) as t:
 c=DebugClient(t);s=StorageClient(c);entries=list(s.list())
 if a.upload:
  for path in ('0:/SDKJPEG','0:/SDKJPEG/BASELINE','0:/SDKJPEG/ENCODER'):
   try:s.mkdir(path)
   except ApxError as e:
    if 'FR_EXIST' not in str(e):raise
 for entry in entries:
  name=entry['name']
  if not name.lower().startswith('jpeg-test-') or not name.lower().endswith('.jpg'):continue
  source=a.directory/name
  with source.open('xb') as out:s.download('0:/'+name,out)
  im=Image.open(source);im.load();target=a.directory/('baseline-'+name)
  im.convert('RGB').save(target,'JPEG',quality=90,subsampling=2,progressive=False)
  row={'source':name,'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),
       'baseline_sha256':hashlib.sha256(target.read_bytes()).hexdigest(),'dimensions':im.size,'card_path':'0:/SDKJPEG/BASELINE/'+name}
  if a.upload:
   with target.open('rb') as data:s.upload(row['card_path'],data)
  report.append(row);print(json.dumps(row),flush=True)
(a.directory/'inventory.json').write_text(json.dumps(report,indent=2)+'\n')
