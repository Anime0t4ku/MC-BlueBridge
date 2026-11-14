#!/usr/bin/env python3
"""Add DS4 settings to Companion Remote 4.2.0. No firmware writes."""
import argparse
import ast
import datetime
import os
from pathlib import Path
import re

TARGETS = [Path('/media/fat/Scripts/companion_remote.sh'),
           Path('/media/fat/Scripts/.config/companion_remote/companion_remote_daemon')]

def replace_file(path, data):
    temporary=path.with_name(path.name+'.mouse.tmp')
    with temporary.open('wb') as f:
        f.write(data);f.flush();os.fsync(f.fileno())
    temporary.chmod(path.stat().st_mode)
    os.replace(str(temporary),str(path))

def install(paths):
    prepared=[]
    for path in paths:
        raw=path.read_bytes()
        text=raw.decode('utf-8').replace('\r\n','\n')
        prepared.append((path,raw,patch(text)[0].encode('utf-8')))
    stamp=datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    # Create every backup before changing either target.
    for path,raw,patched in prepared:
        backup=path.with_name(path.name+'.before-mouse-'+stamp)
        with backup.open('xb') as f:
            f.write(raw);f.flush();os.fsync(f.fileno())
        print('Backup:',backup)
    written=[]
    try:
        for path,raw,patched in prepared:
            replace_file(path,patched);written.append((path,raw))
            print('Patched:',path)
    except Exception:
        for path,raw in reversed(written):replace_file(path,raw)
        raise
    print('Rollback if needed: python3 /tmp/add_bluebridge_mouse.py --restore '+stamp)

def restore(stamp):
    if not re.fullmatch(r'\d{8}-\d{6}',stamp):raise ValueError('Invalid backup timestamp')
    prepared=[]
    for path in TARGETS:
        raw=path.read_bytes()
        if MARKER.encode() not in raw:raise ValueError('Refusing to overwrite an unpatched or updated file: '+str(path))
        backup=path.with_name(path.name+'.before-mouse-'+stamp)
        data=backup.read_bytes()
        ast.parse(extract_daemon(data.decode('utf-8').replace('\r\n','\n')))
        prepared.append((path,data))
    for path,data in prepared:replace_file(path,data);print('Restored:',path)
    print('Run: sh /media/fat/Scripts/companion_remote.sh restart --unattended')

