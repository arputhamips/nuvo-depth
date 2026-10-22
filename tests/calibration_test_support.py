"""Synthetic bundle fixtures only; passing test records do not establish accuracy."""
import hashlib
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]

def write_json(path,value):
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text(json.dumps(value,sort_keys=True,separators=(',',':')))

def prepare_root(root):
    (root/'etc/nuvo-depth').mkdir(parents=True)
    (root/'etc/nuvo-depth/daemon.yaml').write_bytes((ROOT/'config/nuvo_pi5_depth_100mm.yaml').read_bytes())
    service=root/'usr/lib/systemd/system/nuvo-depthd-native.service';service.parent.mkdir(parents=True)
    service.write_text('[Unit]\nConditionPathExists=/etc/nuvo-depth/capture-enabled\n[Service]\nExecStart=/usr/bin/depthd --run /etc/nuvo-depth/daemon.yaml --allow-provisional-depth\n')
    identity=root/'var/lib/nuvo-depth/identity/device.uuid';identity.parent.mkdir(parents=True)
    identity.write_text('unit-1\n')
    contract=json.loads((ROOT/'config/calibration/nuvo_depth_100mm/profile.json').read_text())
    write_json(root/'usr/share/nuvo-depth/calibration/profile.json',contract)
    return {'product':{'platform':'raspberry-pi-5','profile_id':contract['profile_id']},'rig':{'sensor':'imx296'},
            'capture':{'width':728,'height':544,'target_hz':10,'left_camera_index':0,'right_camera_index':1}}

def bundle(root,**changes):
    source=root/'fixture-bundle';source.mkdir(exist_ok=True)
    for name in ['front_left.yaml','front_right.yaml']:
        text=(ROOT/'config/calibration/nuvo_depth_100mm'/name).read_text().replace('provisional_recalibration_required','user_validated')
        (source/name).write_text(text)
    write_json(source/'validation.json',{'status':'pass','synthetic_test_fixture':True})
    contract=json.loads((ROOT/'config/calibration/nuvo_depth_100mm/profile.json').read_text())
    value={'schema_version':1,'unit_id':'unit-1','authority':'user_validated','preset_id':contract['profile_id'],
           'left_camera_id':contract['left_camera_id'],'right_camera_id':contract['right_camera_id'],
           'profile':{'sensor_mode':'imx296-728x544','image_width':728,'image_height':544,'frame_rates_hz':[10]},
           'assets':{name:hashlib.sha256((source/name).read_bytes()).hexdigest() for name in ['front_left.yaml','front_right.yaml','validation.json']}}
    value.update(changes)
    value['bundle_id']=hashlib.sha256(json.dumps(value,sort_keys=True,separators=(',',':')).encode()).hexdigest()
    write_json(source/'manifest.json',value)
    target=root/'var/lib/nuvo-depth/calibrations/user'/value['bundle_id'];target.parent.mkdir(parents=True,exist_ok=True)
    source.rename(target)
    active=target.parent/'active';active.unlink(missing_ok=True);active.symlink_to(value['bundle_id'])
    return target,value
