using System;
using System.Collections.Generic;
using System.Data.SqlClient;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;

namespace Sdgo.Shop
{
    public sealed class Applied
    {
        public int Id,Category,Price,Validity;
        public string Sku,Currency,Label,Name;
        public bool Visible;
        public bool Matches(Product p) { return Id==p.product_id&&Sku==p.item_id&&Category==p.category_id&&Price==p.price&&Validity==p.validity&&Currency==p.currency&&Visible==(p.enabled&&!p.retired)&&Label==p.label&&Name==p.name; }
    }
    public interface IRepository
    {
        Snapshot Read();
        List<Applied> ReadApplied(string owner);
    }
    public sealed class SqlRepository : IRepository
    {
        readonly string ini;
        [DllImport("kernel32.dll",CharSet=CharSet.Unicode)] static extern uint GetPrivateProfileString(string section,string key,string def,StringBuilder text,uint capacity,string file);
        public SqlRepository(string world) { ini=Path.GetFullPath(Path.Combine(world,"WorldServer.ini")); if(!File.Exists(ini)) throw new IOException("所选目录缺少 WorldServer.ini。"); }
        string Value(string section,string key) { var b=new StringBuilder(2048);GetPrivateProfileString(section,key,"",b,2048,ini);return b.ToString(); }
        SqlConnection Connect(string section)
        {
            var b=new SqlConnectionStringBuilder {DataSource=Value(section,"DBServerName"),InitialCatalog=Value(section,"DBName"),UserID=Value(section,"ID"),Password=Value(section,"PWD"),ConnectTimeout=5,ApplicationName="SDGO Shop Manager"};
            var c=new SqlConnection(b.ConnectionString);
            try {c.Open();return c;} catch {c.Dispose();throw new InvalidOperationException("数据库连接失败，请核对所选 WorldServer 的本机连接设置。");}
        }
        static SqlCommand Query(SqlConnection c,string sql) { return new SqlCommand(sql,c) {CommandTimeout=20}; }
        public Snapshot Read()
        {
            var result=new Snapshot();
            using(var game=Connect("DB Game"))
            using(var cmd=Query(game,"SELECT c.CatalogueID,c.Name,c.RefIndex,c.TimeLimit,ISNULL(u.UnitGradeNumber,0) FROM dbo.tbl_ItemCategory c LEFT JOIN dbo.tbl_Units u ON c.CatalogueID/100000=11 AND u.UnitNumber=c.RefIndex WHERE c.TimeLimit IN(0,1,2) AND (c.CatalogueID/100000<>11 OR (u.SallyReady=1 AND c.TimeLimit IN(0,1)))"))
            using(var r=cmd.ExecuteReader()) while(r.Read()) result.Items.Add(new Item {Id=Convert.ToInt32(r[0]),Name=Convert.ToString(r[1]).Split('\0')[0],RefIndex=Convert.ToInt32(r[2]),TimeLimit=Convert.ToInt32(r[3]),Grade=Convert.ToInt32(r[4])});
            using(var billing=Connect("DB StoreBilling"))
            {
                using(var cmd=Query(billing,"SELECT p.f_productid,p.f_itemid,p.f_depth1,p.f_display,s.f_categoryNumber FROM dbo.tbls_StoreItems p JOIN dbo.tbls_SubItems s ON p.f_itemid=s.f_itemid WHERE ISNULL(p.f_package,'0')='0' AND p.f_depth1<>20"))
                using(var r=cmd.ExecuteReader()) while(r.Read()) result.Templates.Add(new Template {Id=Convert.ToInt32(r[0]),Sku=Convert.ToString(r[1]).Trim(),Tab=Convert.ToInt32(r[2]),Visible=Convert.ToString(r[3]).Trim()=="1",Kind=Convert.ToInt32(r[4])/100000});
                using(var cmd=Query(billing,"SELECT f_productid,f_itemid FROM dbo.tbls_StoreItems"))
                using(var r=cmd.ExecuteReader()) while(r.Read()) {result.ProductIds.Add(Convert.ToInt32(r[0]));result.Skus.Add(Convert.ToString(r[1]).Trim());}
                using(var cmd=Query(billing,"SELECT DISTINCT f_itemid FROM dbo.tbls_SubItems"))
                using(var r=cmd.ExecuteReader()) while(r.Read()) result.Skus.Add(Convert.ToString(r[0]).Trim());
            }
            result.Items=result.Items.Where(i=>result.Templates.Any(t=>t.Kind==i.Kind)).OrderBy(i=>i.Kind).ThenBy(i=>i.Name).ThenBy(i=>i.Id).ToList();return result;
        }
        public List<Applied> ReadApplied(string owner)
        {
            var values=new List<Applied>();
            using(var c=Connect("DB StoreBilling"))
            {
                using(var cmd=Query(c,"SELECT OBJECT_ID(N'dbo.sdgo_ShopCatalog',N'U')")) if(cmd.ExecuteScalar()==DBNull.Value) return values;
                using(var cmd=Query(c,"SELECT p.f_productid,p.f_itemid,s.f_categoryNumber,s.f_price,s.f_validity,s.f_payment,p.f_display,s.f_itemNm,p.f_itemNm FROM dbo.sdgo_ShopCatalog o JOIN dbo.tbls_StoreItems p ON p.f_productid=o.ProductId AND p.f_itemid=o.ItemId JOIN dbo.tbls_SubItems s ON s.f_itemtblidx=o.OptionId AND s.f_itemid=o.ItemId WHERE o.CatalogId=@owner"))
                {
                    cmd.Parameters.AddWithValue("@owner",owner);
                    using(var r=cmd.ExecuteReader()) while(r.Read()) values.Add(new Applied {Id=Convert.ToInt32(r[0]),Sku=Convert.ToString(r[1]).Trim(),Category=Convert.ToInt32(r[2]),Price=Convert.ToInt32(r[3]),Validity=Convert.ToInt32(r[4]),Currency=Convert.ToString(r[5]).Trim()=="1"?"GB":"MB",Visible=Convert.ToString(r[6]).Trim()=="1",Label=Convert.ToString(r[7]),Name=Convert.ToString(r[8])});
                }
            }
            return values;
        }
    }
    public sealed class DemoRepository : IRepository
    {
        public List<Applied> Applied=new List<Applied>();
        public Snapshot Read()
        {
            return new Snapshot {Items=new List<Item> {new Item{Id=1120022,Name="Zaku II (Sale)",TimeLimit=0},new Item{Id=1110549,Name="DUAL GUNDAM ASSAULT SHROUD + BUSTER GUNDAM",TimeLimit=1},new Item{Id=1120001,Name="Gundam",TimeLimit=0}},Templates=new List<Template> {new Template{Id=1010001,Sku="100001",Tab=1,Kind=11,Visible=true}},ProductIds=new HashSet<int>{1010001},Skus=new HashSet<string>{"100001"}};
        }
        public List<Applied> ReadApplied(string owner) {return Applied;}
    }
}