MARKER = "SOOPAH_TOUCH_MOUSE_V1"
PANEL = '''<section id="touchMousePanel" class="panel hidden">
<h2>Touchpad mouse</h2>
<label><input id="touchMouseEnabled" type="checkbox" onchange="touchMouseDirty=true"> Use DS4 touchpad as a USB mouse</label>
<p class="subtle">Slide and press the DS4 touchpad. Requires single-controller MiSTer output. Other gamepad controls remain available.</p>
<p id="touchMouseStatus" class="subtle" role="status"></p>
<button id="touchMouseSave" class="btn primary" onclick="saveTouchMouse()" disabled>Save mouse setting and restart</button>
<p class="subtle">This adapter-wide setting applies to supported controllers and is off by default. Leave Controller Tester before checking movement in a mouse-capable core. The tester captures input and suppresses USB output.</p>
</section>'''
JS = '''
// SOOPAH_TOUCH_MOUSE_V1
let touchMouseSaving=false,touchMouseDirty=false,touchMouseAdapter='';
function renderTouchMouse(){
  if(touchMouseAdapter!==selectedAdapterId){touchMouseDirty=false;touchMouseAdapter=selectedAdapterId;}
  const supported=!!statusData.capabilities?.touch_mouse;
  $('touchMousePanel').classList.toggle('hidden',!supported);
  const data=statusData.touch_mouse;
  $('touchMouseSave').disabled=!supported||!data||touchMouseSaving;
  if(!supported)return;
  if(!data){$('touchMouseEnabled').disabled=true;$('touchMouseStatus').textContent='Unable to read mouse settings. Refresh to retry.';return;}
  if(!touchMouseSaving&&!touchMouseDirty) $('touchMouseEnabled').checked=!!data.enabled;
  const eligible=!statusData.multi_controller&&Number(statusData.output_mode)===1&&data.available;
  $('touchMouseEnabled').disabled=touchMouseSaving;
  $('touchMouseStatus').textContent=!eligible
    ? 'Mouse output requires single-controller MiSTer mode. Your saved choice is retained.'
    : data.enabled!==data.active
      ? 'Setting saved. Waiting for the adapter to restart.'
      : data.active?'Mouse output is active. Exit Controller Tester to use it.':'Mouse output is off.';
}
async function saveTouchMouse(){
  if(touchMouseSaving||!statusData.touch_mouse)return;
  touchMouseSaving=true;
  $('touchMouseSave').disabled=true;
  $('touchMouseEnabled').disabled=true;
  try{
    const result=await post('/api/bluebridge/touch-mouse',{enabled:$('touchMouseEnabled').checked});
    touchMouseDirty=false;
    statusData.touch_mouse.enabled=$('touchMouseEnabled').checked;
    toast(result.mouse?.restart_required?'Mouse setting saved. Adapter restarting…':'Mouse setting saved.');
    setTimeout(()=>refresh(),1500);
  }catch(e){toast(e.message,true);}
  finally{touchMouseSaving=false;renderTouchMouse();}
}
saveTouchMouse=guardAdapterAction(saveTouchMouse);
'''
STATUS = '''            # SOOPAH_TOUCH_MOUSE_V1: optional fork capability; upstream stays usable.
            if status["capabilities"].get("touch_mouse"):
                try:
                    status["touch_mouse"] = self.request_json("TOUCH_MOUSE_GET")
                except Exception as mouse_error:
                    status["touch_mouse_error"] = str(mouse_error)
'''
ROUTE = '''                # SOOPAH_TOUCH_MOUSE_V1
                if request_path == "/api/bluebridge/touch-mouse":
                    if method != "POST":
                        _json_http(sock, response(False, "error", "POST required"), "405 Method Not Allowed")
                        return
                    try:
                        if not bluebridge.request_json("HELLO").get("capabilities", {}).get("touch_mouse"):
                            raise ValueError("This firmware does not support touchpad mouse output")
                        payload = json.loads(request_body.decode("utf-8") or "{}")
                        if not isinstance(payload, dict) or type(payload.get("enabled")) is not bool:
                            raise ValueError("enabled must be a boolean")
                        result = bluebridge.request_json("TOUCH_MOUSE_SET", int(payload["enabled"]))
                        _json_http(sock, response(True, "bluebridge", "Mouse setting saved", mouse=result))
                    except Exception as mouse_error:
                        _json_http(sock, response(False, "error", str(mouse_error)), "400 Bad Request")
                    return
'''

def once(text, old, new):
    if text.count(old) != 1:
        raise ValueError('Source does not match supported Companion Remote 4.2.0: '+old[:65])
    return text.replace(old, new, 1)

def patch_mouse(text):
    if MARKER in text:
        raise ValueError('Mouse integration is already installed')
    match=re.search(r"BLUEBRIDGE_HTML = r'''(.*?)'''\.encode\(\"utf-8\"\)", text, re.S)
    if not match:
        raise ValueError('Cannot locate BlueBridge HTML')
    html=match.group(1)
    html=once(html, '<section id="controllerCard"', PANEL+'<section id="controllerCard"')
    html=once(html, 'function renderAll(){', 'function renderAll(){renderTouchMouse();')
    html=once(html, '</script>', JS+'</script>')
    text=text[:match.start(1)]+html+text[match.end(1):]
    anchor='            status["capabilities"] = hello.get("capabilities", {})\n'
    text=once(text,anchor,anchor+STATUS)
    anchor='                if request_path == "/api/bluebridge/output":\n'
    text=once(text,anchor,ROUTE+anchor)
    daemon=extract_daemon(text)
    ast.parse(daemon)
    return text,html,daemon

