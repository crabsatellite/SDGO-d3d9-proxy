using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;

namespace Sdgo.Shop
{
    public sealed class ShopService : IDisposable
    {
        public readonly string DirectoryPath;
        public Settings Settings;
        public Catalog Catalog;
        public Snapshot Snapshot;
        public List<Applied> Applied=new List<Applied>();
        public bool Demo;
        string savedHash;
        readonly FileStream lease;
        readonly DemoRepository demoRepository=new DemoRepository();
        public string CatalogFile { get {return Path.Combine(DirectoryPath,"shop.json");} }
        public string ModDirectory {get {return Path.Combine(Settings.WorldDirectory??"","sdgo","mods","shop_catalog");}}
        public ShopService(string directory,bool demo)
        {
            DirectoryPath=Path.GetFullPath(directory);Directory.CreateDirectory(DirectoryPath);Demo=demo;
            try {lease=new FileStream(Path.Combine(DirectoryPath,"shop-ui.lock"),FileMode.OpenOrCreate,FileAccess.ReadWrite,FileShare.None);} catch(IOException) {throw new InvalidOperationException("这个配置目录已有商城管理窗口，请使用已打开的窗口。");}
            string profile=Path.Combine(DirectoryPath,"owner-settings.json");Settings=File.Exists(profile)?Model.Read<Settings>(profile):new Settings();
            if(!File.Exists(profile))
            {
                string world=Path.GetFullPath(Path.Combine(DirectoryPath,"..","Server","WorldServer"));
                string client=Path.GetFullPath(Path.Combine(DirectoryPath,"..","..","SDGO-client"));
                if(File.Exists(Path.Combine(world,"WorldServer.ini"))) Settings.WorldDirectory=world;
                if(File.Exists(Path.Combine(client,"GOnline.exe"))) Settings.ClientDirectory=client;
            }
            Catalog=File.Exists(CatalogFile)?Model.Read<Catalog>(CatalogFile):Catalog.Empty();Model.Validate(Catalog);savedHash=Model.FileHash(CatalogFile);
        }
        public void Dispose() {lease.Dispose();}
        public void Connect()
        {
            Snapshot=null;Applied=new List<Applied>();
            IRepository repo=Demo?(IRepository)demoRepository:new SqlRepository(Settings.WorldDirectory);
            var snapshot=repo.Read();var applied=repo.ReadApplied(Catalog.catalog_id);Snapshot=snapshot;Applied=applied;
            if(!Demo) Model.Save(Path.Combine(DirectoryPath,"owner-settings.json"),Settings);
        }
        public void RefreshApplied()
        {
            if(!Demo) Applied=new SqlRepository(Settings.WorldDirectory).ReadApplied(Catalog.catalog_id);
        }
        void Unchanged() {if(Model.FileHash(CatalogFile)!=savedHash) throw new InvalidOperationException("配置文件已被其他工具修改。请关闭并重新打开窗口后再应用，避免覆盖新内容。");}
        public void SaveDraft() {Unchanged();Model.Validate(Catalog,Snapshot);Model.Save(CatalogFile,Catalog);savedHash=Model.FileHash(CatalogFile);}
        static void Probe(string dir)
        {
            Directory.CreateDirectory(dir);string test=Path.Combine(dir,".shop-write-"+Guid.NewGuid().ToString("N"));
            using(var s=new FileStream(test,FileMode.CreateNew,FileAccess.Write,FileShare.None,1,FileOptions.DeleteOnClose)) s.WriteByte(0);
        }
        public static string QuoteArg(string value)
        {
            var b=new StringBuilder("\"");int slashes=0;
            foreach(char c in value) {if(c=='\\') {slashes++;continue;} if(c=='\"') b.Append('\\',slashes*2+1);else b.Append('\\',slashes);slashes=0;b.Append(c);}
            b.Append('\\',slashes*2);return b.Append('"').ToString();
        }
        public void Apply()
        {
            Unchanged();Model.Validate(Catalog,Snapshot);
            if(Demo)
            {
                Applied=Catalog.products.Where(p=>!p.retired).Select(p=>new Applied {Id=p.product_id,Sku=p.item_id,Category=p.category_id,Price=p.price,Validity=p.validity,Currency=p.currency,Visible=p.enabled,Label=p.label,Name=p.name}).ToList();demoRepository.Applied=Applied;SaveDraft();return;
            }
            string helper=Path.Combine(ModDirectory,"shop_catalog_apply.exe");
            if(!File.Exists(helper)) throw new InvalidOperationException("所选 WorldServer 尚未安装商城 Mod，请先安装服务器组件。");
            string displayDir=String.IsNullOrEmpty(Settings.ClientDirectory)?null:Path.Combine(Settings.ClientDirectory,"sdgo","mods","shop_labels");
            if(displayDir!=null&&!File.Exists(Path.Combine(displayDir,"shop_labels.dll"))) throw new InvalidOperationException("所选客户端没有安装显示补丁，请重新选择客户端，或清空客户端设置。");
            Probe(DirectoryPath);Probe(ModDirectory);if(displayDir!=null) Probe(displayDir);
            string run=Path.Combine(DirectoryPath,"apply-history",DateTime.UtcNow.ToString("yyyyMMddTHHmmssfffffff")+"-"+Guid.NewGuid().ToString("N").Substring(0,8));Directory.CreateDirectory(run);
            string candidate=Path.Combine(run,"catalog.ini");File.WriteAllText(candidate,Model.Ini(Catalog),Encoding.Unicode);Model.Save(Path.Combine(run,"shop.json"),Catalog);
            var start=new ProcessStartInfo(helper) {UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true,StandardOutputEncoding=Encoding.UTF8,StandardErrorEncoding=Encoding.UTF8,
                Arguments="--host-ini "+QuoteArg(Path.Combine(Settings.WorldDirectory,"WorldServer.ini"))+" --catalog "+QuoteArg(candidate)+" --output "+QuoteArg(run)};
            string output,error;int exit;
            using(var process=Process.Start(start))
            {
                var stdout=process.StandardOutput.ReadToEndAsync();var stderr=process.StandardError.ReadToEndAsync();
                if(!process.WaitForExit(60000)) throw new InvalidOperationException("应用工具仍在处理，结果尚未确认。请稍后刷新服务器状态，勿重复应用。记录目录："+run);
                output=stdout.Result;error=stderr.Result;exit=process.ExitCode;
            }
            File.WriteAllText(Path.Combine(run,"result.txt"),output+error,Encoding.UTF8);
            if(exit!=0) throw new InvalidOperationException("服务器未报告完整应用成功，请检查记录目录后刷新状态："+run+"\r\n"+error);
            try
            {
                Model.Atomic(Path.Combine(ModDirectory,"catalog.ini"),File.ReadAllBytes(candidate));
                Model.Atomic(Path.Combine(ModDirectory,"runtime-display.ini"),File.ReadAllBytes(Path.Combine(run,"runtime-display.ini")));
                SaveDraft();
                if(displayDir!=null) Model.Atomic(Path.Combine(displayDir,"display.ini"),File.ReadAllBytes(Path.Combine(run,"runtime-display.ini")));
            }
            catch(Exception e) {throw new InvalidOperationException("服务器商品已应用，但本地配置/标签保存未完成。请保留并恢复记录目录："+run+"\r\n"+e.Message);}
            RefreshApplied();
        }
        public void Import(string file)
        {
            var incoming=Model.Read<Catalog>(file);Model.Validate(incoming,Snapshot);
            if(incoming.catalog_id!=Catalog.catalog_id&&(Catalog.products.Count>0||Applied.Any(a=>a.Visible))) throw new InvalidOperationException("导入配置的标识不同。当前目录已管理商品，请为另一套配置选择单独的管理目录。");
            if(incoming.catalog_id==Catalog.catalog_id)
            {
                foreach(var old in Catalog.products)
                {
                    var replacement=incoming.products.FirstOrDefault(p=>p.key==old.key);
                    if(replacement!=null&&(replacement.product_id!=old.product_id||replacement.item_id!=old.item_id||replacement.template_id!=old.template_id))
                        throw new InvalidOperationException("导入内容更改了已有商品身份，请新增商品而非复用编号。");
                    if(replacement==null) {var retired=Model.Clone(old);retired.retired=true;retired.enabled=false;incoming.products.Add(retired);}
                }
                Model.Validate(incoming,Snapshot);
            }
            Catalog=incoming;
        }
        public string Status(Product p)
        {
            var a=Applied.FirstOrDefault(x=>x.Id==p.product_id);
            if(p.retired) return a!=null&&a.Visible?"待应用删除":"已删除 / 保留编号";
            if(a==null) return "未应用";
            return a.Matches(p)?(p.enabled?"已上架":"已下架"):"待应用修改";
        }
    }
}
