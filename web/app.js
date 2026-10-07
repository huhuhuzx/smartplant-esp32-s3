const $ = id => document.getElementById(id);
const pages = ['dashboard','history','watering','automation','sensor','settings'];
const titles = {dashboard:'植物概览',history:'历史记录',watering:'手动浇水',automation:'自动化设置',sensor:'传感器校准',settings:'设备设置'};
let status = null, settings = null, socket = null, reconnect = null, online = false, history = [], lastTelemetry = 0;
let statusReceivedAt = 0;

function automaticTime(timestamp) {
  return timestamp ? new Date(timestamp * 1000).toLocaleString('zh-CN', {month:'2-digit',day:'2-digit',hour:'2-digit',minute:'2-digit',second:'2-digit',hour12:false}) : '时间未记录';
}
function renderWateringPlan() {
  if (!status) return;
  const plan = status.auto || {};
  $('auto-last-time').textContent = automaticTime(plan.last_time);
  $('auto-next-time').textContent = '';
  const messages = {disabled:'自动浇水已关闭',sensor_fault:'等待传感器恢复',daily_limit:'今日次数已达上限',wet:'等待土壤变干',ready:'即将自动浇水',watering:'正在浇水'};
  if (!online) {
    $('auto-countdown').textContent = '等待设备连接';
    $('auto-plan-hint').textContent = '连接恢复后更新倒计时。';
    return;
  }
  const seconds = Math.max(0, (plan.next_seconds ?? -1) - Math.floor((performance.now() - statusReceivedAt) / 1000));
  if ((plan.wait_reason === 'cooldown' || plan.wait_reason === 'watering') && seconds > 0) {
    $('auto-countdown').textContent = [Math.floor(seconds/3600),Math.floor(seconds/60)%60,seconds%60].map(n=>String(n).padStart(2,'0')).join(':');
    $('auto-next-time').textContent = plan.next_time ? `最早可再次浇水：${automaticTime(plan.next_time)}` : '设备时间同步中';
  } else {
    $('auto-countdown').textContent = messages[plan.wait_reason] || '等待设备判断';
  }
  $('auto-plan-hint').textContent = plan.wait_reason === 'daily_limit' ? '明天重新计算每日次数，并根据土壤湿度决定是否浇水。' : '倒计时结束后，土壤湿度低于启动阈值才会自动浇水。';
}

