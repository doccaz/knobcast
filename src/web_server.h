#pragma once
// web_server.h
// Captive-portal-style web UI for KnobCast.
// - AP mode when no WiFi configured (fallback)
// - Config: WiFi, Chromecast IP, volume step
// - Control: play/pause, mute, volume
// - Status: connection, volume, playing, discovered devices

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include "config.h"
#include "chromecast_client.h"
#include "debug_log.h"

class CastWebServer {
public:
    void begin(ConfigStore* config, ChromecastClient* cast) {
        _config = config;
        _cast = cast;
        _server.on("/",           HTTP_GET,  [this]() { _handleRoot(); });
        _server.on("/save",       HTTP_POST, [this]() { _handleSave(); });
        _server.on("/status",     HTTP_GET,  [this]() { _handleStatus(); });
        _server.on("/control",    HTTP_POST, [this]() { _handleControl(); });
        _server.on("/scan",       HTTP_GET,  [this]() { _handleScan(); });
        _server.on("/connectcast", HTTP_POST, [this]() { _handleConnectCast(); });
        _server.on("/wifiscan",   HTTP_GET,  [this]() { _handleWifiScan(); });
        _server.on("/testwifi",  HTTP_POST, [this]() { _handleTestWifi(); });
        _server.on("/reset",      HTTP_POST, [this]() { _handleReset(); });
        _server.on("/log",        HTTP_GET,  [this]() { _handleLog(); });
        _server.on("/debug",      HTTP_POST, [this]() { _handleDebugToggle(); });
        _server.onNotFound(                  [this]() { _handleRoot(); }); // captive portal
        _server.begin();
    }

    // Start AP mode with captive portal DNS
    void startAP() {
        uint8_t mac[6];
        WiFi.macAddress(mac);
        snprintf(_apSsid, sizeof(_apSsid), "KnobCast-%02X%02X", mac[4], mac[5]);

        WiFi.mode(WIFI_AP);
        WiFi.softAP(_apSsid);
        delay(100);
        _apIp = WiFi.softAPIP();

        _dns.start(53, "*", _apIp);  // redirect all DNS to us
        _apMode = true;

        Serial.printf("[Web] AP mode: %s  IP: %s\n", _apSsid, _apIp.toString().c_str());
    }

    void loop() {
        if (_apMode) _dns.processNextRequest();
        _server.handleClient();
    }

    bool isAPMode() const { return _apMode; }
    const char* getAPSsid() const { return _apSsid; }
    IPAddress getAPIP() const { return _apIp; }

    // Set by /connectcast — main loop should transition to READY
    bool consumeConnectEvent() {
        if (_castConnected) { _castConnected = false; return true; }
        return false;
    }

private:
    WebServer _server{80};
    DNSServer _dns;
    ConfigStore* _config = nullptr;
    ChromecastClient* _cast = nullptr;
    bool _apMode = false;
    bool _castConnected = false;
    char _apSsid[32] = {};
    IPAddress _apIp;