GYRO_MARKER = 'SOOPAH_DS4_GYRO_V1'
GYRO_PANEL = '''<fieldset id="ds4GyroPanel" class="hidden" disabled style="border:1px solid var(--border);border-radius:12px;padding:16px;margin:16px 0">
<legend>DS4 gyro mouse (experimental)</legend>
<label><input id="ds4GyroEnabled" type="checkbox"> Enable DS4 gyro mouse</label>
<div class="form-group"><label for="ds4GyroSensitivity">Sensitivity (%)</label><input id="ds4GyroSensitivity" type="number" min="1" max="200" value="100"></div>
<div class="form-group"><label for="ds4GyroActivation">Move the pointer</label><select id="ds4GyroActivation"><option value="1">While touching the touchpad</option><option value="2">While holding L2</option><option value="0">Whenever gyro is enabled</option></select></div>
<label><input id="ds4GyroInvertX" type="checkbox"> Invert horizontal movement</label>
<label><input id="ds4GyroInvertY" type="checkbox"> Invert vertical movement</label>
<label><input id="ds4GyroFire" type="checkbox"> Use R2 for mouse left-click / fire</label>
<div class="form-group"><label for="ds4GyroPause">Hold to pause and reposition</label><select id="ds4GyroPause"></select></div>
<p class="subtle">Mouse output above must also be enabled. Touchpad movement remains available. To zero resting drift, hold the pause button with the controller still for at least half a second. The pause button is reserved while gyro is enabled; optional R2 fire replaces gamepad R2. Settings apply after restart.</p>
</fieldset>'''
GYRO_JS = '''
// SOOPAH_DS4_GYRO_V1
function renderDS4Gyro(){
  const supported=!!statusData.capabilities?.ds4_gyro,aim=statusData.ds4_aim;
  $('ds4GyroPanel').classList.toggle('hidden',!supported);
  $('ds4GyroPanel').disabled=!supported||!aim||touchMouseSaving;
  if(!supported||!aim||touchMouseDirty||touchMouseSaving)return;
  $('ds4GyroEnabled').checked=!!aim.enabled;$('ds4GyroSensitivity').value=String(aim.sensitivity);
  $('ds4GyroActivation').value=String(aim.activation);
  $('ds4GyroInvertX').checked=!!(aim.flags&1);$('ds4GyroInvertY').checked=!!(aim.flags&2);$('ds4GyroFire').checked=!!(aim.flags&4);
  const labels=['Cross','Circle','Square','Triangle','L1','R1','L2','R2','Share','Options','L3','R3','PS','Touchpad click'];
  $('ds4GyroPause').replaceChildren(new Option('None',255),...labels.map((name,i)=>new Option(name,i)));
  $('ds4GyroPause').value=String(aim.pause_button);
}
function pointerPayload(){
  const payload={enabled:$('touchMouseEnabled').checked};
  if(statusData.capabilities?.ds4_gyro){
    if(!statusData.ds4_aim)throw Error('Unable to read DS4 gyro settings. Refresh before saving.');
    const sensitivity=Number($('ds4GyroSensitivity').value);
    if(!Number.isInteger(sensitivity)||sensitivity<1||sensitivity>200)throw Error('Choose a whole-number sensitivity from 1 to 200%.');
    payload.ds4_aim={enabled:$('ds4GyroEnabled').checked,sensitivity,
      activation:Number($('ds4GyroActivation').value),pause_button:Number($('ds4GyroPause').value),
      flags:($('ds4GyroInvertX').checked?1:0)|($('ds4GyroInvertY').checked?2:0)|($('ds4GyroFire').checked?4:0)};
  }
  return payload;
}
$('ds4GyroPanel').addEventListener('input',()=>touchMouseDirty=true);
$('ds4GyroPanel').addEventListener('change',()=>touchMouseDirty=true);
'''
GYRO_HELPER = '''# SOOPAH_DS4_GYRO_V1
def save_pointer_settings(bluebridge,payload):
    aim=payload.get("ds4_aim")
    if aim is None:return bluebridge.request_json("TOUCH_MOUSE_SET",int(payload["enabled"]))
    if not bluebridge.request_json("HELLO").get("capabilities",{}).get("ds4_gyro"):
        raise ValueError("This firmware does not advertise DS4 gyro support")
    if not isinstance(aim,dict) or type(aim.get("enabled")) is not bool:
        raise ValueError("Invalid DS4 gyro settings")
    for name,low,high in [("sensitivity",1,200),("activation",0,2),("flags",0,7),("pause_button",0,255)]:
        value=aim.get(name)
        if type(value) is not int or value<low or value>high:raise ValueError("Invalid DS4 gyro "+name)
    if aim["pause_button"]>13 and aim["pause_button"]!=255:raise ValueError("Invalid DS4 pause button")
    return bluebridge.request_json("DS4_AIM_SET",int(aim["enabled"]),aim["sensitivity"],aim["activation"],aim["flags"],aim["pause_button"],int(payload["enabled"]))

'''