function notice(message, success = false) {
  const el = $('notice'); el.textContent = message; el.hidden = !message; el.className = success ? 'notice success' : 'notice';
  if (success) setTimeout(() => { if (el.textContent === message) el.hidden = true; }, 3500);
}
function connection(up) {
  online = up;
  $('connection-dot').className = 'dot ' + (up ? 'online' : 'offline');
  $('connection-label').textContent = up ? '设备已连接' : '设备离线 · 正在重连';
  document.querySelectorAll('[data-water],[data-calibrate],[data-water-calibrate],#automation-form button,#device-form button,#wifi-form button').forEach(el => el.disabled = !up);
  if (!up) notice('设备连接中断。请检查 Wi-Fi，页面会自动重连。');
  else if ($('notice').textContent.includes('连接中断')) $('notice').hidden = true;
  renderWateringPlan();
}
async function api(path, options = {}) {
  const response = await fetch('/api/v1/' + path, {cache:'no-store', ...options, headers:{'Content-Type':'application/json', ...(options.headers || {})}});
  let data; try { data = await response.json(); } catch { throw new Error('设备未返回有效数据'); }
  if (!response.ok) throw new Error(data.message || data.code || '设备请求失败');
  return data;
}
function clock(seconds) { return seconds < 3600 ? `${Math.floor(seconds/60)} 分钟` : seconds < 86400 ? `${Math.floor(seconds/3600)} 小时` : `${Math.floor(seconds/86400)} 天`; }
function render(s) {
  status = s;
  statusReceivedAt = performance.now();
  const soil = s.soil || {}, pump = s.pump || {}, tank = s.tank || {}, aut = s.auto || {}, wifi = s.wifi || {};
  const percent = soil.valid ? soil.percent : null;
  $('soil-value').textContent = percent ?? '--';
  $('soil-fill').style.width = `${percent ?? 0}%`;
  $('soil-state').textContent = !soil.valid ? '传感器异常' : soil.state === 'dry' ? '偏干' : soil.state === 'wet' ? '湿润' : '适宜';
  $('soil-state').className = 'tag ' + (!soil.valid ? 'error' : soil.state === 'dry' ? 'warning' : '');
  $('soil-caption').textContent = !soil.valid ? '检查传感器接线并重新校准。自动浇水已暂停。' : soil.state === 'dry' ? '湿度低于启动阈值，设备将按规则判断是否浇水。' : '当前湿度处于可用范围。';
  $('pump-state').textContent = pump.running ? '运行中' : '已停止';
  $('tank-state').textContent = tank.valid ? `相对水位 ${tank.percent}% · 锁定关闭` : tank.raw < 0 ? '水位采集失败 · 锁定关闭' : '水位未校准 · 锁定关闭';
  $('water-raw').textContent = tank.raw ?? '--';
  $('water-valid').textContent = tank.valid ? `${tank.percent}%` : tank.raw < 0 ? '请检查 ADS1115 和接线' : '请完成空水与满水校准';
  $('auto-state').textContent = aut.enabled ? '已开启' : '已关闭';
  $('today-count').textContent = `${aut.today_count ?? 0} / ${aut.max_daily ?? '--'} 次`;
  renderWateringPlan();
  $('manual-pump-state').textContent = pump.running ? '正在浇水' : '水泵已停止';
  $('pump-remaining').textContent = pump.running ? `设备预计剩余 ${Math.ceil(pump.remaining_ms/1000)} 秒` : '设备会自动限制连续运行时间';
  $('summary-rssi').textContent = wifi.connected ? `${wifi.rssi} dBm` : '未连接';
  $('summary-uptime').textContent = clock(s.uptime || 0);
  $('sensor-raw').textContent = soil.raw ?? '--';
  $('sensor-valid').textContent = soil.valid ? '读数正常' : '读数异常';
  $('wifi-ssid').textContent = wifi.ssid || '未配置';
  $('wifi-ip').textContent = wifi.ip || '--';
  $('device-name-top').textContent = s.name || 'SmartPlant';
  $('live-time').textContent = new Date().toLocaleTimeString('zh-CN',{hour:'2-digit',minute:'2-digit'});
  if (s.alert === 'SENSOR_FAULT') notice('传感器读数异常，自动浇水已暂停。请检查接线与校准。');
  else if (s.alert === 'PUMP_IO_FAULT') notice('水泵停止失败，请断开水泵电源并检查 I²C 驱动。');
  else if (s.alert === 'DAILY_LIMIT') notice('今日自动浇水次数已达上限。');
  else if (online && !document.querySelector('#notice.success')) $('notice').hidden = true;
}
function renderSettings(s) {
  settings = s;
  $('summary-low').textContent = `${s.soil_low}%`;
  $('summary-target').textContent = `${s.soil_target}%`;
  $('low-marker').style.left = `${s.soil_low}%`;
  $('target-marker').style.left = `${s.soil_target}%`;
  $('dry-adc').textContent = s.dry_adc;
  $('wet-adc').textContent = s.wet_adc;
  $('water-dry-adc').textContent = s.water_dry_adc < 0 ? '未校准' : s.water_dry_adc;
  $('water-wet-adc').textContent = s.water_wet_adc < 0 ? '未校准' : s.water_wet_adc;
  document.querySelector('#device-form [name=brightness]').closest('label').hidden = !s.brightness_supported;
  for (const name of ['soil_low','soil_target','pump_seconds','soak_seconds','min_interval','max_daily']) document.querySelector(`[name=${name}]`).value = s[name];
  $('auto_enabled').checked = s.auto_enabled;
  document.querySelector('#device-form [name=name]').value = s.name;
  document.querySelector('#device-form [name=brightness]').value = s.brightness;
}
function showPage() {
  let id = location.hash.slice(1); if (!pages.includes(id)) id = 'dashboard';
  document.querySelectorAll('.page').forEach(p => p.classList.toggle('active', p.id === id));
  document.querySelectorAll('.sidebar nav a').forEach(a => a.classList.toggle('active', a.hash === '#' + id));
  $('page-title').textContent = titles[id];
  if (id === 'history') loadHistory();
}
async function refresh() {
  try { let [s,c] = await Promise.all([api('status'),api('settings')]); if(!s.time_synced)s=await api('time',{method:'POST',body:JSON.stringify({timestamp:Math.floor(Date.now()/1000)})}); renderSettings(c); render(s); connection(true); }
  catch { connection(false); }
}
function connectSocket() {
  if (socket) socket.close();
  socket = new WebSocket(`${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/ws`);
  socket.onopen = () => { connection(true); refresh(); };
  socket.onmessage = e => { try { const s = JSON.parse(e.data); if (s.type === 'telemetry') { lastTelemetry = Date.now(); render(s); } } catch {} };
  socket.onclose = () => { connection(false); clearTimeout(reconnect); reconnect = setTimeout(connectSocket, 2500); };
  socket.onerror = () => socket.close();
}
async function water(seconds) {
  try { const s = await api('pump/start',{method:'POST',body:JSON.stringify({duration:seconds})}); render(s); notice(`设备已接受 ${seconds} 秒浇水请求`,true); }
  catch(e) { notice(`浇水失败：${e.message}`); }
}
async function stop() {
  try { render(await api('pump/stop',{method:'POST'})); notice('水泵已停止',true); }
  catch(e) { notice(`停止失败：${e.message}`); }
}
function timeText(timestamp) { return new Date(timestamp * 1000).toLocaleString('zh-CN',{month:'2-digit',day:'2-digit',hour:'2-digit',minute:'2-digit'}); }
function renderEvents(rows, target, max) {
  const events = rows.filter(r => r.type !== 'sample' && r.type !== 'watering_end').slice(-max).reverse();
  $(target).replaceChildren();
  if (!events.length) { const p=document.createElement('p');p.className='empty';p.textContent='暂无浇水记录';$(target).append(p);return; }
  for (const e of events) {
    const row=document.createElement('div');row.className='event';
    const icon=document.createElement('span');icon.className='event-icon';icon.textContent='·';
    const main=document.createElement('div');main.className='event-main';
    const labels={auto_start:'自动浇水',manual_start:'手动浇水',sensor_fault:'传感器故障',tank_empty:'水箱缺水'};
    const strong=document.createElement('strong');strong.textContent=labels[e.type] || e.type;
    const small=document.createElement('small');small.textContent=e.type.endsWith('_start')?`开始时湿度 ${e.soil}%`:`原始 ADC ${e.raw}`;
    main.append(strong,small);const time=document.createElement('span');time.className='event-time';time.textContent=timeText(e.time);row.append(icon,main,time);$(target).append(row);
  }
}
function chart(rows, hours) {
  const svg=$('history-chart');svg.replaceChildren();const samples=rows.filter(r=>r.type==='sample');
  $('history-empty').style.display=samples.length?'none':'block';svg.style.display=samples.length?'block':'none';if(!samples.length)return;
  const NS='http://www.w3.org/2000/svg', max=Math.floor(Date.now()/1000), min=max-hours*3600;
  const width=Math.max(320,svg.clientWidth),span=width-60,ticks=width<500?2:4;
  svg.setAttribute('viewBox',`0 0 ${width} 300`);
  const add=(tag,attrs)=>{const el=document.createElementNS(NS,tag);for(const [key,value] of Object.entries(attrs))el.setAttribute(key,value);svg.append(el);return el;};
  for(let n=0;n<=100;n+=25){let y=250-n*2;add('line',{x1:40,y1:y,x2:width-20,y2:y,stroke:'#e8eee7'});const t=add('text',{x:5,y:y+4,fill:'#536b57','font-size':11});t.textContent=n+'%';}
  const x=t=>40+(t-min)*span/Math.max(max-min,1), y=p=>250-p*2;
  add('polyline',{points:samples.map(r=>`${x(r.time)},${y(r.soil)}`).join(' '),fill:'none',stroke:'#368758','stroke-width':3,'stroke-linecap':'round','stroke-linejoin':'round'});
  if(samples.length===1)add('circle',{cx:x(samples[0].time),cy:y(samples[0].soil),r:4,fill:'#368758'});
  rows.filter(r=>r.type.endsWith('_start')).forEach(r=>add('circle',{cx:x(r.time),cy:y(r.soil),r:5,fill:'#d6904b',stroke:'#fff','stroke-width':2}));
  for(let i=0;i<=ticks;i++){const tick=add('text',{x:40+i*span/ticks,y:280,fill:'#536b57','font-size':11,'text-anchor':i===0?'start':i===ticks?'end':'middle'});tick.textContent=timeText(min+i*(max-min)/ticks);}
}
async function loadHistory() {
  try {
    const hours=Number($('history-range').value),all=await api(`history?limit=400&hours=${hours}`);
    if(hours!==Number($('history-range').value))return;
    history=all;chart(history,hours);renderEvents(history,'history-events',50);renderEvents(all,'recent-events',4);
    const samples=all.filter(r=>r.type==='sample'),range=hours===24?'24 小时':hours===168?'7 天':'30 天';
    $('history-summary').textContent=samples.length?`最近 ${range} · 图中 ${samples.length} 个采样点 · 已有数据：${timeText(samples[0].time)} 至 ${timeText(samples[samples.length-1].time)}。记录每分钟保存一次，较长范围会抽样显示。`:`最近 ${range} 暂无湿度记录。设备联网并同步时间后，每分钟保存一次。`;
  }
  catch(e){notice(`无法读取历史：${e.message}`);}
}
document.addEventListener('click',e=>{const waterButton=e.target.closest('[data-water]');if(waterButton)water(Number(waterButton.dataset.water));if(e.target.closest('[data-stop]'))stop();const cal=e.target.closest('[data-calibrate]');if(cal){const which=cal.dataset.calibrate;api('calibration/'+which,{method:'POST'}).then(s=>{renderSettings(s);refresh();notice('校准值已保存',true)}).catch(err=>notice(`校准失败：${err.message}`));}});
$('automation-form').addEventListener('submit',async e=>{e.preventDefault();const f=e.currentTarget;const body={auto_enabled:$('auto_enabled').checked};for(const name of ['soil_low','soil_target','pump_seconds','soak_seconds','min_interval','max_daily'])body[name]=Number(f.elements[name].value);try{renderSettings(await api('settings',{method:'PUT',body:JSON.stringify(body)}));notice('自动化设置已保存',true)}catch(err){notice(`保存失败：${err.message}`)}});
$('device-form').addEventListener('submit',async e=>{e.preventDefault();const f=e.currentTarget;try{renderSettings(await api('settings',{method:'PUT',body:JSON.stringify({name:f.elements.name.value,brightness:Number(f.elements.brightness.value)})}));notice('设备设置已保存',true)}catch(err){notice(`保存失败：${err.message}`)}});
$('scan-wifi').addEventListener('click',async()=>{try{const networks=await api('wifi/scan');const select=$('wifi-list');select.replaceChildren();for(const n of networks){const option=document.createElement('option');option.value=n.ssid;option.textContent=`${n.ssid} (${n.rssi} dBm)`;select.append(option)}if(!networks.length)notice('没有找到 Wi-Fi 网络。')}catch(err){notice(`扫描失败：${err.message}`)}});
$('wifi-form').addEventListener('submit',async e=>{e.preventDefault();const f=e.currentTarget;try{await api('wifi',{method:'PUT',body:JSON.stringify({ssid:f.elements.ssid.value,password:f.elements.password.value})});notice('Wi-Fi 已保存，设备正在连接。',true)}catch(err){notice(`连接失败：${err.message}`)}});
$('history-range').addEventListener('change',loadHistory);window.addEventListener('hashchange',showPage);
showPage();refresh();connectSocket();setInterval(()=>{if(!online || Date.now()-lastTelemetry > 5000)api('status').then(s=>{render(s);connection(true)}).catch(()=>connection(false))},5000);setInterval(renderWateringPlan,1000);loadHistory();
setInterval(()=>{if(online && (location.hash==='#history' || !location.hash || location.hash==='#dashboard'))loadHistory()},60000);

document.querySelectorAll('[data-water-calibrate]').forEach(button => button.addEventListener('click', async () => {
  try { const cfg = await api('calibration/water/' + button.dataset.waterCalibrate, {method:'POST',body:'{}'}); renderSettings(cfg); await refresh(); notice('水位校准已保存。完成两端校准后显示相对水位。',true); }
  catch (error) { notice(error.message); }
}));
