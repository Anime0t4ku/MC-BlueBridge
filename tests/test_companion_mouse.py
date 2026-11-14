"""Verify the host integration against the Companion Remote 4.2.0 source."""
import ast
import importlib.util
import json
from pathlib import Path
import sys

root=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('patcher',root/'tools/companion_remote_mouse.py')
p=importlib.util.module_from_spec(spec);spec.loader.exec_module(p)
source=Path(sys.argv[1]).read_text(encoding='utf-8')
patched,html,daemon=p.patch(source)
assert p.patch(p.patch_mouse(source)[0])[0]==patched, 'upgrade from mouse-only patch must match fresh install'
assert p.patch(p.patch_gyro(source)[0])[0]==patched, 'upgrade from gyro patch must match fresh install'
assert p.patch(p.extract_daemon(source))[2]==daemon
try:p.patch(patched)
except ValueError:pass
else:raise AssertionError('must reject repeat patch')
assert html.count('id="ds4GyroPanel"')==1
ns={'json':json};exec(p.GYRO_HELPER,ns)
class Adapter:
    def __init__(self,supported=True):self.calls=[];self.supported=supported
    def request_json(self,cmd,*args):
        self.calls.append((cmd,args))
        if cmd=='HELLO':return {'capabilities':{'ds4_gyro':self.supported}}
        return {'restart_required':True}

aim=dict(enabled=True,sensitivity=125,activation=1,flags=3,pause_button=10)
a=Adapter();ns['save_pointer_settings'](a,dict(enabled=True,ds4_aim=aim))
assert a.calls[-1]==('DS4_AIM_SET',(1,125,1,3,10,1))
a=Adapter(False);ns['save_pointer_settings'](a,dict(enabled=False))
assert a.calls==[('TOUCH_MOUSE_SET',(0,))]
for key,values in dict(enabled=[1,'true',None],sensitivity=[0,201,True,'100'],activation=[-1,3,False],flags=[-1,8,True],pause_button=[14,254,256,False]).items():
    for value in values:
        invalid=dict(aim,**{key:value});a=Adapter()
        try:ns['save_pointer_settings'](a,dict(enabled=True,ds4_aim=invalid))
        except ValueError:pass
        else:raise AssertionError((key,value))
        assert not any(c[0].endswith('_SET') for c in a.calls)
a=Adapter(False)
try:ns['save_pointer_settings'](a,dict(enabled=True,ds4_aim=aim))
except ValueError:pass
else:raise AssertionError('unsupported firmware must reject gyro settings')
tree=ast.parse(daemon)
cls=next(x for x in tree.body if isinstance(x,ast.ClassDef) and x.name=='BlueBridgeManager')
status=next(x for x in cls.body if isinstance(x,ast.FunctionDef) and x.name=='status')
status_ns={};exec(compile(ast.Module(body=[status],type_ignores=[]),'<status>','exec'),status_ns)
for supported in (True,False):
    a=Adapter(supported);a.path='/dev/mock';a.info={};a.adapter_id='mock'
    result=status_ns['status'](a);assert result['detected']
    assert ('ds4_aim' in result)==supported
out=root/'build-host';out.mkdir(exist_ok=True)
(out/'remote-pointer.js').write_text(html.split('// SOOPAH_TOUCH_MOUSE_V1',1)[1].split('</script>',1)[0],encoding='utf-8')
import re
(out/'remote-full.js').write_text('\n'.join(re.findall(r'<script[^>]*>(.*?)</script>',html,re.S)),encoding='utf-8')
print('companion: fresh install and V1 upgrade, syntax, capability fallback, DS4 save and strict input validation passed')

light_ns={};exec(p.LIGHTBAR_HELPER,light_ns)
class ColoursAdapter:
    def __init__(self):self.calls=[]
    def request(self,*args):self.calls.append(args)
    def request_json(self,*args):return {'theme':2,'players':[[64,0,0],[0,64,0],[0,64,0],[32,0,32]],'controllers':[]}
palette=dict(theme=2,players=[[64,0,0],[0,64,0],[7,8,9],[10,11,12]])
a=ColoursAdapter();light_ns['save_lightbar_palette'](a,palette)
assert a.calls==[('LIGHTBAR_PALETTE_SET',2,64,0,0,0,64,0,7,8,9,10,11,12)]
choice=dict(index=0,id=123,player=0,custom_enabled=True,rgb=[1,2,3])
a=ColoursAdapter();light_ns['save_lightbar_controller'](a,choice)
assert a.calls==[('LIGHTBAR_SET',0,123,0,1,1,2,3)]
for invalid in [dict(palette,theme=True),dict(palette,theme=3),dict(palette,theme=1),dict(palette,players=[]),dict(palette,players=[[True,0,0]]*4),dict(palette,players=[[256,0,0]]*4),None]:
    a=ColoursAdapter()
    try:light_ns['save_lightbar_palette'](a,invalid)
    except ValueError:pass
    else:raise AssertionError(invalid)
    assert not a.calls
for invalid in [dict(choice,id=4294967296),dict(choice,index=8),dict(choice,player=5),dict(choice,custom_enabled=1),dict(choice,rgb=[1,2]),dict(choice,id=True)]:
    a=ColoursAdapter()
    try:light_ns['save_lightbar_controller'](a,invalid)
    except ValueError:pass
    else:raise AssertionError(invalid)
    assert not a.calls
(out/'remote-lightbar.js').write_text(html.split('// SOOPAH_LIGHTBAR_V1',1)[1].split('</script>',1)[0],encoding='utf-8')
print('companion colours: palette/theme validation, controller identity bounds and colour serialization passed')