def patch_gyro(text):
    if GYRO_MARKER in text:raise ValueError('DS4 gyro integration is already installed')
    if MARKER not in text:text=patch_mouse(text)[0]
    match=re.search(r"BLUEBRIDGE_HTML = r'''(.*?)'''\.encode\(\"utf-8\"\)",text,re.S)
    if not match:raise ValueError('Cannot locate BlueBridge HTML')
    html=match.group(1)
    html=once(html,'<p id="touchMouseStatus"',GYRO_PANEL+'<p id="touchMouseStatus"')
    html=once(html,'  const data=statusData.touch_mouse;','  renderDS4Gyro();\n  const data=statusData.touch_mouse;')
    html=once(html,"!supported||!data||touchMouseSaving;","!supported||!data||touchMouseSaving||(statusData.capabilities?.ds4_gyro&&!statusData.ds4_aim);")
    html=once(html,"post('/api/bluebridge/touch-mouse',{enabled:$('touchMouseEnabled').checked})","post('/api/bluebridge/touch-mouse',pointerPayload())")
    html=once(html,"    touchMouseDirty=false;","    if(statusData.capabilities?.ds4_gyro)statusData.ds4_aim=pointerPayload().ds4_aim;\n    touchMouseDirty=false;")
    html=once(html,'</script>',GYRO_JS+'</script>')
    text=text[:match.start(1)]+html+text[match.end(1):]
    extra='''            if status["capabilities"].get("ds4_gyro"):
                try:
                    status["ds4_aim"] = self.request_json("DS4_AIM_GET")
                except Exception as gyro_error:
                    status["ds4_aim_error"] = str(gyro_error)
'''
    text=once(text,STATUS,STATUS+extra)
    text=once(text,'def handle_client(sock, address, path):',GYRO_HELPER+'def handle_client(sock, address, path):')
    text=once(text,'result = bluebridge.request_json("TOUCH_MOUSE_SET", int(payload["enabled"]))','result = save_pointer_settings(bluebridge,payload)')
    daemon=extract_daemon(text);ast.parse(daemon)
    return text,html,daemon

