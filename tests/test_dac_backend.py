# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline volume mapping, device identity, persistence and write-boundary tests."""
import copy
import json
import tempfile
import unittest
import importlib.util
import sys
from pathlib import Path
from unittest.mock import patch
from aegaudio import backend,dac_settings

CARD=dict(index=4,device_path='/sys/devices/pci0000:00/0000:06:00.0',vendor='0x11020011',subsystem='0x11020051')
DAC=dict(numid=76,name=backend.DIRECT_DAC,key='dac-key',iface='MIXER',writable=True,type='INTEGER',count=2,min=0,max=255,values=['209','209'])

class DacTests(unittest.TestCase):
    def test_service_restores_only_matching_dac_preference(self):
        path=Path(__file__).resolve().parents[1]/'integration/ae5_service.py'
        loader=importlib.util.spec_from_file_location('ae5_volume_service_test',path)
        service=importlib.util.module_from_spec(loader)
        with patch.dict(sys.modules,{'dac_settings':dac_settings}):loader.loader.exec_module(service)
        cfg=dict(pci='0000:06:00.0',vendor_id=CARD['vendor'],subsystem_id=CARD['subsystem'],home='/example')
        with patch.object(service,'config',return_value=cfg),patch.object(dac_settings,'load',return_value=['215','217']),patch.object(service,'command',return_value={'exit':0,'stdout':': values=215,217\n'}) as command,patch.object(service,'log'):
            service.restore_dac_preference(4)
            writes=[args.args[0] for args in command.call_args_list if 'cset' in args.args[0]]
            self.assertEqual(writes,[['amixer','-c','4','cset','name='+backend.DIRECT_DAC,'215,217']])
        for value in (None,ValueError('wrong card')):
            with patch.object(service,'config',return_value=cfg),patch.object(dac_settings,'load',side_effect=value if isinstance(value,Exception) else None,return_value=None),patch.object(service,'command') as command,patch.object(service,'log'):
                service.restore_dac_preference(4)
                command.assert_not_called()

    def test_db_metadata(self):
        text="""numid=76,iface=MIXER,name='AE-5: Direct DAC Playback Volume'
  ; type=INTEGER,access=rw-vR--,values=2,min=0,max=255,step=1
  : values=209,209
  | dBscale-min=-127.50dB,step=0.50dB,mute=0
"""
        c=backend.parse_controls(text)[0]
        self.assertEqual(c['db_min']+209*c['db_step'],-23)

    def test_profile_does_not_change_dac(self):
        data=backend.profile(CARD,[DAC])
        self.assertEqual(data['controls'],[])
        data['controls']=[DAC]
        with patch.object(backend,'current_card',return_value=CARD),patch.object(backend,'controls',return_value=[DAC]):
            with self.assertRaises(ValueError):backend.profile_plan(CARD,data)

    def test_write_then_persist_without_other_control(self):
        after=copy.deepcopy(DAC);after['values']=['215','211']
        with patch.object(backend,'current_card',return_value=CARD),patch.object(backend,'controls',side_effect=[[DAC],[after]]),patch.object(backend,'run') as run,patch.object(dac_settings,'save') as save:
            self.assertEqual(backend.set_control(CARD,DAC,after['values'])['values'],after['values'])
            run.assert_called_once_with(['amixer','-c','4','cset','numid=76','215,211'])
            self.assertEqual(save.call_args.args[2],after['values'])

    def test_stale_or_invalid_writes_never_reach_hardware(self):
        for values,current in [(['256','209'],DAC),(['209'],DAC),(['0','0'],dict(DAC,values=['208','208']))]:
            with self.subTest(values=values),patch.object(backend,'current_card',return_value=CARD),patch.object(backend,'controls',return_value=[current]),patch.object(backend,'run') as run:
                with self.assertRaises((ValueError,RuntimeError)):backend.set_control(CARD,DAC,values)
                run.assert_not_called()

    def test_failed_readback_not_saved(self):
        with patch.object(backend,'current_card',return_value=CARD),patch.object(backend,'controls',return_value=[DAC]),patch.object(backend,'run'),patch.object(dac_settings,'save') as save:
            with self.assertRaisesRegex(RuntimeError,'readback differs'):backend.set_control(CARD,DAC,['215','215'])
            save.assert_not_called()

    def test_save_failure_reports_applied_level(self):
        after=dict(DAC,values=['215','215'])
        with patch.object(backend,'current_card',return_value=CARD),patch.object(backend,'controls',side_effect=[[DAC],[after]]),patch.object(backend,'run') as run,patch.object(dac_settings,'save',side_effect=OSError('read-only directory')):
            with self.assertRaisesRegex(RuntimeError,'was applied, but saving'):backend.set_control(CARD,DAC,after['values'])
            self.assertEqual(run.call_count,1)

    def test_preference_identity_and_ranges(self):
        identity=dac_settings.identity(CARD)
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)/'direct-dac.json'
            self.assertIsNone(dac_settings.load(path,identity))
            dac_settings.save(path,identity,['209','209'])
            self.assertEqual(dac_settings.load(path,identity),['209','209'])
            with self.assertRaises(ValueError):dac_settings.load(path,dict(identity,pci='0000:08:00.0'))
            for values in ([256,0],[-1,0],[True,0],[1],['209',209]):
                path.write_text(json.dumps(dict(schema=1,device=identity,values=values)))
                with self.assertRaises(ValueError):dac_settings.load(path,identity)

    def test_direct_flag_covers_192k_and_idle(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);pcm=root/'pcm0p/sub0';pcm.mkdir(parents=True)
            (pcm/'hw_params').write_text('format: S32_LE\nrate: 192000 (192000/1)\n')
            with patch.object(backend,'Path',return_value=root):
                self.assertTrue(backend.state(CARD,[dict(name='AE-5: Direct Active',values=['on'])])['direct'])
                self.assertFalse(backend.state(CARD,[dict(name='AE-5: Direct Active',values=['off'])])['direct'])
                (pcm/'hw_params').write_text('closed')
                self.assertTrue(backend.state(CARD,[dict(name='AE-5: Direct Active',values=['on'])])['direct'])

if __name__=='__main__':unittest.main()
