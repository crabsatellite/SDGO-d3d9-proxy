using System;
using System.Collections.Generic;
using System.Linq;
using Sdgo.Shop;

namespace Sdgo.Manager
{
    // Domain-only assembly: no WinForms, Program.Main or independent UI.
    public static class ShopApi
    {
        static ShopService service;
        static string directory, world, client;
        static bool demo;
        static readonly HashSet<string> Editable=new HashSet<string>{"name","label","price","currency","validity","enabled"};
        static string Text(Dictionary<string,object> d,string key,string fallback="") {return d.ContainsKey(key)?Convert.ToString(d[key]):fallback;}
        static object State()
        {
            return new {catalog=service.Catalog,revision=Model.FileHash(service.CatalogFile),demo=service.Demo,
                items=service.Snapshot==null?new List<Item>():service.Snapshot.Items,
                products=service.Catalog.products.Select(p=>new {product=p,status=service.Status(p)}).ToArray()};
        }
        static void Open(Dictionary<string,object> request)
        {
            string d=Text(request,"workspace"),w=Text(request,"world"),c=Text(request,"client");bool useDemo=Text(request,"demo")=="1";
            if(service!=null&&d==directory&&w==world&&c==client&&useDemo==demo)return;
            if(service!=null)service.Dispose();
            service=new ShopService(d,useDemo);directory=d;world=w;client=c;demo=useDemo;
            service.Settings.WorldDirectory=w;service.Settings.ClientDirectory=c;
            service.Connect();
        }
        public static string Execute(string input)
        {
            try
            {
                var request=Model.Json().Deserialize<Dictionary<string,object>>(input);Open(request);
                string action=Text(request,"action");
                var body=request.ContainsKey("body")?(Dictionary<string,object>)request["body"]:new Dictionary<string,object>();
                if(action=="read")return Model.Json().Serialize(new {ok=true,value=State()});
                if(Text(request,"revision")!=Model.FileHash(service.CatalogFile))throw new InvalidOperationException("商城配置已改变，请刷新后重试。");
                if(action=="create")
                {
                    int category=Convert.ToInt32(body["category_id"]);var item=service.Snapshot.Items.FirstOrDefault(i=>i.Id==category);
                    if(item==null)throw new InvalidOperationException("物品不存在。");
                    service.Catalog.products.Add(Model.Create(service.Catalog,service.Snapshot,item));service.SaveDraft();
                }
                else if(action=="update"||action=="delete"||action=="restore")
                {
                    string key=Text(body,"key");var product=service.Catalog.products.FirstOrDefault(p=>p.key==key);
                    if(product==null)throw new InvalidOperationException("商品不存在。");
                    if(action=="update")
                    {
                        var values=(Dictionary<string,object>)body["values"];
                        if(values.Keys.Any(k=>!Editable.Contains(k)))throw new InvalidOperationException("已有商品的身份字段不能更改。");
                        var merged=Model.Json().Deserialize<Dictionary<string,object>>(Model.Json().Serialize(product));
                        foreach(var pair in values)merged[pair.Key]=pair.Value;
                        var changed=Model.Json().Deserialize<Product>(Model.Json().Serialize(merged));
                        service.Catalog.products[service.Catalog.products.IndexOf(product)]=changed;
                    }
                    else {product.retired=action=="delete";product.enabled=action=="restore";}
                    service.SaveDraft();
                }
                else if(action=="apply")service.Apply();
                else if(action=="import")
                {
                    var incoming=Model.Json().Deserialize<Catalog>(Model.Json().Serialize(body["catalog"]));Model.Validate(incoming,service.Snapshot);
                    if(incoming.catalog_id!=service.Catalog.catalog_id)throw new InvalidOperationException("导入配置属于不同的商城工作区。");
                    foreach(var old in service.Catalog.products)
                    {
                        var next=incoming.products.FirstOrDefault(p=>p.key==old.key);
                        if(next!=null&&(next.product_id!=old.product_id||next.item_id!=old.item_id||next.template_id!=old.template_id))throw new InvalidOperationException("导入配置改变了商品身份。");
                        if(next==null){var retired=Model.Clone(old);retired.retired=true;retired.enabled=false;incoming.products.Add(retired);}
                    }
                    service.Catalog=incoming;service.SaveDraft();
                }
                else throw new InvalidOperationException("商城操作未登记。");
                return Model.Json().Serialize(new {ok=true,value=State()});
            }
            catch(Exception error)
            {
                // Discard a failed candidate so a subsequent read is the persisted state.
                if(service!=null){service.Dispose();service=null;}
                return Model.Json().Serialize(new {ok=false,error=error.Message});
            }
        }
    }
}