LIGHTBAR_MARKER='SOOPAH_LIGHTBAR_V1'
LIGHTBAR_PANEL='''<section id="lightbarPanel" class="panel hidden">
<h2>DS4 light-bar colours</h2>
<fieldset id="lightbarDefaults" disabled style="border:0;padding:0;margin:0">
<div class="form-group"><label for="lightbarTheme">Colour theme</label><select id="lightbarTheme" onchange="lightbarThemeChanged()"><option value="2">Nintendo — P1 red, P2 green</option><option value="1">Sega — P1 blue, P2 yellow/orange</option><option value="0">Custom</option></select></div>
<div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(120px,1fr));gap:12px">
<label>P1 colour <input id="lightbarP1" type="color" value="#400000" oninput="lightbarColourChanged(0)"></label>
<label>P2 colour <input id="lightbarP2" type="color" value="#004000" oninput="lightbarColourChanged(1)"></label>
<label>P3 colour <input id="lightbarP3" type="color" value="#004000" oninput="lightbarColourChanged(2)"></label>
<label>P4 colour <input id="lightbarP4" type="color" value="#200020" oninput="lightbarColourChanged(3)"></label></div>
<p class="subtle">Themes set P1/P2 and retain your P3/P4 choices. Auto follows BlueBridge connection slots, not MiSTer player assignment. Each adapter currently supports two connected controllers.</p>
<button id="lightbarSavePalette" class="btn primary" onclick="saveLightbarPalette()">Save theme and colours</button>
</fieldset>
<fieldset id="lightbarController" class="hidden" disabled style="border:1px solid var(--border);border-radius:12px;padding:16px;margin:16px 0">
<legend id="lightbarControllerName">Controller colour</legend>
<div class="form-group"><label for="lightbarPlayer">Preset</label><select id="lightbarPlayer" onchange="lightbarControllerDirty=true"><option value="0">Auto — BlueBridge slot</option><option value="1">P1 colour</option><option value="2">P2 colour</option><option value="3">P3 colour</option><option value="4">P4 colour</option></select></div>
<label><input id="lightbarCustom" type="checkbox" onchange="lightbarControllerDirty=true;renderLightbar()"> Override with a custom colour</label>
<label>Custom colour <input id="lightbarCustomColour" type="color" value="#400000" oninput="lightbarControllerDirty=true"></label>
<p id="lightbarControllerStatus" class="subtle"></p>
<button id="lightbarSaveController" class="btn" onclick="saveLightbarController()">Save controller colour</button>
</fieldset>
<p id="lightbarStatus" class="subtle" role="status">Connect an adapter to read colours.</p>
<p class="subtle">Connected DS4 light bars update without a restart. Other controllers are unaffected. Rumble retains the selected colour.</p>
</section>'''
LIGHTBAR_JS='''
// SOOPAH_LIGHTBAR_V1
let lightbarSaving=false,lightbarPaletteDirty=false,lightbarControllerDirty=false,lightbarAdapter='',lightbarControllerId='';
function lightbarHex(rgb){return '#'+rgb.map(v=>Number(v).toString(16).padStart(2,'0')).join('');}
function lightbarRGB(hex){if(!/^#[0-9a-f]{6}$/i.test(hex))throw Error('Choose a valid colour.');return [1,3,5].map(i=>parseInt(hex.slice(i,i+2),16));}
function lightbarThemeChanged(){
  const theme=Number($('lightbarTheme').value),colours={1:['#000040','#402000'],2:['#400000','#004000']}[theme];
  if(colours){$('lightbarP1').value=colours[0];$('lightbarP2').value=colours[1];}
  lightbarPaletteDirty=true;
}
function lightbarColourChanged(player){if(player<2)$('lightbarTheme').value='0';lightbarPaletteDirty=true;}
function renderLightbar(){
  if(lightbarAdapter!==selectedAdapterId){lightbarAdapter=selectedAdapterId;lightbarPaletteDirty=lightbarControllerDirty=false;lightbarControllerId='';}
  const supported=!!statusData.capabilities?.ds4_lightbar,data=statusData.lightbar;
  $('lightbarPanel').classList.toggle('hidden',!supported);
  $('lightbarDefaults').disabled=!supported||!data||lightbarSaving;
  if(!supported)return;
  if(!data){$('lightbarStatus').textContent='Unable to read light-bar settings. Refresh to retry.';$('lightbarController').classList.add('hidden');return;}
  if(!lightbarPaletteDirty&&!lightbarSaving){$('lightbarTheme').value=String(data.theme);data.players.forEach((rgb,i)=>$('lightbarP'+(i+1)).value=lightbarHex(rgb));}
  const c=selectedController(),choice=c&&data.controllers.find(x=>Number(x.index)===Number(c.index)&&Number(x.id)===Number(c.id));
  const key=c?String(c.id)+':'+c.index:'';
  if(key!==lightbarControllerId){lightbarControllerId=key;lightbarControllerDirty=false;}
  const ds4=Number(c?.kind)===4;
  $('lightbarController').classList.toggle('hidden',!ds4||!choice);
  $('lightbarController').disabled=lightbarSaving||!choice;
  if(ds4&&choice){
    $('lightbarControllerName').textContent=c.name+' colour';
    if(!lightbarControllerDirty&&!lightbarSaving){$('lightbarPlayer').value=String(choice.player);$('lightbarCustom').checked=!!choice.custom_enabled;$('lightbarCustomColour').value=lightbarHex(choice.rgb);}
    $('lightbarCustomColour').disabled=!$('lightbarCustom').checked||lightbarSaving;
    $('lightbarPlayer').disabled=$('lightbarCustom').checked||lightbarSaving;
    $('lightbarControllerStatus').textContent=c.connected?'Connected on BlueBridge slot '+(Number(c.slot)+1)+'.':'Saved choice will apply when this DS4 connects.';
  }
  $('lightbarStatus').textContent=lightbarPaletteDirty||lightbarControllerDirty?'Unsaved colour changes.':'Colours are saved on the adapter.';
}
async function saveLightbarPalette(){
  if(lightbarSaving)return;lightbarSaving=true;renderLightbar();
  try{
    const result=await post('/api/bluebridge/lightbar/palette',{theme:Number($('lightbarTheme').value),players:[1,2,3,4].map(i=>lightbarRGB($('lightbarP'+i).value))});
    statusData.lightbar=result.lightbar;lightbarPaletteDirty=false;toast('Theme and colours saved.');
  }catch(e){toast(e.message,true);}finally{lightbarSaving=false;renderLightbar();}
}
async function saveLightbarController(){
  if(lightbarSaving)return;const c=selectedController();if(!c||Number(c.kind)!==4)return;
  lightbarSaving=true;renderLightbar();
  try{
    const result=await post('/api/bluebridge/lightbar/controller',{index:Number(c.index),id:Number(c.id),player:Number($('lightbarPlayer').value),custom_enabled:$('lightbarCustom').checked,rgb:lightbarRGB($('lightbarCustomColour').value)});
    statusData.lightbar=result.lightbar;lightbarControllerDirty=false;toast('Controller colour saved.');
  }catch(e){toast(e.message,true);}finally{lightbarSaving=false;renderLightbar();}
}
saveLightbarPalette=guardAdapterAction(saveLightbarPalette);saveLightbarController=guardAdapterAction(saveLightbarController);
'''
LIGHTBAR_HELPER='''# SOOPAH_LIGHTBAR_V1
def lightbar_rgb(value):
    if not isinstance(value,list) or len(value)!=3 or any(type(v) is not int or v<0 or v>255 for v in value):
        raise ValueError("A colour must contain three whole numbers from 0 to 255")
    return value
def save_lightbar_palette(bluebridge,payload):
    if not isinstance(payload,dict):raise ValueError("Invalid colour settings")
    theme=payload.get("theme");players=payload.get("players")
    if type(theme) is not int or theme<0 or theme>2:raise ValueError("Invalid colour theme")
    if not isinstance(players,list) or len(players)!=4:raise ValueError("Four player colours are required")
    colours=[lightbar_rgb(rgb) for rgb in players]
    expected={1:[[0,0,64],[64,32,0]],2:[[64,0,0],[0,64,0]]}
    if theme and colours[:2]!=expected[theme]:raise ValueError("Theme colours changed; select Custom")
    bluebridge.request("LIGHTBAR_PALETTE_SET",theme,*[v for rgb in colours for v in rgb])
    return bluebridge.request_json("LIGHTBAR_GET")
def save_lightbar_controller(bluebridge,payload):
    if not isinstance(payload,dict):raise ValueError("Invalid controller colour settings")
    for name,maximum in [("index",7),("id",4294967295),("player",4)]:
        value=payload.get(name)
        if type(value) is not int or value<0 or value>maximum:raise ValueError("Invalid controller "+name)
    if type(payload.get("custom_enabled")) is not bool:raise ValueError("Invalid custom colour choice")
    rgb=lightbar_rgb(payload.get("rgb"))
    bluebridge.request("LIGHTBAR_SET",payload["index"],payload["id"],payload["player"],int(payload["custom_enabled"]),*rgb)
    return bluebridge.request_json("LIGHTBAR_GET")

'''
LIGHTBAR_ROUTE='''                if request_path in ("/api/bluebridge/lightbar/palette","/api/bluebridge/lightbar/controller"):
                    if method != "POST":
                        _json_http(sock,response(False,"error","POST required"),"405 Method Not Allowed")
                        return
                    try:
                        if not bluebridge.request_json("HELLO").get("capabilities",{}).get("ds4_lightbar"):
                            raise ValueError("Light-bar colours require firmware 1.1.5 or later")
                        payload=json.loads(request_body.decode("utf-8") or "{}")
                        result=save_lightbar_palette(bluebridge,payload) if request_path.endswith("/palette") else save_lightbar_controller(bluebridge,payload)
                        _json_http(sock,response(True,"bluebridge","Colours saved",lightbar=result))
                    except Exception as lightbar_error:
                        _json_http(sock,response(False,"error",str(lightbar_error)),"400 Bad Request")
                    return
'''

