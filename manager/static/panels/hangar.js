import {e,button} from '/panel_helpers.js';
export async function mount(root,ctx){
 const intro=e('section',undefined,'manager-card');
 intro.append(e('h2','机库整理与批量回收服务端'),e('p','玩家在游戏的「机体管理」页整理或预览回收。整理只写槽位；GB 回收在两次确认后按所选实例批量执行，服务器核对归属、保护条件、保留数量和奖励，删除与 GB 入账在同一事务完成。提交后重新登录查看。'),e('p','服主选择包含 DB Game、DB Member 和 DB StoreBilling 的原生服务器 INI 后安装一次。玩家不需要数据库配置，日常使用不依赖 Manager 常驻。此处不操作任何玩家的库存。'));
 root.append(intro);
 const actions=e('div',undefined,'actions'),result=e('pre',undefined,'manager-card');root.append(actions,result);
 let state=await ctx.request('/api/state',undefined,'GET');
 const show=()=>result.textContent=JSON.stringify(state,null,2);show();
 for(const [id,label] of [['state','检查服务端'],['install','安装或更新服务端'],['remove','移除排序通道']])
  actions.append(button(label,async()=>{state=await ctx.request('/api/'+id,id==='state'?undefined:{revision:state.revision},id==='state'?'GET':'POST');show();},ctx,id==='install'?'primary':''));
}
