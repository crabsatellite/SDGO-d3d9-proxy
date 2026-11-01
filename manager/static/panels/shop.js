import {e,button,field,download} from '/panel_helpers.js';
export async function mount(root,ctx){
 root.inert=true;
 let state, selected=null, query='';
 const controls=e('div',undefined,'actions'),search=document.createElement('input');search.placeholder='搜索商品名称或编号';search.oninput=()=>{query=search.value;render();};controls.append(search);
 const select=document.createElement('select');select.ariaLabel='新增商品的原生物品';
 controls.append(select,button('新增商品',async()=>{if(!select.value)throw Error('请选择原生物品');await action('create',{category_id:Number(select.value)});},ctx,'primary'),button('应用到游戏',async()=>{if(!confirm('将当前商城草稿应用到已选择的服务器和客户端？'))return;await action('apply',{});ctx.message('服务器已返回应用结果。游戏内显示与购买由你手动检查。');},ctx),button('导出配置',async()=>download(await ctx.fetch('/api/shop/export'),'shop.json'),ctx));
 const importFile=document.createElement('input');importFile.type='file';importFile.accept='.json';importFile.hidden=true;importFile.onchange=async()=>{try{await action('import',{catalog:JSON.parse(await importFile.files[0].text())});}catch(error){ctx.message(error.message,true);}};
 controls.append(button('导入配置',()=>importFile.click(),ctx),importFile);root.append(controls);
 const layout=e('div',undefined,'settings-grid'),list=e('section',undefined,'manager-card'),editor=e('section',undefined,'manager-card');layout.append(list,editor);root.append(layout);
 async function load(){state=await ctx.request('/api/shop/catalog');const options=state.items.map(item=>new Option(item.Name+' · '+item.Id,item.Id));select.replaceChildren(...options);render();}
 async function action(name,body){state=await ctx.request('/api/shop/'+name,body,'POST',{'X-Shop-Revision':state.revision});render();}
 function render(){
  list.replaceChildren(e('h2',state.demo?'商城商品（演示）':'商城商品'));
  const rows=state.products.filter(row=>JSON.stringify(row.product).toLowerCase().includes(query.toLowerCase()));
  for(const row of rows){const p=row.product;list.append(button(p.name+' · '+p.price+' '+p.currency+' · '+row.status,()=>{selected=p.key;render();},ctx));}
  if(!rows.length)list.append(e('p','还没有管理商品。选择一个原生物品，然后新增。','muted'));
  editor.replaceChildren();const product=state.catalog.products.find(p=>p.key===selected);if(!product){editor.append(e('p','选择商品查看和编辑。','muted'));return;}
  editor.append(e('h2',product.name),e('p','商品 '+product.product_id+' · 物品 '+product.category_id,'muted'));
  const fields={};for(const [key,label,type] of [['name','商品名称'],['label','选项标签'],['price','售价','number'],['validity','数量 / 租赁天数','number'],['enabled','上架','checkbox']]){fields[key]=field(label,product[key],type);editor.append(fields[key].box);}
  const currency=e('label','币种'),value=document.createElement('select');for(const name of ['GB','MB'])value.append(new Option(name,name));value.value=product.currency;currency.append(value);editor.append(currency);
  editor.append(button('保存商品',async()=>{const values={currency:value.value};for(const [key,entry] of Object.entries(fields))values[key]=entry.input.type==='checkbox'?entry.input.checked:entry.input.type==='number'?Number(entry.input.value):entry.input.value;await action('update',{key:product.key,values});ctx.message('商品草稿已保存，应用到游戏后才会更改服务器商品。');},ctx,'primary'),button(product.retired?'恢复商品':'删除商品',()=>action(product.retired?'restore':'delete',{key:product.key}),ctx));
 }
 try{await load();}finally{root.inert=false;}
}