def patch(text):
    if LIGHTBAR_MARKER in text:raise ValueError('Light-bar integration is already installed')
    if GYRO_MARKER not in text:text=patch_gyro(text)[0]
    match=re.search(r"BLUEBRIDGE_HTML = r'''(.*?)'''\.encode\(\"utf-8\"\)",text,re.S)
    if not match:raise ValueError('Cannot locate BlueBridge HTML')
    html=match.group(1)
    html=once(html,'<section id="controllerCard"',LIGHTBAR_PANEL+'<section id="controllerCard"')
    html=once(html,'function renderAll(){','function renderAll(){renderLightbar();')
    html=once(html,'</script>',LIGHTBAR_JS+'</script>')
    text=text[:match.start(1)]+html+text[match.end(1):]
    extra='''            if status["capabilities"].get("ds4_lightbar"):
                try:
                    status["lightbar"] = self.request_json("LIGHTBAR_GET")
                except Exception as lightbar_error:
                    status["lightbar_error"] = str(lightbar_error)
'''
    text=once(text,STATUS,STATUS+extra)
    text=once(text,'def handle_client(sock, address, path):',LIGHTBAR_HELPER+'def handle_client(sock, address, path):')
    anchor='                if request_path == "/api/bluebridge/output":\n'
    text=once(text,anchor,LIGHTBAR_ROUTE+anchor)
    daemon=extract_daemon(text);ast.parse(daemon)
    return text,html,daemon

