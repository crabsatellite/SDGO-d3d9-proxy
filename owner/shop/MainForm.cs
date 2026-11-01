using System;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace Sdgo.Shop
{
    public sealed class ProductRow
    {
        public Product Product {get;set;}
        public string Name {get;set;}
        public string Item {get;set;}
        public string Currency {get;set;}
        public int Price {get;set;}
        public string Label {get;set;}
        public string State {get;set;}
        public string Sync {get;set;}
    }
    public sealed class MainForm : Form
    {
        public readonly ShopService Service;
        readonly TextBox world,client,search;
        readonly Label summary,status;
        readonly DataGridView grid;
        readonly ComboBox filter;
        readonly Panel body;
        readonly FlowLayoutPanel connections;
        bool busy,dirty;
        public MainForm(ShopService service)
        {
            Service=service;Ui.Configure(this,service.Demo?"SDGO 商城管理 · 演示数据":"SDGO 商城管理",1330,850);
            var layout=new TableLayoutPanel {Dock=DockStyle.Fill,Padding=new Padding(20),ColumnCount=1,RowCount=5};layout.RowStyles.Add(new RowStyle(SizeType.Absolute,58));layout.RowStyles.Add(new RowStyle(SizeType.Absolute,108));layout.RowStyles.Add(new RowStyle(SizeType.Absolute,48));layout.RowStyles.Add(new RowStyle(SizeType.Percent,100));layout.RowStyles.Add(new RowStyle(SizeType.Absolute,48));Controls.Add(layout);
            var title=Ui.Label("商城商品管理");title.Font=new Font(Font.FontFamily,20,FontStyle.Bold);summary=Ui.Label("连接本服后选择商品、设置价格和租期。");layout.Controls.Add(Ui.Flow(title,summary));
            connections=new FlowLayoutPanel {Dock=DockStyle.Fill,WrapContents=false,FlowDirection=FlowDirection.TopDown};layout.Controls.Add(connections);
            world=new TextBox {Name="worldDirectory",ReadOnly=true,Width=650,Text=service.Settings.WorldDirectory??"",Margin=new Padding(3,8,6,3)};var chooseWorld=Ui.Button("选择服务端…","chooseWorld");var connect=Ui.Button("连接 / 刷新","connectShop",true);
            client=new TextBox {Name="clientDirectory",ReadOnly=true,Width=650,Text=service.Settings.ClientDirectory??"",Margin=new Padding(3,8,6,3)};var chooseClient=Ui.Button("选择客户端…","chooseClient");var clearClient=Ui.Button("仅导出配置","clearClient");
            connections.Controls.Add(Ui.Flow(Ui.Label("WorldServer"),world,chooseWorld,connect));connections.Controls.Add(Ui.Flow(Ui.Label("本机客户端  "),client,chooseClient,clearClient));
            search=new TextBox {Name="productSearch",Width=315,Margin=new Padding(3,8,6,3)};filter=new ComboBox {Name="productStateFilter",DropDownStyle=ComboBoxStyle.DropDownList,Width=155,Margin=new Padding(3,7,10,3)};filter.Items.AddRange(new object[]{"未删除商品","所有商品","已上架草稿","已下架草稿","已删除"});filter.SelectedIndex=0;
            var add=Ui.Button("新增商品…","addProduct",true);var edit=Ui.Button("编辑…","editProduct");var toggle=Ui.Button("上架 / 下架","toggleProduct");var remove=Ui.Button("删除…","deleteProduct");var restore=Ui.Button("恢复草稿","restoreProduct");
            layout.Controls.Add(Ui.Flow(Ui.Label("搜索"),search,filter,add,edit,toggle,remove,restore));
            body=new Panel {Dock=DockStyle.Fill};layout.Controls.Add(body);var content=new TableLayoutPanel {Dock=DockStyle.Fill,ColumnCount=1,RowCount=2};content.RowStyles.Add(new RowStyle(SizeType.Percent,100));content.RowStyles.Add(new RowStyle(SizeType.Absolute,66));body.Controls.Add(content);
            grid=Ui.Grid("shopProducts");Ui.Column(grid,"Name","商品名称",200,true);Ui.Column(grid,"Item","出售物品",260);Ui.Column(grid,"Currency","币种",65);Ui.Column(grid,"Price","价格",90);Ui.Column(grid,"Label","选项 / 租期",130);Ui.Column(grid,"State","草稿状态",90);Ui.Column(grid,"Sync","服务端状态",150);content.Controls.Add(grid);
            var save=Ui.Button("保存草稿","saveShopDraft");var apply=Ui.Button("应用到商城…","applyShop",true);var import=Ui.Button("导入商品配置…","importShop");var export=Ui.Button("导出商品配置…","exportShop");var player=Ui.Button("导出玩家显示配置…","exportPlayerProfile");
            content.Controls.Add(Ui.Flow(save,apply,import,export,player));status=new Label {Name="shopStatus",Dock=DockStyle.Fill,Text="草稿保存在本机。只有“应用到商城”会修改服务端自有商品。",Padding=new Padding(4,12,0,0),AutoEllipsis=true};layout.Controls.Add(status);
            chooseWorld.Click+=delegate{Choose(world,"选择包含 WorldServer.ini 的服务端目录");};chooseClient.Click+=delegate{Choose(client,"选择已安装显示补丁的客户端目录");};clearClient.Click+=delegate{client.Text="";};
            connect.Click+=async delegate{await Connect();};search.TextChanged+=delegate{Render();};filter.SelectedIndexChanged+=delegate{Render();};
            add.Click+=delegate{NewProduct();};edit.Click+=delegate{Edit();};grid.CellDoubleClick+=delegate(object s,DataGridViewCellEventArgs e){if(e.RowIndex>=0)Edit();};
            toggle.Click+=delegate{var p=Selected();if(p!=null&&!p.retired){p.enabled=!p.enabled;Changed();}};
            remove.Click+=delegate{var p=Selected();if(p!=null&&MessageBox.Show(this,"删除“"+p.name+"”？\r\n应用到商城后下架，保留编号、购买历史和玩家已买物品。","删除商品",MessageBoxButtons.OKCancel,MessageBoxIcon.Question)==DialogResult.OK){p.retired=true;p.enabled=false;Changed();}};
            restore.Click+=delegate{var p=Selected();if(p!=null&&p.retired){p.retired=false;p.enabled=false;Changed();}};
            save.Click+=delegate{try{Service.SaveDraft();dirty=false;status.Text="草稿已保存；服务端商品尚未改变。";Render();}catch(Exception e){Ui.Error(this,e);}};
            apply.Click+=async delegate{await Apply();};import.Click+=delegate{Import();};export.Click+=delegate{Export();};player.Click+=delegate{ExportPlayer();};
            Shown+=async delegate{if(Service.Demo||!String.IsNullOrEmpty(world.Text))await Connect();};FormClosing+=OnClosing;Render();
        }
        void Choose(TextBox box,string title) {using(var d=new FolderBrowserDialog {Description=title,SelectedPath=box.Text})if(d.ShowDialog(this)==DialogResult.OK)box.Text=d.SelectedPath;}
        async Task Connect()
        {
            if(busy)return;Service.Settings.WorldDirectory=world.Text;Service.Settings.ClientDirectory=client.Text;
            await Run(delegate{Service.Connect();},Service.Demo?"已加载演示物品，不连接真实区服。":"已连接本服；读取了物品与本配置的服务端状态。");
        }
        async Task<bool> Run(Action action,string success)
        {
            busy=true;body.Enabled=false;connections.Enabled=false;status.Text="正在处理…";EnabledToolbar(false);
            try{await Task.Run(action);status.Text=success;return true;}catch(Exception e){status.Text="操作未完成："+e.Message;Ui.Error(this,e);return false;}finally{busy=false;body.Enabled=true;connections.Enabled=true;EnabledToolbar(true);Render();}
        }
        void EnabledToolbar(bool enabled) {foreach(Control c in Controls[0].Controls[2].Controls)c.Enabled=enabled;}
        Product Selected() {return grid.CurrentRow==null?null:((ProductRow)grid.CurrentRow.DataBoundItem).Product;}
        bool Ready() {if(Service.Snapshot!=null&&(Service.Demo||String.Equals(Service.Settings.WorldDirectory,world.Text,StringComparison.OrdinalIgnoreCase)))return true;MessageBox.Show(this,"请先为当前选择的服务端点击“连接 / 刷新”。","商城管理");return false;}
        void NewProduct()
        {
            if(!Ready())return;
            using(var picker=new ItemPicker(Service.Snapshot))if(picker.ShowDialog(this)==DialogResult.OK)
            try{using(var editor=new ProductEditor(Service,Model.Create(Service.Catalog,Service.Snapshot,picker.Selected)))if(editor.ShowDialog(this)==DialogResult.OK){Service.Catalog.products.Add(editor.Result);Changed();}}catch(Exception e){Ui.Error(this,e);}
        }
        void Edit()
        {
            if(!Ready())return;var p=Selected();if(p==null)return;
            if(p.retired){MessageBox.Show(this,"先选择“恢复草稿”，再编辑商品。","商城管理");return;}
            try{using(var editor=new ProductEditor(Service,p))if(editor.ShowDialog(this)==DialogResult.OK){Service.Catalog.products[Service.Catalog.products.FindIndex(x=>x.key==p.key)]=editor.Result;Changed();}}catch(Exception e){Ui.Error(this,e);}
        }
        void Changed() {dirty=true;status.Text="草稿有修改。保存草稿仅写本机；应用到商城才会生效。";Render();}
        void Render()
        {
            string q=search.Text.Trim();string key=Selected()==null?null:Selected().key;
            var products=Service.Catalog.products.Where(p=>(q.Length==0||p.name.IndexOf(q,StringComparison.OrdinalIgnoreCase)>=0||p.key.IndexOf(q,StringComparison.OrdinalIgnoreCase)>=0||p.category_id.ToString().Contains(q))&&(filter.SelectedIndex==1||filter.SelectedIndex==0&&!p.retired||filter.SelectedIndex==2&&!p.retired&&p.enabled||filter.SelectedIndex==3&&!p.retired&&!p.enabled||filter.SelectedIndex==4&&p.retired));
            grid.DataSource=products.Select(p=>new ProductRow {Product=p,Name=p.name,Item=Service.Snapshot==null?p.category_id.ToString():(Service.Snapshot.Items.FirstOrDefault(i=>i.Id==p.category_id)??new Item {Name="未知类别 "+p.category_id}).Name,Currency=p.currency,Price=p.price,Label=p.label,State=p.retired?"已删除":p.enabled?"上架":"下架",Sync=Service.Snapshot==null?"尚未连接":Service.Status(p)}).ToList();
            if(key!=null)foreach(DataGridViewRow row in grid.Rows)if(((ProductRow)row.DataBoundItem).Product.key==key){grid.CurrentCell=row.Cells[0];break;}
            summary.Text=String.Format("{0} 件商品 · {1} 件设为上架 · {2}{3}",Service.Catalog.products.Count(p=>!p.retired),Service.Catalog.products.Count(p=>!p.retired&&p.enabled),Service.Catalog.catalog_id,dirty?" · 草稿未保存":"");
        }
        async Task Apply()
        {
            if(!Ready()||busy)return;Service.Settings.ClientDirectory=client.Text;
            try{Model.Validate(Service.Catalog,Service.Snapshot);}catch(Exception e){Ui.Error(this,e);return;}
            string rows=String.Join("\r\n",Service.Catalog.products.Where(p=>Service.Status(p)!="已上架"&&Service.Status(p)!="已下架").Take(12).Select(p=>p.name+"  "+p.price.ToString("N0")+" "+p.currency+"  "+p.label+"  "+(p.retired?"删除":p.enabled?"上架":"下架")));
            if(MessageBox.Show(this,"将配置应用到：\r\n"+(Service.Demo?"演示数据":world.Text)+"\r\n\r\n"+rows+"\r\n\r\n原版 / 其他模块商品保留。不会购买、发钱或修改玩家仓库。","确认应用到商城",MessageBoxButtons.OKCancel,MessageBoxIcon.Question)!=DialogResult.OK)return;
            if(await Run(delegate{Service.Apply();},Service.Demo?"演示应用完成；没有连接真实数据库。":"服务端配置已应用。重新打开游戏商城查看；新玩家显示配置在客户端下次启动加载。"))dirty=false;Render();
        }
        void Import()
        {
            using(var d=new OpenFileDialog {Filter="商品配置|*.json",Title="导入商品配置"})if(d.ShowDialog(this)==DialogResult.OK)
            try{Service.Import(d.FileName);Changed();}catch(Exception e){Ui.Error(this,e);}
        }
        void Export()
        {
            using(var d=new SaveFileDialog {Filter="商品配置|*.json",FileName="shop.json",Title="导出不含数据库设置的商品配置"})if(d.ShowDialog(this)==DialogResult.OK)
            try{Model.Validate(Service.Catalog);Model.Save(d.FileName,Service.Catalog);status.Text="商品配置已导出，不含本机路径或数据库密码。";}catch(Exception e){Ui.Error(this,e);}
        }
        void ExportPlayer()
        {
            string source=Path.Combine(Service.ModDirectory,"runtime-display.ini");if(Service.Demo||!File.Exists(source)){MessageBox.Show(this,"先成功应用到真实商城，再导出服务端生成的玩家显示配置。","商城管理");return;}
            using(var d=new SaveFileDialog {Filter="玩家显示配置|*.ini",FileName="display.ini",Title="导出已应用的玩家显示配置"})if(d.ShowDialog(this)==DialogResult.OK)
            try{Model.Atomic(d.FileName,File.ReadAllBytes(source));status.Text="已导出上次成功应用的玩家显示配置；未应用的草稿不在其中。";}catch(Exception e){Ui.Error(this,e);}
        }
        void OnClosing(object sender,FormClosingEventArgs e)
        {
            if(busy){e.Cancel=true;return;}if(!dirty)return;
            var answer=MessageBox.Show(this,"保存本机草稿后退出？服务端不会自动应用。","未保存的草稿",MessageBoxButtons.YesNoCancel,MessageBoxIcon.Question);
            if(answer==DialogResult.Cancel){e.Cancel=true;return;}if(answer==DialogResult.Yes)try{Service.SaveDraft();}catch(Exception x){Ui.Error(this,x);e.Cancel=true;}
        }
    }
}
