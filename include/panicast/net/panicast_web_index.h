R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta http-equiv="X-UA-Compatible" content="IE=edge">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1">
<title>panicast Remote</title>
<style>
:root{--bg:#0d1117;--bg2:#161b22;--fg:#c9d1d9;--mut:#8b949e;--acc:#58a6ff;--grn:#3fb950;--bd:#30363d}
*{box-sizing:border-box}
body{margin:0;font-family:Segoe UI,Arial,sans-serif;background:var(--bg);color:var(--fg)}
.bar{background:var(--bg2);border-bottom:1px solid var(--bd);padding:8px 12px;display:flex;align-items:center;gap:10px}
.bar b{font-size:16px}
.dot{width:9px;height:9px;border-radius:50%;background:#888}
.dot.on{background:var(--grn)}
.wrap{max-width:760px;margin:0 auto;padding:14px}
.card{background:var(--bg2);border:1px solid var(--bd);border-radius:10px;padding:14px;margin-bottom:12px}
.mode{display:inline-block;background:var(--acc);color:#000;border-radius:4px;padding:2px 7px;font-size:12px;font-weight:bold}
.row{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin:8px 0}
button{background:var(--bg);color:var(--fg);border:1px solid var(--bd);border-radius:8px;padding:9px 14px;font-size:15px;cursor:pointer;min-width:44px;min-height:44px}
button:hover{border-color:var(--acc)}
button.act{background:var(--acc);color:#000;border-color:var(--acc)}
input[type=range]{width:160px;vertical-align:middle}
input[type=text]{background:var(--bg);color:var(--fg);border:1px solid var(--bd);border-radius:8px;padding:10px;font-size:15px}
.mut{color:var(--mut);font-size:13px}
#log{white-space:pre-wrap;font-family:Consolas,monospace;font-size:12px;color:var(--mut);max-height:160px;overflow:auto}
#pinbox{position:fixed;inset:0;background:rgba(0,0,0,.8);display:none;align-items:center;justify-content:center;z-index:9}
#pinbox .c{background:var(--bg2);border:1px solid var(--bd);border-radius:10px;padding:24px;text-align:center}
@media(max-width:560px){input[type=range]{width:120px}}
</style>
</head>
<body>
<div class="bar"><span class="dot" id="dot"></span><b>panicast</b><span class="mut" id="ver"></span>
<span style="flex:1"></span><span class="mut" id="ping"></span></div>
<div class="wrap">
  <div class="card">
    <div><span class="mode" id="mode">--</span> <b id="title">Not playing</b></div>
    <div class="mut" id="sub"></div>
    <div class="row" style="margin-top:10px">
      <input type="range" id="seek" min="0" max="1000" value="0">
      <span id="time" class="mut">0:00 / 0:00</span>
    </div>
    <div class="row">
      <button onclick="send('previous')">&#9198;</button>
      <button onclick="send('seek -15')">&#9194;15</button>
      <button id="pp" class="act" onclick="send('play_pause')">&#9654;&#65039;</button>
      <button onclick="send('seek 15')">15&#9193;</button>
      <button onclick="send('next')">&#9197;</button>
    </div>
    <div class="row">
      <span class="mut">Vol</span><input type="range" id="vol" min="0" max="100" value="100" onchange="send('volume '+this.value)">
      <span class="mut" id="volv">100</span>
      <span class="mut" style="margin-left:8px">Speed</span>
      <button onclick="send('speed_down')">-</button><span id="spd">1.0</span><button onclick="send('speed_up')">+</button>
      <button onclick="send('speed_reset')">1x</button>
    </div>
    <div class="row">
      <button onclick="send('repeat')">Repeat</button>
      <button onclick="send('shuffle')">Shuffle</button>
      <button onclick="send('cycle')">Cycle</button>
      <span class="mut" id="pmode"></span>
      <span class="mut" style="margin-left:auto">Sleep</span>
      <input type="text" id="sleep" size="6" placeholder="30m"><button onclick="send('sleep '+g('sleep').value)">Set</button>
      <button onclick="send('sleep_cancel')">X</button>
    </div>
    <div class="row">
      <span class="mut">Mode:</span>
      <button onclick="send('mode RADIO')">R</button>
      <button onclick="send('mode PODCAST')">P</button>
      <button onclick="send('mode FAVOURITE')">F</button>
      <button onclick="send('mode HISTORY')">H</button>
      <button onclick="send('mode ONLINE')">O</button>
      <button onclick="send('mode ACCOUNT')">Y</button>
      <button onclick="send('mode BILIBILI')">B</button>
      <button onclick="send('mode TIKTOK')">T</button>
      <button onclick="send('mode IPTV')">I</button>
    </div>
    <div class="row">
      <button onclick="send('nav_up')">Up</button>
      <button onclick="send('nav_down')">Down</button>
      <button onclick="send('nav_enter')">Enter</button>
      <button onclick="send('nav_back')">Back</button>
      <button onclick="send('nav_top')">Top</button>
      <button onclick="send('nav_bottom')">Bottom</button>
      <button onclick="send('mpv f')">Full</button>
      <button onclick="send('mpv m')">Mute</button>
    </div>
  </div>
  <div class="card"><b>Status</b><div id="log"></div></div>
</div>
<div id="pinbox"><div class="c"><b>Enter PIN</b><br><span class="mut">(shown in panicast via :pin, or 6696)</span><br><br>
<input type="text" id="pin" size="8" placeholder="PIN"><br><br><button class="act" onclick="doPin()">Connect</button></div></div>
<script>
var ws,buf="",lastElapsed=0,lastTs=0,speed=1,dur=0,authed=false;
function g(id){return document.getElementById(id)}
function fmt(s){s=Math.max(0,s|0);var m=(s/60)|0;return m+':'+('0'+(s%60)).slice(-2)}
function send(line){if(ws&&ws.readyState==1)ws.send(line+"\n")}
function showPin(){g('pinbox').style.display='flex'}
function doPin(){var p=g('pin').value;send("password "+p);g('pinbox').style.display='none'}
function handle(line){
  if(line=="OK"||line.indexOf("ACK")==0){buf="";return}
  var i=line.indexOf(":");if(i<0)return;
  var k=line.substr(0,i).trim(),v=line.substr(i+1).trim();
  if(k=="volume"){g('vol').value=v;g('volv').textContent=v}
  else if(k=="state"){var b=g('pp');b.innerHTML=v=="play"?"&#9208;&#65039;":"&#9654;&#65039;"}
  else if(k=="elapsed"){lastElapsed=parseFloat(v);lastTs=Date.now()}
  else if(k=="duration"){dur=parseFloat(v)}
  else if(k=="speed"){speed=parseFloat(v);g('spd').textContent=speed.toFixed(1)}
  else if(k=="mode"){g('mode').textContent=v}
  else if(k=="title"){g('title').textContent=v||"Not playing"}
  else if(k=="play_mode"){g('pmode').textContent=v}
  else if(k=="sleep_remaining"){g('sub').textContent=v<0?"":"sleep "+v+"s"}
  g('log').textContent=line;
}
function tick(){ // interpolate progress bar
  if(dur>0&&lastTs){var el=lastElapsed+(Date.now()-lastTs)/1000*speed;el=Math.min(el,dur);
    g('seek').value=Math.round(el/dur*1000);g('time').textContent=fmt(el)+" / "+fmt(dur)}
}
function onMsg(evt){
  buf+=evt.data;
  var p;while((p=buf.indexOf("\n"))>=0){var ln=buf.substr(0,p);buf=buf.substr(p+1);
    if(ln.indexOf("auth required")>=0&&!authed){authed=false;showPin();continue}
    handle(ln)}
}
function connect(){
  ws=new WebSocket("ws://"+location.host+"/");
  ws.onopen=function(){g('dot').className="dot on";send("idle player mixer options mode");setInterval(function(){send("status")},1500);setInterval(tick,250);setInterval(function(){var t=Date.now();send("ping")},25000)}
  ws.onmessage=onMsg;
  ws.onclose=function(){g('dot').className="dot";setTimeout(connect,2000)}
  ws.onerror=function(){}
}
connect();
</script>
</body>
</html>
)HTML"