def extract_daemon(text):
    if "<<'PYEOF'\n" in text:
        return text.split("<<'PYEOF'\n",1)[1].split('\nPYEOF',1)[0]
    return text

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--preview',type=Path,help='Validate a local companion_remote.sh and write preview files only')
    parser.add_argument('--install',action='store_true',help='Back up and patch both MiSTer source files; restart separately')
    parser.add_argument('--restore',metavar='TIMESTAMP',help='Restore the two backups from this installation')
    args=parser.parse_args()
    if args.preview:
        text=args.preview.read_text(encoding='utf-8')
        patched,html,daemon=patch(text)
        args.preview.with_name('companion_remote.mouse.sh').write_text(patched,encoding='utf-8',newline='\n')
        args.preview.with_name('bluebridge-mouse-preview.html').write_text(html,encoding='utf-8')
        args.preview.with_name('companion_remote.mouse.py').write_text(daemon,encoding='utf-8')
        print('Source and daemon syntax validated; preview written. Nothing installed.')
    elif args.install:
        install(TARGETS)
        print('Run: sh /media/fat/Scripts/companion_remote.sh restart --unattended')
        print('Then refresh /bluebridge. A future upstream Remote update may replace this patch.')
    elif args.restore:restore(args.restore)
    else:parser.error('Choose --preview FILE, --install or --restore TIMESTAMP')

if __name__=='__main__':main()
