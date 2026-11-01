const $=id=>document.getElementById(id);
let state, token, hostId, panelCleanup=()=>{}, activePanel=null, archiveView=false, navigation=0;
function element(tag, text, cls){const node=document.createElement(tag);if(text!==undefined)node.textContent=text;if(cls)node.className=cls;return node;}
function message(text,error=false){$('manager-message').textContent=text;$('manager-message').classList.toggle('error',error);}
async function request(path,body,method='POST',extra={}){
 const response=await fetch(path,body===undefined?{}:{method,headers:{'Content-Type':'application/json','X-ModManager-Token':token,...extra},body:JSON.stringify(body)});
 const type=response.headers.get('Content-Type')||'';
 if(!type.includes('json')){if(!response.ok)throw Error('请求失败');return response;}
 const result=await response.json();if(!response.ok)throw Error(result.error||'请求失败');return result;
}
function button(text,fn,cls=''){const b=element('button',text,cls);b.onclick=()=>Promise.resolve().then(fn).catch(e=>message(e.message,true));return b;}
function clear(){panelCleanup();panelCleanup=()=>{};$('content').replaceChildren();message('');}
function selectedHost(){return state.hosts.find(h=>h.id===hostId)||state.hosts[0];}
function accept(value){state=value;hostId=selectedHost()?.id;renderFrame();}
function renderFrame(){
 const developer=state.mode==='developer';$('mode-label').textContent=developer?'开发信息已展开':'日常管理';$('change-mode').textContent=developer?'收起开发信息':'显示开发信息';$('eyebrow').textContent=developer?'开发调试':'玩家与服主';$('add-host').textContent='添加客户端 / 服务器';
 const roles={client:'客户端',battle:'对战服务器',agent:'中继服务器',world:'世界服务器'};
 $('host-select').replaceChildren(...state.hosts.map(h=>{const o=new Option(h.name+' · '+(developer?h.role:roles[h.role]),h.id);o.selected=h.id===hostId;return o;}));
 $('nav-mods').classList.toggle('selected',activePanel===null&&!archiveView);$('nav-archive').classList.toggle('selected',activePanel===null&&archiveView);$('nav-services').classList.toggle('selected',activePanel==='services');$('provider-nav').replaceChildren();for(const panel of state.panels||[]){$('provider-nav').append(button(panel.title,()=>showProvider(panel.id),'nav-item'+(activePanel===panel.id?' selected':'')));}
}
function supportActions(context){const actions=element('div',undefined,'actions ai-actions');for(const [kind,label] of [['configure','复制给 AI 配置 / 调试'],['bug','发现 Bug · 复制给 AI 报告']])actions.append(button(label,async()=>{const value=await request('/api/ai-context',{...context,kind});try{await navigator.clipboard.writeText(value.text);message('已复制，可以粘贴给 DeepSeek 或其他 AI。');}catch{const d=document.createElement('dialog'),t=document.createElement('textarea');t.value=value.text;d.append(element('h2','给 AI 的说明'),t,button('关闭',()=>d.close()));document.body.append(d);d.showModal();t.select();}} ,'quiet'));return actions;}
function showMods(isArchive=false){
 ++navigation;
 archiveView=isArchive===true;activePanel=null;renderFrame();clear();$('page-title').textContent=archiveView?'修复归档':'游戏功能';const root=$('content'),host=selectedHost();document.querySelector('.host-tools').hidden=false;
 if(!host){const blank=element('section',undefined,'empty-state');blank.append(element('small','欢迎使用 SDGO','eyebrow'),element('h2','从你的客户端开始'),element('p','选择已经安装好的单机版，查看可用 Mod。游戏仍按原来的方式启动。'),button('选择客户端',openPath,'primary'));root.append(blank);return;}
 root.append(element('p',archiveView?'这些修复随框架自动工作，日常不需要逐项配置。需要排查时，可在这里查看说明或复制给 AI。':'管理客户端和服务器的游戏功能。商城、副官、扭蛋机和联机设置在左侧；自动 Bug 修复收在归档中。','intro'));
 if(!archiveView){const gates=element('section',undefined,'manager-card row');for(const [key,title] of [['gameplay','游戏扩展总开关'],['network','联机功能总开关']]){const label=element('label',title),input=document.createElement('input');input.type='checkbox';input.checked=host.categories[key];input.onchange=async()=>{try{accept(await request('/api/category',{host:host.id,category:key,enabled:input.checked,revision:host.global_revision}));showMods();message('分类开关已保存，重新打开对应游戏 / 服务器后生效。');}catch(e){input.checked=!input.checked;message(e.message,true);}};label.prepend(input);gates.append(label);}root.append(gates);}
 if(host.role==='client'){const file=document.createElement('input');file.type='file';file.accept='.zip';file.hidden=true;file.onchange=async()=>{try{const bytes=new Uint8Array(await file.files[0].arrayBuffer());let text='';for(let i=0;i<bytes.length;i+=8192)text+=String.fromCharCode(...bytes.subarray(i,i+8192));const result=await request('/api/import',{host:host.id,package:btoa(text)});accept(result.state);showMods();message('配置包已导入，使用原来的启动方式重新打开游戏即可。');}catch(error){message(error.message,true);}};root.append(file,button('导入客户端配置包',()=>file.click(),'back'));}
 const grid=element('div',undefined,'mod-grid');
 for(const mod of (archiveView?host.archive:host.mods)){
  const card=element('section',undefined,'mod-card'),top=element('div',undefined,'mod-top');top.append(element('h2',mod.title));const toggle=document.createElement('input');toggle.type='checkbox';toggle.checked=mod.enabled;toggle.ariaLabel='启用 '+mod.title;
  toggle.disabled=archiveView&&state.mode!=='developer';toggle.onchange=async()=>{try{const next=await request('/api/toggle',{host:host.id,mod:mod.id,enabled:toggle.checked,revision:{global:host.global_revision,manifest:mod.revision}});accept(next);showMods(archiveView);message('已保存，重新启动对应游戏或服务器后生效。');}catch(e){toggle.checked=!toggle.checked;message(e.message,true);}};top.append(toggle);card.append(top,element('p',mod.description));
  const meta=element('div',undefined,'mod-meta');meta.append(element('span',mod.version?'v'+mod.version:''),element('span',mod.enabled?'配置已启用':'配置已关闭'));
  if(mod.blocked_by.length)meta.append(element('span','上层开关已关闭'));card.append(meta);
  card.append(button(archiveView?'查看说明':'设置功能',()=>showSettings(host.id,mod.id)),supportActions({host:host.id,mod:mod.id}));
  grid.append(card);
 }
 if(!(archiveView?host.archive:host.mods).length)root.append(element('section',archiveView?'此目录没有需要归档的自动修复。':'当前目录尚无已安装的可配置功能。可导入配套 Mod 包，或添加你的服务器。','empty-state'));
 root.append(grid);
}
async function showSettings(host,mod){
 const data=await request('/api/settings?'+new URLSearchParams({host,mod}));clear();$('page-title').textContent=data.contract.title;
 const root=$('content');root.append(button('← 返回功能列表',()=>showMods(archiveView),'back'));root.append(element('p',data.contract.description||'调整此功能的日常设置。','intro'));if(data.contract.provider)root.append(button('打开对应管理面板',()=>showProvider(data.contract.provider),'primary'));
 const box=element('section',undefined,'manager-card'),fields=element('div',undefined,'settings-grid'),controls={};
 for(const field of data.contract.settings){const label=element('label',field.label),input=document.createElement(field.type==='choice'?'select':'input');input.dataset.key=field.id;
  if(field.type==='choice')for(const option of field.options)input.append(new Option(option,option));
  if(field.type!=='choice')input.type=field.type==='boolean'?'checkbox':['integer','number'].includes(field.type)?'number':'text';
  if(field.type==='boolean')input.checked=data.values[field.id];else input.value=data.values[field.id]??'';
  if(field.minimum!==undefined)input.min=field.minimum;if(field.maximum!==undefined)input.max=field.maximum;
  if(field.type==='number')input.step='any';label.append(input,element('span',field.activation==='hot_reload'?'支持运行中更新':'保存后需重启对应游戏 / 服务器','field-help'));if(state.mode==='developer')label.append(element('code',field.file+' · '+field.key,'field-help'));fields.append(label);controls[field.id]=input;
 }
 box.append(fields);if(data.contract.settings.length){box.append(button('保存配置',async()=>{const values={};for(const field of data.contract.settings){const c=controls[field.id];values[field.id]=field.type==='boolean'?c.checked:['integer','number'].includes(field.type)?Number(c.value):c.value;}const result=await request('/api/settings',{host,mod,values,revisions:data.revisions});data.revisions=result.revisions;message('已保存配置。请按字段提示重新启动游戏 / 服务器或等待热更新。');},'primary'));}else box.append(element('p',data.contract.presentation==='automatic_fix'?'此项修复自动工作，日常无需调整。':'此功能没有需要手动填写的日常参数。','muted'));root.append(box,supportActions({host,mod}));
}
async function showProvider(id){
 const sequence=++navigation;const config=await request('/api/providers/'+id+'/config');if(sequence!==navigation)return;clear();activePanel=id;renderFrame();$('page-title').textContent=config.title;document.querySelector('.host-tools').hidden=true;
 const root=$('content'),card=element('section',undefined,'manager-card'),details=element('details');details.open=!config.configured;details.append(element('summary','选择游戏目录与现有配置'));root.append(supportActions({provider:id}));
 const grid=element('div',undefined,'settings-grid'),inputs={};for(const [key,title] of Object.entries(config.fields)){const label=element('label',title),input=document.createElement('input');input.value=config.config[key]||'';label.append(input);if(key!=='demo')label.append(button('浏览…',async()=>{const value=await request('/api/pick',{kind:['server_ini','rules','previous','catalog','patterns','lookup','profile','module_binary','symbol_fragment'].includes(key)?'file':'directory'});if(value.path)input.value=value.path;},'quiet'));grid.append(label);inputs[key]=input;}
 details.append(grid,button('保存工作区设置',async()=>{const data=Object.fromEntries(Object.entries(inputs).map(([k,v])=>[k,v.value.trim()]));await request('/api/providers/'+id+'/config',data);await showProvider(id);},'primary'));card.append(details);root.append(card);
 if(id==='rewards'&&config.configured){const actions=element('div',undefined,'actions');for(const [action,title] of [['start','启动钱包服务'],['stop','停止本 Manager 的钱包服务']])actions.append(button(title,async()=>{const value=await request('/api/services/rewards.wallet',{action});message(value.error|| (value.running?'钱包服务正在运行':'本 Manager 的钱包服务已停止'));}));actions.append(button('检查安装 / 更新',async()=>{const value=await request('/api/providers/rewards/api/rewards/install-plan',{});const blocked=value.plan.blockers||[];message(blocked.length?'需要先处理：'+blocked.join('；'):'安装计划已通过检查。请确认选中的对战服务器已关闭。');if(blocked.length)return;card.append(button('应用刚才检查的安装计划',async()=>{if(!confirm('按刚才检查的计划安装到选中的服务器？不会停止其他游戏服务。'))return;await request('/api/providers/rewards/api/rewards/install',{review_id:value.review_id});message('安装操作已完成。启动对应服务器后，再查看面板运行状态。');},'primary'));}));card.append(actions);}
 if(id==='operators'&&config.configured)card.append(button('更新已选择的本机客户端',async()=>{const current=await request('/api/providers/operators/api/operators');await request('/api/providers/operators/api/operators/update-client',{},'POST',{'X-Catalog-Revision':current.revision});message('配套副官客户端配置已更新，重新打开这个客户端后生效。');}));
 if(!config.configured){root.append(element('p','先选择已有配置工作区。此操作不会导入或重置数据库。','intro'));return;}
 const mount=element('section',undefined,'provider-root');root.append(mount);
 const timers=new Set(),listeners=[];const ctx={token,request:(path,body,method='POST',headers={})=>request('/api/providers/'+id+path,body,method,headers),fetch:async(path,options={})=>{const headers=new Headers(options.headers);headers.set('X-ModManager-Token',token);return fetch(String(path).startsWith('/api/providers/')?path:'/api/providers/'+id+path,{...options,headers});},setInterval:(fn,ms)=>{const t=setInterval(fn,ms);timers.add(t);return t;},onWindow:(name,fn)=>{window.addEventListener(name,fn);listeners.push([name,fn]);},message};
 message('正在读取模块工作区…');const module=await import('/panels/'+id+'.js');if(sequence!==navigation)return;const dispose=await module.mount(mount,ctx);if(sequence===navigation){message('');mount.dataset.ready='1';}
 panelCleanup=()=>{timers.forEach(clearInterval);listeners.forEach(([name,fn])=>window.removeEventListener(name,fn));if(dispose)dispose();};
}
async function openPath(){try{const value=await request('/api/pick',{kind:'directory'});if(!value.path)return;accept(await request('/api/hosts',{path:value.path}));showMods();}catch(error){message(error.message,true);$('path-title').textContent='添加客户端或服务器';$('host-path').value='';$('path-dialog').showModal();}}
async function showServices(){
 ++navigation;activePanel='services';clear();renderFrame();$('page-title').textContent='后台任务';document.querySelector('.host-tools').hidden=true;
 const data=await request('/api/services'),root=$('content');
 root.append(element('p','所有 Mod 后台服务由 Manager 统一启动、停止和恢复。退出 Manager 后服务会停止；再次打开时会恢复你已启用的任务。','intro'));
 for(const item of data.services){
  const card=element('section',undefined,'manager-card');card.append(element('h2',item.title),element('p',item.running?(item.healthy===false?'运行中，等待健康回报':'正在运行'):'已停止','muted'));
  if(item.error)card.append(element('p',item.error,'error'));
  card.append(element('p',item.desired?'已启用自动恢复':'未启用自动恢复','muted'),button(item.desired?'停止任务':'启动任务',async()=>{await request('/api/services/'+item.id,{action:item.desired?'stop':'start'});await showServices();},'primary'));
  if(state.mode==='developer')card.append(element('code',item.id+' · owner='+item.owner,'field-help'));
  root.append(card);
 }
 for(const item of data.resources){const card=element('section',undefined,'manager-card');card.append(element('h2',item.id==='shop.bridge'?'商城业务通道':'内部业务通道'),element('p',item.running?'按需运行，由 Manager 管理':'已停止','muted'));root.append(card);}
}
$('nav-services').onclick=()=>showServices().catch(e=>message(e.message,true));
$('nav-mods').onclick=()=>showMods(false);$('nav-archive').onclick=()=>showMods(true);$('host-select').onchange=()=>{hostId=$('host-select').value;showMods(archiveView);};$('add-host').onclick=openPath;
$('confirm-host').onclick=async event=>{event.preventDefault();try{accept(await request('/api/hosts',{path:$('host-path').value.trim()}));$('path-dialog').close();showMods();}catch(e){message(e.message,true);}};
$('change-mode').onclick=async()=>{try{accept(await request('/api/mode',{mode:state.mode==='standard'?'developer':'standard'}));if(activePanel==='services')await showServices();else if(activePanel)await showProvider(activePanel);else showMods(archiveView);}catch(e){message(e.message,true);}};
$('exit-manager').onclick=async()=>{if(!confirm('请先保存正在编辑的内容。退出管理器？游戏不会关闭。'))return;try{await request('/api/exit',{});$('content').replaceChildren(element('section','管理器已退出，可以关闭本页。','empty-state'));}catch(e){message(e.message,true);}};
try{const initial=await request('/api/state');token=initial.session_token;accept(initial);showMods();}catch(e){message(e.message,true);}
