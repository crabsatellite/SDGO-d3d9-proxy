using System;
using System.Drawing;
using System.Linq;
using System.Windows.Forms;

namespace Sdgo.Shop
{
    // Same native Windows controls, palette and typography as the currency owner tool.
    public static class Ui
    {
        public static readonly Color Ink=Color.FromArgb(29,42,63),Blue=Color.FromArgb(38,99,205);
        public static Label Label(string text) {return new Label {Text=text,AutoSize=true,ForeColor=Ink,Margin=new Padding(4,9,6,5)};}
        public static Button Button(string text,string name,bool primary=false) {return new Button {Text=text,Name=name,AccessibleName=text,AutoSize=true,MinimumSize=new Size(88,34),Margin=new Padding(4),FlatStyle=FlatStyle.Flat,BackColor=primary?Blue:Color.White,ForeColor=primary?Color.White:Ink,Padding=new Padding(8,2,8,2)};}
        public static FlowLayoutPanel Flow(params Control[] controls) {var p=new FlowLayoutPanel {AutoSize=true,Dock=DockStyle.Fill,WrapContents=true,Margin=new Padding(0)};p.Controls.AddRange(controls);return p;}
        public static void Configure(Form f,string title,int width,int height) {f.Text=title;f.Font=new Font("Microsoft YaHei UI",9F);f.ForeColor=Ink;f.BackColor=Color.FromArgb(244,247,251);f.StartPosition=FormStartPosition.CenterScreen;f.Size=new Size(width,height);f.MinimumSize=new Size(Math.Min(width,1040),Math.Min(height,650));f.AutoScaleMode=AutoScaleMode.Font;}
        public static DataGridView Grid(string name)
        {
            var g=new DataGridView {Name=name,Dock=DockStyle.Fill,AutoGenerateColumns=false,AllowUserToAddRows=false,AllowUserToDeleteRows=false,RowHeadersVisible=false,ReadOnly=true,SelectionMode=DataGridViewSelectionMode.FullRowSelect,MultiSelect=false,BackgroundColor=Color.White,BorderStyle=BorderStyle.FixedSingle,RowTemplate={Height=32},ColumnHeadersHeight=37,EnableHeadersVisualStyles=false};
            g.ColumnHeadersDefaultCellStyle.BackColor=Color.FromArgb(234,240,249);g.ColumnHeadersDefaultCellStyle.ForeColor=Ink;g.AlternatingRowsDefaultCellStyle.BackColor=Color.FromArgb(248,250,253);return g;
        }
        public static void Column(DataGridView g,string property,string label,int width,bool fill=false) {g.Columns.Add(new DataGridViewTextBoxColumn {Name=property,DataPropertyName=property,HeaderText=label,Width=width,AutoSizeMode=fill?DataGridViewAutoSizeColumnMode.Fill:DataGridViewAutoSizeColumnMode.None});}
        public static void Error(IWin32Window owner,Exception e) {MessageBox.Show(owner,e.Message,"商城管理",MessageBoxButtons.OK,MessageBoxIcon.Error);}
    }
    public sealed class ItemRow
    {
        public Item Item {get;set;}
        public string Name {get{return Item.Name;}}
        public string Type {get{return Item.KindName;}}
        public string Term {get{return Item.Mode;}}
        public int Id {get{return Item.Id;}}
    }
    public sealed class ItemPicker : Form
    {
        public Item Selected;
        readonly DataGridView grid;
        readonly TextBox search;
        readonly ComboBox filter;
        readonly Snapshot snapshot;
        readonly int kind;
        public ItemPicker(Snapshot data,int requiredKind=0)
        {
            snapshot=data;kind=requiredKind;Ui.Configure(this,"选择出售的物品",1000,690);MinimumSize=new Size(780,530);
            var p=new TableLayoutPanel {Dock=DockStyle.Fill,Padding=new Padding(18),ColumnCount=1,RowCount=3};p.RowStyles.Add(new RowStyle(SizeType.Absolute,48));p.RowStyles.Add(new RowStyle(SizeType.Percent,100));p.RowStyles.Add(new RowStyle(SizeType.Absolute,48));Controls.Add(p);
            search=new TextBox {Name="itemSearch",Width=350,Margin=new Padding(3,8,9,3)};
            filter=new ComboBox {Name="itemModeFilter",DropDownStyle=ComboBoxStyle.DropDownList,Width=165,Margin=new Padding(3,7,5,3)};filter.Items.AddRange(new object[]{"全部销售类型","永久机体","租赁 / 有效天数","数量物品"});filter.SelectedIndex=0;
            p.Controls.Add(Ui.Flow(Ui.Label("搜索名称 / 编号"),search,filter));
            grid=Ui.Grid("itemChoices");Ui.Column(grid,"Name","物品名称",370,true);Ui.Column(grid,"Type","类别",120);Ui.Column(grid,"Term","销售类型",155);Ui.Column(grid,"Id","类别编号",110);p.Controls.Add(grid);
            var ok=Ui.Button("选择物品","chooseItem",true);var cancel=Ui.Button("取消","cancelItem");cancel.DialogResult=DialogResult.Cancel;p.Controls.Add(Ui.Flow(cancel,ok));CancelButton=cancel;
            search.TextChanged+=delegate{Render();};filter.SelectedIndexChanged+=delegate{Render();};ok.Click+=delegate{Choose();};grid.CellDoubleClick+=delegate(object s,DataGridViewCellEventArgs e){if(e.RowIndex>=0) Choose();};Render();
        }
        void Render()
        {
            string q=search.Text.Trim();grid.DataSource=snapshot.Items.Where(i=>(kind==0||kind==i.Kind)&&(q.Length==0||i.Name.IndexOf(q,StringComparison.OrdinalIgnoreCase)>=0||i.Id.ToString().Contains(q))&&(filter.SelectedIndex==0||filter.SelectedIndex==1&&i.Kind==11&&i.TimeLimit==0||filter.SelectedIndex==2&&i.TimeLimit==1||filter.SelectedIndex==3&&i.Kind!=11&&i.TimeLimit!=1)).Take(1000).Select(i=>new ItemRow {Item=i}).ToList();
        }
        void Choose() {if(grid.CurrentRow==null)return;Selected=((ItemRow)grid.CurrentRow.DataBoundItem).Item;DialogResult=DialogResult.OK;Close();}
    }
    public sealed class ProductEditor : Form
    {
        public Product Result;
        readonly ShopService service;
        readonly Product draft;
        Item item;
        readonly TextBox name,label,itemText;
        readonly NumericUpDown price,validity;
        readonly ComboBox currency;
        readonly CheckBox enabled;
        readonly Label validityLabel;
        bool changing;
        public ProductEditor(ShopService owner,Product product)
        {
            service=owner;draft=Model.Clone(product);item=service.Snapshot.Items.First(i=>i.Id==draft.category_id);
            Ui.Configure(this,"编辑商城商品",830,650);MinimumSize=new Size(770,600);MaximizeBox=false;
            var p=new TableLayoutPanel {Dock=DockStyle.Fill,Padding=new Padding(24),ColumnCount=2,RowCount=9};p.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute,135));p.ColumnStyles.Add(new ColumnStyle(SizeType.Percent,100));for(int i=0;i<8;i++)p.RowStyles.Add(new RowStyle(SizeType.Absolute,i==0?80:52));p.RowStyles.Add(new RowStyle(SizeType.Percent,100));Controls.Add(p);
            itemText=new TextBox {Name="selectedItem",ReadOnly=true,Multiline=true,Height=54,Width=395,Text=item.Name};var choose=Ui.Button("更换物品…","changeItem");p.Controls.Add(Ui.Label("出售物品"),0,0);p.Controls.Add(Ui.Flow(itemText,choose),1,0);
            name=new TextBox {Name="productName",MaxLength=50,Text=draft.name,Width=510,Margin=new Padding(3,9,3,3)};Add(p,1,"商城商品名称",name);
            price=new NumericUpDown {Name="productPrice",Minimum=1,Maximum=Int32.MaxValue,Value=draft.price,ThousandsSeparator=true,Width=180,Margin=new Padding(3,7,12,3)};
            currency=new ComboBox {Name="productCurrency",DropDownStyle=ComboBoxStyle.DropDownList,Width=110,Margin=new Padding(3,7,3,3)};currency.Items.AddRange(new object[]{"GB","MB"});currency.SelectedItem=draft.currency;Add(p,2,"价格与币种",Ui.Flow(price,currency));
            validity=new NumericUpDown {Name="productValidity",Minimum=1,Maximum=1000000,Value=draft.validity,Width=180,Margin=new Padding(3,7,3,3)};validityLabel=Ui.Label("");p.Controls.Add(validityLabel,0,3);p.Controls.Add(validity,1,3);
            label=new TextBox {Name="productLabel",MaxLength=50,Text=draft.label,Width=510,Margin=new Padding(3,9,3,3)};Add(p,4,"销售选项文字",label);
            enabled=new CheckBox {Name="productEnabled",Text="应用后上架此商品",Checked=draft.enabled&&!draft.retired,AutoSize=true,Margin=new Padding(3,11,3,3)};Add(p,5,"上架状态",enabled);
            p.Controls.Add(Ui.Label("编号由工具管理，修改价格不会改变商品身份。"),1,6);
            var ok=Ui.Button("保存到草稿","saveProduct",true);var cancel=Ui.Button("取消","cancelProduct");cancel.DialogResult=DialogResult.Cancel;p.Controls.Add(Ui.Flow(cancel,ok),1,7);CancelButton=cancel;
            p.Controls.Add(Ui.Label("保存草稿后，点击主窗口“应用到商城”才会影响游戏。"),1,8);
            choose.Click+=delegate{using(var picker=new ItemPicker(service.Snapshot,item.Kind))if(picker.ShowDialog(this)==DialogResult.OK){item=picker.Selected;itemText.Text=item.Name;Mode(true);}};
            validity.ValueChanged+=delegate{if(!changing&&(item.Kind==11||item.TimeLimit==1))label.Text=item.Label((int)validity.Value);};ok.Click+=delegate{Save();};Mode(false);
        }
        static void Add(TableLayoutPanel p,int row,string title,Control c) {p.Controls.Add(Ui.Label(title),0,row);p.Controls.Add(c,1,row);}
        void Mode(bool reset)
        {
            changing=true;bool permanent=item.Kind==11&&item.TimeLimit==0;validity.Enabled=!permanent;validity.Maximum=item.TimeLimit==1?36500:1000000;
            if(permanent) validity.Value=1;else if(reset||validity.Value>validity.Maximum)validity.Value=item.TimeLimit==1?7:1;
            validityLabel.Text=item.TimeLimit==1?"租期 / 有效天数":"数量";label.ReadOnly=item.Kind==11||item.TimeLimit==1;
            if(reset||label.ReadOnly)label.Text=item.Label((int)validity.Value);changing=false;
        }
        void Save()
        {
            try {draft.category_id=item.Id;draft.name=name.Text.Trim();draft.label=label.Text.Trim();draft.price=(int)price.Value;draft.currency=Convert.ToString(currency.SelectedItem);draft.validity=(int)validity.Value;draft.enabled=enabled.Checked;draft.retired=false;
                var test=Model.Clone(service.Catalog);test.products.RemoveAll(p=>p.key==draft.key);test.products.Add(draft);Model.Validate(test,service.Snapshot);Result=draft;DialogResult=DialogResult.OK;Close();}
            catch(Exception e){Ui.Error(this,e);}
        }
    }
}
