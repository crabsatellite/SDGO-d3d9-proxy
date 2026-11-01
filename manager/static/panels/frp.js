import {e,button,field,download} from '/panel_helpers.js';
export async function mount(root,ctx){
 let state=await ctx.request('/api/profile');let profile=state.profile||{schema_version:2,udp_bind_ip:'127.0.0.1',login:{logical_ip:'127.0.0.1',logical_port:5001,external_host:'',external_port:0},udp:[]};
 const box=e('section',undefined,'manager-card');box.append(e('h2','服主端点配置'),e('p','填写你自己的 Sakura 隧道地址。玩家只接收导出的客户端配置，不需要填写端口或使用专用启动器。','intro'));
 const grid=e('div',undefined,'settings-grid'),inputs={};
 for(const [key,label,value,type] of [['ip','游戏登录逻辑 IP',profile.login.logical_ip],['port','游戏登录逻辑端口',profile.login.logical_port,'number'],['external','Sakura TCP 地址',profile.login.external_host],['externalPort','Sakura TCP 端口',profile.login.external_port,'number'],['bind','Agent 逻辑 UDP IP',profile.udp_bind_ip]]){const f=field(label,value,type);grid.append(f.box);inputs[key]=f.input;}box.append(grid);root.append(box);
 const pool=e('section',undefined,'manager-card');pool.append(e('h2','UDP 端口池'),e('p','每行对应一条已有的 UDP 隧道，本地端口必须连续。','muted'));const table=e('div');pool.append(table);root.append(pool);let udpInputs=[];
 function renderPool(){table.replaceChildren();udpInputs=[];profile.udp.forEach((row,index)=>{const r=e('div',undefined,'settings-grid'),entry={};for(const [key,title,type] of [['local_port','本地端口','number'],['external_host','Sakura UDP 地址'],['external_port','Sakura UDP 端口','number']]){const f=field(title,row[key],type);r.append(f.box);entry[key]=f.input;}r.append(button('删除此端口',()=>{capture();profile.udp.splice(index,1);renderPool();},ctx));udpInputs.push(entry);table.append(r);});}
 function capture(){profile.udp=udpInputs.map(row=>Object.fromEntries(Object.entries(row).map(([k,v])=>[k,k.includes('port')?Number(v.value):v.value.trim()])));}
 pool.append(button('添加 UDP 端口',()=>{capture();profile.udp.push({local_port:(profile.udp.at(-1)?.local_port||10000)+1,external_host:inputs.external.value,external_port:0});renderPool();},ctx));renderPool();
 async function save(){capture();profile.login={...profile.login,logical_ip:inputs.ip.value.trim(),logical_port:Number(inputs.port.value),external_host:inputs.external.value.trim(),external_port:Number(inputs.externalPort.value)};profile.udp_bind_ip=inputs.bind.value.trim();state=await ctx.request('/api/profile',{profile,revision:state.revision});ctx.message('FRP 配置已保存。导出并分发后，相关客户端和 Agent 需按配置重启。');}
 root.append(button('保存配置',save,ctx,'primary'),button('导出玩家配置包',async()=>{await save();await download(await ctx.request('/api/export',{}),'SDGO-player-connection.zip');},ctx));
}
