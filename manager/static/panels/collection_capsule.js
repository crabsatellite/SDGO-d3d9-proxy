import {e,button,download} from '/panel_helpers.js';
export async function mount(root,ctx){
 const intro=e('section',undefined,'manager-card');
 intro.append(
  e('h2','活动收藏扭蛋 · Beta'),
  e('p','每次 100,000 GP，奖品是可永久持有的初始形态，并通过游戏内领取箱交付。规则是“先补图鉴，图鉴满后补机库”：第一阶段补齐尚未解锁的图鉴机体；全部符合条件的图鉴机体解锁后，第二阶段补回已解锁但当前机库和领取箱都没有的机体。'),
  e('p','C/B/A/S 默认概率为 35%/30%/20%/15%。当前阶段中没有候选机体的等级会退出抽取，其余等级按原权重重新分配。若图鉴已经解锁完且所有机体都在机库或领取箱中，本次抽取走原生失败退款路径。'),
  e('p','使用361×322活动海报。先生成配套资源，再检查并分别安装到目标 WorldServer 和客户端，最后导出玩家包。安装前关闭对应程序；本面板不会停止游戏服务。')
 );
 root.append(intro);
 let state=await ctx.request('/api/state',undefined);
 const summary=e('p'),result=e('p'),actions=e('div',undefined,'actions');root.append(summary,actions,result);
 const render=()=>{const position=state.placement?`活动机台第${state.placement.page}页、第${state.placement.page_slot}格。`:'';summary.textContent=state.generated?`已生成 · ${state.eligible_roots} 个可用初始形态 · 100,000 GP。${position}生成不代表已安装或游戏验收通过。`:'尚无与当前配置匹配的构建。';};render();
 for(const [id,label] of [['upgrade-catalog','更新为图鉴规则（无需重启）'],['generate','生成 / 更新配套资源'],['check-install','检查安装条件'],['install-server','安装 / 更新目标 World'],['install-client','安装 / 更新本机客户端'],['export','导出玩家包']]){
  actions.append(button(label,async()=>{
   if(id==='upgrade-catalog'&&!confirm('以当前永久机库和未领取单位奖品初始化图鉴，删除旧完整历史证明与额外历史表；以后先补图鉴，全解锁后补机库。原生账号、机体和抽取记录保留。继续更新已安装的收藏扭蛋规则？'))return;
   if(id.startsWith('install-')&&!confirm('更新工作区中选定的目标？请先关闭对应程序。'))return;
   const value=await ctx.request('/api/'+id,{revision:state.revision});
   if(value instanceof Response){await download(value,'SDGO-CollectionCapsule-Beta-Player.zip');result.textContent='玩家包已生成，请查看下载目录并分发给其他玩家。';}
   else {result.textContent=({'upgrade-catalog':'图鉴规则已更新，旧账号可直接使用；本次仅更新 SQL，无需重启 World。','generate':'配套资源已生成。接下来检查安装条件，并分别安装服务端和客户端。','check-install':'安装条件检查通过。此检查不会写入服务端或客户端。','install-server':'服务端已安装。现在可以启动选定的 WorldServer。','install-client':'客户端资源已安装。请使用原来的方式启动游戏。'})[id];state=await ctx.request('/api/state',undefined);render();}
  },ctx,id==='generate'?'primary':''));
 }
 const family=e('input');family.type='file';family.accept='.json';family.hidden=true;
 family.onchange=async()=>{try{
  const payload=JSON.parse(await family.files[0].text());
  if(!confirm('登记该内容包的初始形态？机体定义必须已经在目标数据库中。'))return;
  await ctx.request('/api/register-family',{revision:state.revision,family:payload});
  result.textContent='初始形态已登记，可重新生成配套资源。';
 }catch(error){ctx.message(error.message,true);}finally{family.value='';}};
 root.append(family,button('登记导入机体的初始形态',()=>family.click(),ctx));
 root.append(e('p','安装时会以当前永久机体和领取箱内尚未领取的永久机体建立最小图鉴解锁基线。Mod 只保存账号与已解锁机体 ID，不导入旧版完整获得历史，也不记录时间、来源或抽取事件。普通扭蛋目录另行更新后，请在本页重新生成并安装客户端资源，再导出最新玩家包。实际扣费、动画和重启持久化仍需手动验收。','intro'));
}
