import {e,button,download} from '/panel_helpers.js';
export async function mount(root,ctx){
 const intro=e('section',undefined,'manager-card');
 intro.append(e('h2','收藏扭蛋 · Beta'),e('p','每次 100,000 GP，永久获得此前从未获得的初始形态。已出售或置换的机体仍计入历史；奖池随机体库和导入元数据更新。'),e('p','先生成配套资源，再检查并分别安装到目标 WorldServer 和客户端，最后导出玩家包。安装前关闭对应程序；本面板不会停止游戏服务。'));
 root.append(intro);
 let state=await ctx.request('/api/state',undefined);
 const summary=e('p'),result=e('pre'),actions=e('div',undefined,'actions');root.append(summary,actions,result);
 const render=()=>{summary.textContent=state.generated?`已生成 · ${state.eligible_roots} 个可用初始形态 · 100,000 GP。生成不代表已安装或游戏验收通过。`:'尚无与当前配置匹配的构建。';};render();
 for(const [id,label] of [['generate','生成 / 更新配套资源'],['check-install','检查安装条件'],['install-server','安装 / 更新目标 World'],['install-client','安装 / 更新本机客户端'],['export','导出玩家包']]){
  actions.append(button(label,async()=>{
   if(id.startsWith('install-')&&!confirm('更新工作区中选定的目标？请先关闭对应程序。'))return;
   const value=await ctx.request('/api/'+id,{revision:state.revision});
   if(value instanceof Response)await download(value,'SDGO-CollectionCapsule-Beta-Player.zip');
   else {result.textContent=JSON.stringify(value,null,2);state=await ctx.request('/api/state',undefined);render();}
  },ctx,id==='generate'?'primary':''));
 }
 for(const [id,label,key] of [['import-history','导入可信完整获得历史','history'],['register-family','登记导入机体的初始形态','family']]){
  const file=e('input');file.type='file';file.accept='.json';file.hidden=true;
  file.onchange=async()=>{try{const payload=JSON.parse(await file.files[0].text());if(!confirm(id==='import-history'?'确认该文件包含从账号创建起的完整历史？当前机库快照不满足此要求。':'登记该内容包的初始形态？机体定义必须已经在目标数据库中。'))return;const value=await ctx.request('/api/'+id,{revision:state.revision,[key]:payload});result.textContent=JSON.stringify(value,null,2);}catch(error){ctx.message(error.message,true);}finally{file.value='';}};
  root.append(file,button(label,()=>file.click(),ctx));
 }
 root.append(e('p','旧注册账号的历史不完整时不能抽取，须导入可信完整历史。普通扭蛋目录另行更新后，请在本页重新生成并安装客户端资源，再导出最新玩家包。实际扣费、动画和重启持久化仍需手动验收。','intro'));
}
