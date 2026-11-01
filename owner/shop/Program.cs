using System;
using System.IO;
using System.Windows.Forms;

namespace Sdgo.Shop
{
    public static class Program
    {
        [STAThread] public static int Main(string[] args)
        {
            Application.EnableVisualStyles();Application.SetCompatibleTextRenderingDefault(false);
            bool demo=false;string state=null;
            for(int i=0;i<args.Length;i++) {if(args[i]=="--demo")demo=true;else if(args[i]=="--state"&&i+1<args.Length)state=args[++i];else return 2;}
            if(state==null) state=!demo&&File.Exists(Path.Combine(AppDomain.CurrentDomain.BaseDirectory,"shop.json"))?AppDomain.CurrentDomain.BaseDirectory:Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"SDGO",demo?"ShopCatalogDemo":"ShopCatalog");
            try{using(var service=new ShopService(state,demo)) Application.Run(new MainForm(service));return 0;}
            catch(Exception e){MessageBox.Show(e.Message,"SDGO 商城管理",MessageBoxButtons.OK,MessageBoxIcon.Error);return 1;}
        }
    }
}
