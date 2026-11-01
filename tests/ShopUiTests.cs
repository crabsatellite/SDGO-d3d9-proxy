using System;
using System.IO;
using System.Linq;
using Sdgo.Shop;

public static class ShopUiTests
{
    static int checks;
    static void Check(bool ok,string name) {if(!ok)throw new Exception(name);checks++;Console.WriteLine("PASS "+name);}
    static void Refuses(Action action,string name) {try{action();}catch(InvalidOperationException){Check(true,name);return;}throw new Exception("Accepted invalid state: "+name);}
    public static int Main(string[] args)
    {
        try
        {
            string root=Path.Combine(AppDomain.CurrentDomain.BaseDirectory,"test-data",Guid.NewGuid().ToString("N"));Directory.CreateDirectory(root);
            using(var service=new ShopService(root,true))
            {
                service.Connect();var permanent=service.Snapshot.Items.First(i=>i.Id==1120022);var rental=service.Snapshot.Items.First(i=>i.TimeLimit==1);
                var a=Model.Create(service.Catalog,service.Snapshot,permanent);service.Catalog.products.Add(a);var b=Model.Create(service.Catalog,service.Snapshot,rental);b.currency="MB";service.Catalog.products.Add(b);
                Check(a.product_id!=b.product_id&&a.item_id!=b.item_id,"new products reserve distinct identities");Check(a.label=="永久"&&b.label=="租赁 7 天","numeric lease types produce Chinese labels");
                var invalid=Model.Clone(service.Catalog);invalid.products[0].price=0;Refuses(()=>Model.Validate(invalid),"zero price rejected before native buy popup");
                invalid=Model.Clone(service.Catalog);invalid.products[0].validity=7;Refuses(()=>Model.Validate(invalid,service.Snapshot),"permanent unit cannot become a seven-day sale");
                invalid=Model.Clone(service.Catalog);invalid.products[1].item_id=invalid.products[0].item_id;Refuses(()=>Model.Validate(invalid),"duplicate SKU refused");
                service.SaveDraft();Check(Model.Read<Catalog>(service.CatalogFile).products[1].label=="租赁 7 天","draft JSON round trip preserves Unicode and schema");
                service.Apply();Check(service.Catalog.products.All(p=>service.Status(p)=="已上架"),"applied states distinguish draft from live definition");
                a.price=600;Check(service.Status(a)=="待应用修改","price edit is pending until apply");service.Apply();a.enabled=false;service.Apply();Check(service.Status(a)=="已下架","disable is reversible and distinct from deletion");
                a.retired=true;service.Apply();Check(service.Status(a)=="已删除 / 保留编号","delete retires rather than reusing identity");
                var c=Model.Create(service.Catalog,service.Snapshot,permanent);Check(c.product_id!=a.product_id&&c.item_id!=a.item_id,"retired IDs remain reserved");
                string export=Path.Combine(root,"export.json");Model.Save(export,service.Catalog);Check(!File.ReadAllText(export).Contains("WorldDirectory"),"portable export contains no local connection settings");
                var imported=Model.Clone(service.Catalog);imported.products.RemoveAt(1);Model.Save(export,imported);service.Import(export);Check(service.Catalog.products.First(p=>p.key==b.key).retired,"import omission retires old products while reserving identity");
                imported=Model.Clone(service.Catalog);imported.products[0].item_id="199999";Model.Save(export,imported);Refuses(()=>service.Import(export),"import cannot change an existing immutable SKU");
                Refuses(()=>{using(var duplicate=new ShopService(root,true)){}},"second writer window cannot share this state directory");
                File.AppendAllText(service.CatalogFile," ");Refuses(()=>service.SaveDraft(),"external draft edits are not silently overwritten");
            }
            if(args.Length==2) Sql(root,args[0],args[1]);
            Console.WriteLine("SHOP OWNER UI SERVICE TESTS PASS: "+checks);return 0;
        }
        catch(Exception e){Console.Error.WriteLine(e.Message);return 1;}
    }
    static void Sql(string root,string privateIni,string helper)
    {
        // Explicit opt-in: this INI must be the restricted isolated test login.
        string ini=File.ReadAllText(privateIni);if(!ini.Contains("sdgo_shop_test_"))throw new Exception("Only isolated shop fixture credentials accepted");
        string world=Path.Combine(root,"world"),state=Path.Combine(root,"sql-state"),mod=Path.Combine(world,"sdgo","mods","shop_catalog");Directory.CreateDirectory(mod);
        File.Copy(privateIni,Path.Combine(world,"WorldServer.ini"));File.Copy(helper,Path.Combine(mod,"shop_catalog_apply.exe"));
        File.WriteAllText(Path.Combine(mod,"mod.ini"),"[mod]\r\nenabled=0\r\n");
        using(var service=new ShopService(state,false))
        {
            service.Settings.WorldDirectory=world;service.Catalog.catalog_id="ui-sql-"+Guid.NewGuid().ToString("N");service.Connect();
            var a=Model.Create(service.Catalog,service.Snapshot,service.Snapshot.Items.First(i=>i.Id==1120022));a.name="UI TEST GB";a.price=501;service.Catalog.products.Add(a);
            var b=Model.Create(service.Catalog,service.Snapshot,service.Snapshot.Items.First(i=>i.Id==1110549));b.name="UI TEST MB RENTAL";b.price=502;b.currency="MB";service.Catalog.products.Add(b);
            service.Apply();Check(service.Applied.Count==2&&service.Catalog.products.All(p=>service.Status(p)=="已上架"),"UI service applies new offers through actual native engine to isolated SQL");
            Check(File.ReadAllText(Path.Combine(mod,"mod.ini")).Contains("enabled=1"),"first explicit GUI apply enables configured startup module");
            a.name="UI TEST renamed";a.price=601;b.enabled=false;service.Apply();Check(service.Status(a)=="已上架"&&service.Status(b)=="已下架","UI edit and unlist read back exact native fields");
            a.retired=true;a.enabled=false;b.retired=true;service.Apply();Check(service.Applied.All(p=>!p.Visible),"UI delete hides only managed offers");
            Check(File.ReadAllText(Path.Combine(mod,"runtime-display.ini")).Contains("[shop]"),"UI publishes generated server display profile");
        }
    }
}