    // ── JSON string escaping ─────────────────────────────────────────────
    static String _jsonEsc(const char* s) {
        String out;
        out.reserve(strlen(s) + 8);
        while (*s) {
            switch (*s) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:   out += *s;     break;
            }
            s++;
        }
        return out;
    }
    static String _jsonEsc(const String& s) { return _jsonEsc(s.c_str()); }

    // ── HTML page (embedded PROGMEM) ─────────────────────────────────────
    void _handleRoot() {
        String html = F(R"rawhtml(<!DOCTYPE html>
<html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<link rel="icon" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'><defs><radialGradient id='g1' cx='40%25' cy='35%25'><stop offset='0%25' stop-color='%23e8e8e8'/><stop offset='100%25' stop-color='%23888'/></radialGradient><radialGradient id='g2' cx='45%25' cy='40%25'><stop offset='0%25' stop-color='%23aaa'/><stop offset='100%25' stop-color='%23555'/></radialGradient></defs><circle cx='50' cy='50' r='46' fill='%23333'/><circle cx='50' cy='50' r='42' fill='url(%23g1)' stroke='%23666' stroke-width='1.5'/><circle cx='50' cy='50' r='34' fill='url(%23g2)' stroke='%23777' stroke-width='1'/><g stroke='%23999' stroke-width='1.5'><line x1='50' y1='4' x2='50' y2='12'/><line x1='82.3' y1='17.7' x2='76.6' y2='23.4' /><line x1='96' y1='50' x2='88' y2='50'/><line x1='82.3' y1='82.3' x2='76.6' y2='76.6'/><line x1='50' y1='96' x2='50' y2='88'/><line x1='17.7' y1='82.3' x2='23.4' y2='76.6'/><line x1='4' y1='50' x2='12' y2='50'/><line x1='17.7' y1='17.7' x2='23.4' y2='23.4'/></g><line x1='50' y1='48' x2='50' y2='22' stroke='%23fff' stroke-width='4' stroke-linecap='round'/><circle cx='50' cy='50' r='6' fill='%23444' stroke='%23333' stroke-width='1'/></svg>">
<title>KnobCast</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,sans-serif;background:#1a1a2e;color:#e0e0e0;padding:16px;max-width:480px;margin:0 auto}
h1{color:#e94560;margin-bottom:12px;font-size:1.4em}
h2{color:#0f3460;background:#e0e0e0;padding:6px 10px;margin:16px -16px 10px;font-size:1em}
label{display:block;margin:6px 0 2px;font-size:.9em;color:#aaa}
input[type=text],input[type=password],input[type=number]{width:100%;padding:8px;border:1px solid #333;border-radius:4px;background:#16213e;color:#e0e0e0;font-size:1em}
button,.btn{display:inline-block;padding:10px 18px;border:none;border-radius:4px;font-size:1em;cursor:pointer;margin:4px 2px;color:#fff}
.btn-primary{background:#e94560}
.btn-ctrl{background:#0f3460;min-width:60px}
.btn-danger{background:#833}
.btn:disabled{opacity:.4}
.status-box{background:#16213e;border:1px solid #333;border-radius:6px;padding:10px;margin:8px 0}
.vol-row{display:flex;align-items:center;gap:8px}
.vol-row input[type=range]{flex:1}
.vol-val{min-width:36px;text-align:right}
.device-list{list-style:none;padding:0}
.device-list li{padding:6px 8px;border-bottom:1px solid #333;display:flex;justify-content:space-between;align-items:center}
.device-list li button{padding:4px 10px;font-size:.85em}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px}
.dot-on{background:#0f6}
.dot-off{background:#f33}
.ctrl-row{display:flex;justify-content:center;align-items:center;gap:6px;margin:10px 0;flex-wrap:wrap}
.ctrl-row .btn{min-width:44px;min-height:44px;font-size:1.3em;padding:6px 10px;display:flex;align-items:center;justify-content:center}
.ctrl-row .btn-lg{min-width:56px;min-height:56px;font-size:1.6em;border-radius:50%}
.vol-section{margin:10px 0}
.seek-row{display:flex;align-items:center;gap:6px;margin:6px 0;font-size:.85em;color:#aaa}
.seek-row input[type=range]{flex:1}
.seek-time{min-width:38px;text-align:center;font-variant-numeric:tabular-nums}
#toast{position:fixed;bottom:20px;left:50%;transform:translateX(-50%);background:#333;color:#fff;padding:8px 20px;border-radius:20px;display:none;font-size:.9em;z-index:99}
.pass-wrapper{position:relative;display:flex;align-items:center;flex:1}
.pass-wrapper input{padding-right:36px!important}
.eye-btn{position:absolute;right:8px;background:none;border:none;color:#aaa;cursor:pointer;padding:4px;font-size:1.2em;display:flex;align-items:center;justify-content:center}
.eye-btn:hover{color:#e94560}
.badge-conn{background:#0f6;color:#000;padding:2px 6px;border-radius:10px;font-size:0.75em;font-weight:bold;margin-left:8px}
</style>
</head><body>
<h1><svg width="28" height="28" viewBox="0 0 100 100" style="vertical-align:middle;margin-right:6px"><defs><radialGradient id="kg1" cx="40%" cy="35%"><stop offset="0%" stop-color="#e8e8e8"/><stop offset="100%" stop-color="#888"/></radialGradient><radialGradient id="kg2" cx="45%" cy="40%"><stop offset="0%" stop-color="#aaa"/><stop offset="100%" stop-color="#555"/></radialGradient></defs><circle cx="50" cy="50" r="46" fill="#333"/><circle cx="50" cy="50" r="42" fill="url(#kg1)" stroke="#666" stroke-width="1.5"/><circle cx="50" cy="50" r="34" fill="url(#kg2)" stroke="#777" stroke-width="1"/><g stroke="#999" stroke-width="1.5"><line x1="50" y1="4" x2="50" y2="12"/><line x1="82.3" y1="17.7" x2="76.6" y2="23.4"/><line x1="96" y1="50" x2="88" y2="50"/><line x1="82.3" y1="82.3" x2="76.6" y2="76.6"/><line x1="50" y1="96" x2="50" y2="88"/><line x1="17.7" y1="82.3" x2="23.4" y2="76.6"/><line x1="4" y1="50" x2="12" y2="50"/><line x1="17.7" y1="17.7" x2="23.4" y2="23.4"/></g><line x1="50" y1="48" x2="50" y2="22" stroke="#fff" stroke-width="4" stroke-linecap="round"/><circle cx="50" cy="50" r="6" fill="#444" stroke="#333" stroke-width="1"/></svg>KnobCast</h1>

<h2>&#x2699; WiFi Configuration</h2>
<form id="cfgForm">
<label>SSID <button type="button" class="btn btn-ctrl" style="padding:4px 10px;font-size:.85em" onclick="doWifiScan()">Scan WiFi</button></label>
<input type="text" name="ssid" id="ssid" value="" maxlength="63">
<ul class="device-list" id="wifiList" style="display:none"></ul>
<label>Password</label>
<div style="display:flex;gap:4px">
  <div class="pass-wrapper">
    <input type="password" name="pass" id="pass" value="" maxlength="63">
    <button type="button" class="eye-btn" id="togglePass" onclick="togglePassVis()" title="Toggle visibility">&#x1f441;</button>
  </div>
</div>
<label>Chromecast IP (blank = auto-discover)</label>
<input type="text" name="castIp" id="castIp" value="" maxlength="39" placeholder="e.g. 192.168.1.42">
<label>Volume step (%)</label>
<input type="number" name="volStep" id="volStep" value="2" min="1" max="20" step="1">
<label>Menu Timeout</label>
<select name="menuTimeout" id="menuTimeout">
  <option value="5">5 seconds</option>
  <option value="15" selected>15 seconds</option>
  <option value="30">30 seconds</option>
  <option value="60">60 seconds</option>
</select>
<label>Screen Timeout (burn-in protection)</label>
<select name="screenTimeout" id="screenTimeout">
  <option value="30">30 seconds</option>
  <option value="60">1 minute</option>
  <option value="300">5 minutes</option>
  <option value="600" selected>10 minutes</option>
  <option value="0">Never</option>
</select>
<label>Progress Bar</label>
<select name="barMode" id="barMode">
  <option value="0" selected>Volume</option>
  <option value="1">Elapsed time</option>
</select>
<label style="display:flex;align-items:center;gap:6px;margin-top:8px;cursor:pointer"><input type="checkbox" name="scanOnBoot" id="scanOnBoot" checked> Scan for devices on boot</label>
<label style="display:flex;align-items:center;gap:6px;margin-top:4px;cursor:pointer"><input type="checkbox" name="autoConnect" id="autoConnect" checked> Auto-connect to last device</label>
<br><br>
<button type="button" class="btn btn-ctrl" onclick="doTestWifi()">Test Connection</button>
<button type="submit" class="btn btn-primary">Save</button>
<button type="button" class="btn btn-danger" onclick="doReset()">Factory Reset</button>
</form>

<h2>&#x1f3b5; Now Playing</h2>
<div class="status-box" id="statusBox">
  <p><span class="dot dot-off" id="connDot"></span> <span id="connText">Disconnected</span></p>
  <p>Device: <strong id="devName">&mdash;</strong></p>
  <p>App: <strong id="appName">&mdash;</strong> &nbsp; <strong id="playState"></strong></p>

  <div class="seek-row">
    <span class="seek-time" id="curTime">0:00</span>
    <input type="range" id="seekBar" min="0" max="100" value="0" step="1">
    <span class="seek-time" id="durTime">0:00</span>
  </div>

  <div class="ctrl-row">
    <button class="btn btn-ctrl" onclick="doCmd('prev')" title="Previous">&#x23EE;&#xFE0E;</button>
    <button class="btn btn-ctrl" id="btnStop" onclick="doCmd('stop')" title="Stop" style="display:none">&#x23F9;&#xFE0E;</button>
    <button class="btn btn-ctrl btn-lg" id="btnPlay" onclick="togglePlay()" title="Play/Pause">&#x25B6;&#xFE0E;</button>
    <button class="btn btn-ctrl" id="btnMute" onclick="toggleMute()" title="Mute/Unmute">&#x1f50a;</button>
    <button class="btn btn-ctrl" onclick="doCmd('next')" title="Next">&#x23ED;&#xFE0E;</button>
  </div>

  <div class="vol-section">
    <div class="vol-row">
      <button class="btn btn-ctrl" onclick="doCmd('voldown')" style="min-width:36px;padding:4px 8px">&#x2796;&#xFE0E;</button>
      <input type="range" id="volSlider" min="0" max="100" value="50">
      <button class="btn btn-ctrl" onclick="doCmd('volup')" style="min-width:36px;padding:4px 8px">&#x2795;&#xFE0E;</button>
      <span class="vol-val" id="volVal">50%</span>
    </div>
  </div>

  <p style="text-align:center;margin-top:6px"><button class="btn btn-danger" style="padding:4px 14px;font-size:.85em" onclick="doCmd('disconnect')">Disconnect</button></p>
</div>

<h2>&#x1f4e1; Discovered Devices</h2>
<button class="btn btn-primary" onclick="doScan()">Scan Network</button>
<ul class="device-list" id="deviceList"><li>Press Scan to search</li></ul>

<h2>&#x1f41b; Debug Log</h2>
<div>
<button class="btn btn-ctrl" id="btnDebug" onclick="toggleDebug()">Enable</button>
<button class="btn btn-ctrl" onclick="clearLog()">Clear</button>
<button class="btn btn-ctrl" onclick="refreshLog()">Refresh</button>
<label style="display:inline-flex;align-items:center;margin-left:10px;cursor:pointer;color:#e0e0e0"><input type="checkbox" id="logFollow" checked style="margin-right:5px"> Follow</label>
</div>
<pre id="logBox" style="background:#0d1117;color:#8b949e;padding:8px;border-radius:4px;font-size:.75em;max-height:300px;overflow-y:auto;white-space:pre-wrap;word-break:break-all;display:none;margin-top:8px"></pre>

<h2>&#x2139; About</h2>
<div class="status-box" style="text-align:center">
  <p style="font-size:1.3em;color:#e94560;margin-bottom:6px"><strong><svg width="22" height="22" viewBox="0 0 100 100" style="vertical-align:middle;margin-right:4px"><defs><radialGradient id="ag1" cx="40%" cy="35%"><stop offset="0%" stop-color="#e8e8e8"/><stop offset="100%" stop-color="#888"/></radialGradient><radialGradient id="ag2" cx="45%" cy="40%"><stop offset="0%" stop-color="#aaa"/><stop offset="100%" stop-color="#555"/></radialGradient></defs><circle cx="50" cy="50" r="46" fill="#333"/><circle cx="50" cy="50" r="42" fill="url(#ag1)" stroke="#666" stroke-width="1.5"/><circle cx="50" cy="50" r="34" fill="url(#ag2)" stroke="#777" stroke-width="1"/><g stroke="#999" stroke-width="1.5"><line x1="50" y1="4" x2="50" y2="12"/><line x1="82.3" y1="17.7" x2="76.6" y2="23.4"/><line x1="96" y1="50" x2="88" y2="50"/><line x1="82.3" y1="82.3" x2="76.6" y2="76.6"/><line x1="50" y1="96" x2="50" y2="88"/><line x1="17.7" y1="82.3" x2="23.4" y2="76.6"/><line x1="4" y1="50" x2="12" y2="50"/><line x1="17.7" y1="17.7" x2="23.4" y2="23.4"/></g><line x1="50" y1="48" x2="50" y2="22" stroke="#fff" stroke-width="4" stroke-linecap="round"/><circle cx="50" cy="50" r="6" fill="#444" stroke="#333" stroke-width="1"/></svg>KnobCast</strong></p>
  <p>Firmware <strong>v1.0.6</strong></p>
  <p style="margin:8px 0">A physical Chromecast remote built with<br>ESP32-C3 + KY-040 encoder + 72×40 OLED</p>
  <p style="color:#aaa;font-size:.85em">Created by <strong style="color:#e0e0e0">Erico Mendonca</strong></p>
  <p style="margin-top:8px"><a href="https://github.com/doccaz/knobcast" target="_blank" rel="noopener" style="color:#e94560;text-decoration:none">&#x1f517; github.com/doccaz/knobcast</a></p>
</div>

<div id="toast"></div>

<script>
var _playing=false,_muted=false,_dbgEnabled=false,_dbgTimer=null,_curHost='',_curPort=0;
function togglePassVis(){var p=document.getElementById('pass');var b=document.getElementById('togglePass');if(p.type==='password'){p.type='text';b.innerHTML='&#x1f5d9;'}else{p.type='password';b.innerHTML='&#x1f441;'}}
function toggleDebug(){var a=_dbgEnabled?'off':'on';fetch('/debug',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'action='+a}).then(r=>r.json()).then(j=>{_dbgEnabled=j.enabled;document.getElementById('btnDebug').textContent=_dbgEnabled?'Disable':'Enable';document.getElementById('logBox').style.display=_dbgEnabled?'block':'none';if(_dbgEnabled){refreshLog();_dbgTimer=setInterval(refreshLog,2000)}else{if(_dbgTimer)clearInterval(_dbgTimer);_dbgTimer=null}})}
function clearLog(){fetch('/debug',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'action=clear'}).then(()=>refreshLog())}
function refreshLog(){fetch('/log').then(r=>r.json()).then(j=>{_dbgEnabled=j.enabled;document.getElementById('btnDebug').textContent=_dbgEnabled?'Disable':'Enable';var box=document.getElementById('logBox');box.style.display=_dbgEnabled?'block':'none';if(j.lines&&j.lines.length>0){box.textContent=j.lines.join('\n');if(document.getElementById('logFollow').checked)box.scrollTop=box.scrollHeight}else{box.textContent='(no log entries)'}})}
function toast(msg){var t=document.getElementById('toast');t.textContent=msg;t.style.display='block';setTimeout(()=>t.style.display='none',2000)}
function fmtTime(s){s=Math.round(s);var m=Math.floor(s/60);s=s%60;return m+':'+(s<10?'0':'')+s}
function doCmd(cmd){fetch('/control',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'cmd='+cmd}).then(r=>r.json()).then(j=>{if(!j.ok&&cmd!=='disconnect')toast('Error');pollStatus()}).catch(()=>toast('Error'))}
function togglePlay(){doCmd(_playing?'pause':'play')}
function toggleMute(){doCmd(_muted?'unmute':'mute')}
function doScan(){document.getElementById('deviceList').innerHTML='<li>Scanning...</li>';fetch('/scan').then(r=>r.json()).then(j=>{renderDevices(j.devices)}).catch(()=>toast('Scan failed'))}
function renderDevices(devices){var ul=document.getElementById('deviceList');ul.innerHTML='';if(!devices||devices.length===0){ul.innerHTML='<li>No devices found</li>';return}devices.forEach(d=>{var li=document.createElement('li');var isConn=(d.ip===_curHost && d.port===_curPort);var btnHtml=isConn?'<span class="badge-conn">CONNECTED</span>':'<button class="btn btn-ctrl" onclick="connectDev(\''+d.ip+'\','+d.port+',\''+d.name.replace(/'/g,"\\'")+'\')">Connect</button>';li.innerHTML='<span>'+d.name+' <small>('+d.ip+':'+d.port+')</small></span> <span>'+btnHtml+' <button class="btn btn-ctrl" onclick="document.getElementById(\'castIp\').value=\''+d.ip+'\';toast(\'IP saved to config\')">Save IP</button></span>';ul.appendChild(li)})}
function connectDev(ip,port,name){toast('Connecting to '+name+'...');fetch('/connectcast',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ip='+encodeURIComponent(ip)+'&port='+port+'&name='+encodeURIComponent(name)}).then(r=>r.json()).then(j=>{toast(j.msg);pollStatus()}).catch(()=>toast('Failed'))}
function pollStatus(){fetch('/status').then(r=>r.json()).then(j=>{_playing=j.playing;_muted=j.muted;_curHost=j.host;_curPort=j.port;document.getElementById('connDot').className='dot '+(j.connected?'dot-on':'dot-off');document.getElementById('connText').textContent=j.connected?'Connected to '+j.host:'Disconnected';document.getElementById('devName').textContent=j.deviceName||'\u2014';document.getElementById('appName').textContent=j.app||'\u2014';document.getElementById('playState').textContent=j.playing?'\u25B6 Playing':'\u23F8 Idle';document.getElementById('btnPlay').innerHTML=j.playing?'\u23F8\uFE0E':'\u25B6\uFE0E';document.getElementById('btnPlay').title=j.playing?'Pause':'Play';document.getElementById('btnStop').style.display=j.playing?'':'none';document.getElementById('btnMute').innerHTML=j.muted?'\uD83D\uDD07':'\uD83D\uDD0A';document.getElementById('volSlider').value=Math.round(j.volume*100);document.getElementById('volVal').textContent=Math.round(j.volume*100)+'%';document.getElementById('curTime').textContent=fmtTime(j.currentTime||0);document.getElementById('durTime').textContent=fmtTime(j.duration||0);var sb=document.getElementById('seekBar');if(j.duration>0){sb.max=Math.round(j.duration);sb.value=Math.round(j.currentTime||0)}if(j.devices)renderDevices(j.devices)}).catch(e=>console.error('pollStatus:',e))}
document.getElementById('volSlider').addEventListener('change',function(){doCmd('setvol:'+this.value)});
document.getElementById('seekBar').addEventListener('change',function(){doCmd('seek:'+this.value)});
document.getElementById('cfgForm').addEventListener('submit',function(e){e.preventDefault();var fd=new FormData(this);fetch('/save',{method:'POST',body:new URLSearchParams(fd)}).then(r=>r.json()).then(j=>{if(!j.ok){toast('Error')}else if(j.reboot){toast('Saved! Rebooting...');setTimeout(()=>location.reload(),5000)}else{toast('Saved!');pollStatus()}}).catch(()=>toast('Error'))});
function doTestWifi(){var s=document.getElementById('ssid').value;var p=document.getElementById('pass').value;if(!s){toast('Enter SSID first');return}toast('Testing...');fetch('/testwifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p)}).then(r=>r.json()).then(j=>{toast(j.msg)}).catch(()=>toast('Test failed'))}
function doWifiScan(){var ul=document.getElementById('wifiList');ul.style.display='block';ul.innerHTML='<li>Scanning...</li>';fetch('/wifiscan').then(r=>r.json()).then(j=>{ul.innerHTML='';if(!j.networks||j.networks.length===0){ul.innerHTML='<li>No networks found</li>';return}j.networks.forEach(n=>{var li=document.createElement('li');var lock=n.open?'':'&#x1f512; ';var bars=n.rssi>-50?'&#x2588;&#x2588;&#x2588;':n.rssi>-70?'&#x2588;&#x2588;':'&#x2588;';li.innerHTML=lock+n.ssid+' <small>'+bars+' '+n.rssi+'dBm</small> <button class="btn btn-ctrl" onclick="document.getElementById(\'ssid\').value=\''+n.ssid.replace(/'/g,"\\'")+'\';toast(\'SSID set\')">Use</button>';ul.appendChild(li)})}).catch(()=>{ul.innerHTML='<li>Scan failed</li>'})}
function doReset(){if(confirm('Factory reset? All settings will be erased.')){fetch('/reset',{method:'POST'}).then(()=>{toast('Reset! Rebooting...');setTimeout(()=>location.reload(),5000)})}}
setInterval(pollStatus,3000);
window.addEventListener('load',function(){fetch('/status').then(r=>r.json()).then(j=>{document.getElementById('ssid').value=j.ssid||'';document.getElementById('pass').value=j.wifiPass||'';document.getElementById('castIp').value=j.castIp||'';document.getElementById('volStep').value=Math.round((j.volStep||0.02)*100);if(j.menuTimeout){var ms=document.getElementById('menuTimeout');if(ms)ms.value=j.menuTimeout}if(j.screenTimeout!==undefined){var ss=document.getElementById('screenTimeout');if(ss)ss.value=j.screenTimeout}if(j.barMode!==undefined){document.getElementById('barMode').value=j.barMode}if(j.scanOnBoot!==undefined){document.getElementById('scanOnBoot').checked=j.scanOnBoot}if(j.autoConnect!==undefined){document.getElementById('autoConnect').checked=j.autoConnect}pollStatus()}).catch(e=>console.error('load:',e))});
</script>
</body></html>)rawhtml");
        _server.send(200, "text/html", html);
    }

    // ── Save config ──────────────────────────────────────────────────────
    void _handleSave() {
        String ssid    = _server.arg("ssid");
        String pass    = _server.arg("pass");
        String castIp  = _server.arg("castIp");
        String volStep = _server.arg("volStep");

        // Check if WiFi credentials changed — only reboot if so
        bool wifiChanged = (strcmp(_config->cfg.wifiSsid, ssid.c_str()) != 0 ||
                            strcmp(_config->cfg.wifiPass, pass.c_str()) != 0);

        strncpy(_config->cfg.wifiSsid, ssid.c_str(), sizeof(_config->cfg.wifiSsid) - 1);
        strncpy(_config->cfg.wifiPass, pass.c_str(), sizeof(_config->cfg.wifiPass) - 1);
        strncpy(_config->cfg.castIp,   castIp.c_str(), sizeof(_config->cfg.castIp) - 1);

        int pct = volStep.toInt();
        if (pct < 1) pct = 1;
        if (pct > 20) pct = 20;
        _config->cfg.volumeStep = pct / 100.0f;

        String menuTimeout  = _server.arg("menuTimeout");
        String screenTimeout = _server.arg("screenTimeout");
        int mt = menuTimeout.toInt();
        int st = screenTimeout.toInt();
        if (mt > 0) _config->cfg.menuTimeout = mt;
        if (st >= 0) _config->cfg.screenTimeout = st;

        String barMode = _server.arg("barMode");
        _config->cfg.barMode = barMode.toInt();

        _config->cfg.scanOnBoot = _server.hasArg("scanOnBoot");
        _config->cfg.autoConnect = _server.hasArg("autoConnect");

        _config->cfg.configured = true;
        _config->save();

        if (wifiChanged) {
            _server.send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
            delay(1000);
            ESP.restart();
        } else {
            _server.send(200, "application/json", "{\"ok\":true,\"reboot\":false}");
        }
    }

    // ── Status JSON ──────────────────────────────────────────────────────
    void _handleStatus() {
        String json = "{";
        json += "\"connected\":" + String(_cast->isConnected() ? "true" : "false");
        json += ",\"volume\":" + String(_cast->getVolume(), 2);
        json += ",\"muted\":" + String(_cast->isMuted() ? "true" : "false");
        json += ",\"playing\":" + String(_cast->isPlaying() ? "true" : "false");
        json += ",\"app\":\"" + _jsonEsc(_cast->getAppName()) + "\"";
        json += ",\"deviceName\":\"" + _jsonEsc(_cast->getFriendlyName()) + "\"";
        json += ",\"currentTime\":" + String(_cast->getCurrentTime(), 1);
        json += ",\"duration\":" + String(_cast->getDuration(), 1);
        json += ",\"host\":\"" + _jsonEsc(_cast->getHost()) + "\"";
        json += ",\"ssid\":\"" + _jsonEsc(_config->cfg.wifiSsid) + "\"";
        json += ",\"castIp\":\"" + _jsonEsc(_config->cfg.castIp) + "\"";
        json += ",\"wifiPass\":\"" + _jsonEsc(_config->cfg.wifiPass) + "\"";
        json += ",\"volStep\":" + String(_config->cfg.volumeStep, 2);
        json += ",\"menuTimeout\":" + String(_config->cfg.menuTimeout);
        json += ",\"screenTimeout\":" + String(_config->cfg.screenTimeout);
        json += ",\"barMode\":" + String(_config->cfg.barMode);
        json += ",\"scanOnBoot\":" + String(_config->cfg.scanOnBoot ? "true" : "false");
        json += ",\"autoConnect\":" + String(_config->cfg.autoConnect ? "true" : "false");
        json += ",\"port\":" + String(_cast->isConnected() ? _cast->getPort() : 0);
        
        // Include discovered devices for immediate sync
        json += ",\"devices\":[";
        for (int i = 0; i < _cast->deviceCount; i++) {
            if (i > 0) json += ",";
            json += "{\"name\":\"" + _jsonEsc(_cast->devices[i].name) + "\"";
            json += ",\"ip\":\"" + _jsonEsc(_cast->devices[i].ip) + "\"";
            json += ",\"port\":" + String(_cast->devices[i].port) + "}";
        }
        json += "]";

        json += "}";
        _server.send(200, "application/json", json);
    }

    // ── Control commands ─────────────────────────────────────────────────
    void _handleControl() {
        String cmd = _server.arg("cmd");
        bool ok = false;

        if (cmd == "play")        ok = _cast->play();
        else if (cmd == "pause")  ok = _cast->pause();
        else if (cmd == "stop")   ok = _cast->stop();
        else if (cmd == "next")   ok = _cast->next();
        else if (cmd == "prev")   ok = _cast->previous();
        else if (cmd == "mute")   ok = _cast->setMute(true);
        else if (cmd == "unmute") ok = _cast->setMute(false);
        else if (cmd == "disconnect") { _cast->disconnect(); ok = true; }
        else if (cmd == "volup") {
            float v = _cast->getVolume() + _config->cfg.volumeStep;
            if (v > 1.0f) v = 1.0f;
            ok = _cast->setVolume(v);
        }
        else if (cmd == "voldown") {
            float v = _cast->getVolume() - _config->cfg.volumeStep;
            if (v < 0.0f) v = 0.0f;
            ok = _cast->setVolume(v);
        }
        else if (cmd.startsWith("setvol:")) {
            int pct = cmd.substring(7).toInt();
            float v = pct / 100.0f;
            if (v < 0) v = 0;
            if (v > 1) v = 1;
            ok = _cast->setVolume(v);
        }
        else if (cmd.startsWith("seek:")) {
            float pos = cmd.substring(5).toFloat();
            ok = _cast->seek(pos);
        }

        _server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
    }

    // ── Connect to a specific Chromecast by IP ─────────────────────────
    void _handleConnectCast() {
        String ip   = _server.arg("ip");
        String portStr = _server.arg("port");
        String name = _server.arg("name");
        
        uint16_t port = portStr.length() > 0 ? portStr.toInt() : 8009;

        if (ip.length() == 0) {
            _server.send(200, "application/json", "{\"ok\":false,\"msg\":\"No IP\"}");
            return;
        }

        // Disconnect from current device if connected
        if (_cast->isConnected()) _cast->disconnect();

        if (_cast->connect(ip.c_str(), port)) {
            if (name.length() > 0) _cast->setFriendlyName(name.c_str());
            _config->saveLastDevice(ip.c_str(), name.c_str(), port);
            _castConnected = true;
            _server.send(200, "application/json", "{\"ok\":true,\"msg\":\"Connected\"}");
        } else {
            _server.send(200, "application/json", "{\"ok\":false,\"msg\":\"Connection failed\"}");
        }
    }

    // ── Scan for Chromecast devices ──────────────────────────────────────
    void _handleScan() {
        int n = _cast->discoverAll(8000);
        String json = "{\"devices\":[";
        for (int i = 0; i < n; i++) {
            if (i > 0) json += ",";
            json += "{\"name\":\"" + _jsonEsc(_cast->devices[i].name) + "\"";
            json += ",\"ip\":\"" + _jsonEsc(_cast->devices[i].ip) + "\"";
            json += ",\"port\":" + String(_cast->devices[i].port) + "}";
        }
        json += "]}";
        _server.send(200, "application/json", json);
    }

    // ── Test WiFi connection without saving ─────────────────────────────
    void _handleTestWifi() {
        String ssid = _server.arg("ssid");
        String pass = _server.arg("pass");
        if (ssid.length() == 0) {
            _server.send(200, "application/json", "{\"ok\":false,\"msg\":\"SSID empty\"}");
            return;
        }

        // If we're in AP mode, briefly switch to STA+AP to test
        wifi_mode_t prevMode = WiFi.getMode();
        if (prevMode == WIFI_AP) {
            WiFi.mode(WIFI_AP_STA);
        }

        WiFi.begin(ssid.c_str(), pass.c_str());
        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED) {
            if (millis() - start > 10000) {
                WiFi.disconnect(true);
                if (prevMode == WIFI_AP) WiFi.mode(WIFI_AP);
                _server.send(200, "application/json",
                    "{\"ok\":false,\"msg\":\"Connection timeout\"}");
                return;
            }
            delay(200);
        }

        String ip = WiFi.localIP().toString();
        WiFi.disconnect(true);
        if (prevMode == WIFI_AP) WiFi.mode(WIFI_AP);

        _server.send(200, "application/json",
            "{\"ok\":true,\"msg\":\"Connected! IP: " + ip + "\"}");
    }

    // ── Scan for WiFi networks ──────────────────────────────────────────
    void _handleWifiScan() {
        int n = WiFi.scanNetworks();
        String json = "{\"networks\":[";
        for (int i = 0; i < n && i < 20; i++) {
            if (i > 0) json += ",";
            json += "{\"ssid\":\"";
            // Escape any quotes in SSID
            String ssid = WiFi.SSID(i);
            ssid.replace("\"", "\\\"");
            json += ssid;
            json += "\",\"rssi\":" + String(WiFi.RSSI(i));
            json += ",\"open\":" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false");
            json += "}";
        }
        json += "]}";
        WiFi.scanDelete();
        _server.send(200, "application/json", json);
    }

    // ── Factory reset ────────────────────────────────────────────────────
    void _handleReset() {
        _config->reset();
        _server.send(200, "application/json", "{\"ok\":true}");
        delay(1000);
        ESP.restart();
    }

    // ── Debug log ───────────────────────────────────────────────────────
    void _handleLog() {
        String json = "{\"enabled\":";
        json += dbgLog.enabled ? "true" : "false";
        json += ",\"lines\":";
        json += dbgLog.toJson();
        json += "}";
        _server.send(200, "application/json", json);
    }

    void _handleDebugToggle() {
        String action = _server.arg("action");
        if (action == "on") {
            dbgLog.clear();
            dbgLog.enabled = true;
        } else if (action == "off") {
            dbgLog.enabled = false;
        } else if (action == "clear") {
            dbgLog.clear();
        }
        _server.send(200, "application/json",
            dbgLog.enabled ? "{\"enabled\":true}" : "{\"enabled\":false}");
    }
};
