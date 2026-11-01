import {e,button,download} from '/panel_helpers.js';
export async function mount(root,ctx){
 root.append(e('section','先在工作区设置中选择目录表来源和配套客户端 gdrs。更新时提供上一版分配记录，避免复用旧机体编号。生成、安装和游戏内验收是不同步骤。','manager-card'));
 const actions=e('div',undefined,'actions'),result=e('pre',undefined,'manager-card');root.append(actions,result);
 for(const [id,label] of [['check','检查目录'],['generate','生成配套清单'],['install_client','安装到所选客户端'],['install_server','安装到所选 BattleServer'],['export','导出玩家更新包']]){
  actions.append(button(label,async()=>{if(id.startsWith('install')&&!confirm('安装到工作区中配置的目标？目标游戏或服务器需要已停止。'))return;const value=await ctx.request('/api/'+id,{});if(value instanceof Response)await download(value,'SDGO-AI-catalog-client.zip');else result.textContent=JSON.stringify(value,null,2);},ctx,id==='generate'?'primary':''));
 }
}
