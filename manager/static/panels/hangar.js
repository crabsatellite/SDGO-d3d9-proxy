import {e,button} from '/panel_helpers.js';
export async function mount(root,ctx){
 const intro=e('section',undefined,'manager-card');
 intro.append(e('h2','机库整理服务端'),e('p','玩家在游戏的「机体管理」页点击整理。服务器按 S/A/B/C、子等级、机体编号和获得时间写入持久槽位；完成后重新登录。排序保留机体、锁定、舰队、培养、货币和保管库内容。'),e('p','服主选择原生服务器 INI 后安装一次。玩家不需要数据库配置，日常整理不依赖 Manager 常驻。此处不操作任何玩家的库存。'));
 root.append(intro);
 const actions=e('div',undefined,'actions'),result=e('pre',undefined,'manager-card');root.append(actions,result);
 let state=await ctx.request('/api/state',undefined,'GET');
 const show=()=>result.textContent=JSON.stringify(state,null,2);show();
 for(const [id,label] of [['state','检查服务端'],['install','安装或更新服务端'],['remove','移除排序通道']])
  actions.append(button(label,async()=>{state=await ctx.request('/api/'+id,id==='state'?undefined:{revision:state.revision},id==='state'?'GET':'POST');show();},ctx,id==='install'?'primary':''));
}
